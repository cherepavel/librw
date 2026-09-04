#ifdef RW_PSP

#include <limits.h>
#include <stdint.h>
#include <pspkernel.h>
#include <string.h>

#include "../rwbase.h"
#include "../rwrender.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwengine.h"
#include "rwpspimpl.h"

namespace rw {
namespace psp {

struct PspMeshInstance {
	uint32 indexOffset;
	uint32 numIndices;
	Material *material;
};

struct PspGeometryInstance {
	InstanceDataHeader header;
	uint32 serialNumber;
	uint32 numVertices;
	uint32 numIndices;
	uint32 numMeshes;
	void *allocation;
	PspGeometryVertex *vertices;
	uint16 *indices;
	PspMeshInstance meshes[1];
};

static void *
align16(void *pointer)
{
	return reinterpret_cast<void *>((reinterpret_cast<uintptr>(pointer) + 15) & ~uintptr(15));
}

void *
destroyNativeGeometry(void *object, int32, int32)
{
	Geometry *geometry = static_cast<Geometry *>(object);
	if(geometry->instData == nil || geometry->instData->platform != PLATFORM_PSP)
		return object;
	PspGeometryInstance *instance =
	    reinterpret_cast<PspGeometryInstance *>(geometry->instData);
	geometry->instData = nil;
	rwFree(instance->allocation);
	rwFree(instance);
	return object;
}

static bool32
instanceGeometry(Geometry *geometry)
{
	if(geometry == nil || geometry->numVertices <= 0 ||
	   geometry->morphTargets == nil || geometry->meshHeader == nil)
		return false;
	MeshHeader *meshHeader = geometry->meshHeader;
	if(meshHeader->numMeshes == 0 || meshHeader->totalIndices == 0 ||
	   static_cast<uint32>(geometry->numVertices) > UINT16_MAX)
		return false;

	size_t headerSize = sizeof(PspGeometryInstance) +
	    sizeof(PspMeshInstance)*(meshHeader->numMeshes - 1);
	if(headerSize < sizeof(PspGeometryInstance))
		return false;
	PspGeometryInstance *instance = static_cast<PspGeometryInstance *>(
	    rwMalloc(headerSize, MEMDUR_EVENT | ID_GEOMETRY));
	if(instance == nil)
		return false;
	memset(instance, 0, headerSize);

	size_t vertexSize = sizeof(PspGeometryVertex)*geometry->numVertices;
	size_t indexSize = sizeof(uint16)*meshHeader->totalIndices;
	if(vertexSize/sizeof(PspGeometryVertex) != static_cast<size_t>(geometry->numVertices) ||
	   indexSize/sizeof(uint16) != meshHeader->totalIndices ||
	   vertexSize > SIZE_MAX - indexSize - 31){
		rwFree(instance);
		return false;
	}
	instance->allocation = rwMalloc(vertexSize + indexSize + 31,
	    MEMDUR_EVENT | ID_GEOMETRY);
	if(instance->allocation == nil){
		rwFree(instance);
		return false;
	}
	instance->header.platform = PLATFORM_PSP;
	instance->serialNumber = meshHeader->serialNum;
	instance->numVertices = geometry->numVertices;
	instance->numIndices = meshHeader->totalIndices;
	instance->numMeshes = meshHeader->numMeshes;
	instance->vertices = static_cast<PspGeometryVertex *>(align16(instance->allocation));
	instance->indices = static_cast<uint16 *>(align16(
	    reinterpret_cast<uint8 *>(instance->vertices) + vertexSize));

	MorphTarget *morph = &geometry->morphTargets[0];
	for(int32 i = 0; i < geometry->numVertices; i++){
		PspGeometryVertex &vertex = instance->vertices[i];
		vertex.x = morph->vertices[i].x;
		vertex.y = morph->vertices[i].y;
		vertex.z = morph->vertices[i].z;
		if(morph->normals){
			vertex.nx = morph->normals[i].x;
			vertex.ny = morph->normals[i].y;
			vertex.nz = morph->normals[i].z;
		}else{
			vertex.nx = vertex.ny = 0.0f;
			vertex.nz = 1.0f;
		}
		RGBA color = geometry->colors ? geometry->colors[i] : makeRGBA(255, 255, 255, 255);
		vertex.color = color.red | color.green << 8 | color.blue << 16 | color.alpha << 24;
		if(geometry->numTexCoordSets > 0 && geometry->texCoords[0]){
			vertex.u = geometry->texCoords[0][i].u;
			vertex.v = geometry->texCoords[0][i].v;
		}else
			vertex.u = vertex.v = 0.0f;
	}

	Mesh *mesh = meshHeader->getMeshes();
	uint32 indexOffset = 0;
	for(uint32 i = 0; i < meshHeader->numMeshes; i++, mesh++){
		if(mesh->numIndices > meshHeader->totalIndices - indexOffset){
			rwFree(instance->allocation);
			rwFree(instance);
			return false;
		}
		PspMeshInstance &dst = instance->meshes[i];
		dst.indexOffset = indexOffset;
		dst.numIndices = mesh->numIndices;
		dst.material = mesh->material;
		memcpy(instance->indices + indexOffset, mesh->indices,
		    sizeof(uint16)*mesh->numIndices);
		indexOffset += mesh->numIndices;
	}
	if(indexOffset != meshHeader->totalIndices){
		rwFree(instance->allocation);
		rwFree(instance);
		return false;
	}
	sceKernelDcacheWritebackRange(instance->vertices, vertexSize);
	sceKernelDcacheWritebackRange(instance->indices, indexSize);
	geometry->instData = &instance->header;
	geometry->lockedSinceInst = 0;
	return true;
}

static void
instance(ObjPipeline*, Atomic *atomic)
{
	Geometry *geometry = atomic->geometry;
	if(geometry == nil || geometry->flags & Geometry::NATIVE)
		return;
	PspGeometryInstance *data =
	    reinterpret_cast<PspGeometryInstance *>(geometry->instData);
	if(data && (data->header.platform != PLATFORM_PSP ||
	   data->serialNumber != geometry->meshHeader->serialNum ||
	   geometry->lockedSinceInst)){
		destroyNativeGeometry(geometry, 0, 0);
		data = nil;
	}
	if(data == nil)
		instanceGeometry(geometry);
}

static void
uninstance(ObjPipeline*, Atomic *atomic)
{
	destroyNativeGeometry(atomic->geometry, 0, 0);
}

static void
render(ObjPipeline *pipeline, Atomic *atomic)
{
	Geometry *geometry = atomic->geometry;
	if(geometry == nil)
		return;
	pipeline->instance(atomic);
	PspGeometryInstance *data =
	    reinterpret_cast<PspGeometryInstance *>(geometry->instData);
	if(data == nil || data->header.platform != PLATFORM_PSP)
		return;
	PrimitiveType primitive = geometry->meshHeader->flags & MeshHeader::TRISTRIP ?
	    PRIMTYPETRISTRIP : PRIMTYPETRILIST;
	const Matrix *world = atomic->getFrame() ? atomic->getFrame()->getLTM() : nil;
	for(uint32 i = 0; i < data->numMeshes; i++){
		PspMeshInstance &mesh = data->meshes[i];
		Texture *texture = mesh.material ? mesh.material->texture : nil;
		SetRenderStatePtr(TEXTURERASTER, texture ? texture->raster : nil);
		SetRenderState(VERTEXALPHA,
		    mesh.material && mesh.material->color.alpha != 255);
		drawGeometry(world, primitive, data->vertices, data->numVertices,
		    data->indices + mesh.indexOffset, mesh.numIndices);
	}
}

ObjPipeline *
makeDefaultPipeline(void)
{
	ObjPipeline *pipeline = ObjPipeline::create();
	if(pipeline == nil)
		return nil;
	pipeline->init(PLATFORM_PSP);
	pipeline->impl.instance = instance;
	pipeline->impl.uninstance = uninstance;
	pipeline->impl.render = render;
	return pipeline;
}

} // namespace psp
} // namespace rw

#endif // RW_PSP
