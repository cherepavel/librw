#ifdef RW_PSP

#include <limits.h>
#include <stdint.h>
#include <pspkernel.h>
#include <stdio.h>
#include <string.h>

#include "../rwbase.h"
#include "../rwrender.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwanim.h"
#include "../rwengine.h"
#include "../rwplugins.h"
#include "rwpspimpl.h"

namespace rw {
namespace psp {

struct PspMeshInstance {
	uint32 indexOffset;
	uint32 numIndices;
	Material *material;
	bool32 vertexAlpha;
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
		dst.vertexAlpha = 0;
		memcpy(instance->indices + indexOffset, mesh->indices,
		    sizeof(uint16)*mesh->numIndices);
		if(geometry->colors)
			for(uint32 j = 0; j < mesh->numIndices; j++)
				if(geometry->colors[mesh->indices[j]].alpha != 255){
					dst.vertexAlpha = 1;
					break;
				}
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
renderMeshes(Atomic *atomic, PspGeometryInstance *data,
    const PspGeometryVertex *vertices)
{
	Geometry *geometry = atomic->geometry;
	PrimitiveType primitive = geometry->meshHeader->flags & MeshHeader::TRISTRIP ?
	    PRIMTYPETRISTRIP : PRIMTYPETRILIST;
	const Matrix *world = atomic->getFrame() ? atomic->getFrame()->getLTM() : nil;
	for(uint32 i = 0; i < data->numMeshes; i++){
		PspMeshInstance &mesh = data->meshes[i];
		Texture *texture = mesh.material ? mesh.material->texture : nil;
		SetRenderStatePtr(TEXTURERASTER, texture ? texture->raster : nil);
		SetRenderState(VERTEXALPHA, mesh.vertexAlpha ||
		    (mesh.material && mesh.material->color.alpha != 255));
		drawGeometry(world, primitive, vertices, data->numVertices,
		    data->indices + mesh.indexOffset, mesh.numIndices);
	}
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
	renderMeshes(atomic, data, data->vertices);
}

static void
renderSkin(ObjPipeline *pipeline, Atomic *atomic)
{
	static int32 diagnosticCount;
	Geometry *geometry = atomic->geometry;
	Skin *skin = geometry ? Skin::get(geometry) : nil;
	if(geometry == nil || skin == nil)
		return;
	pipeline->instance(atomic);
	PspGeometryInstance *data =
	    reinterpret_cast<PspGeometryInstance *>(geometry->instData);
	if(data == nil || data->header.platform != PLATFORM_PSP)
		return;
	if(diagnosticCount < 4){
		printf("PSP_SKIN_RENDER vertices=%lu bones=%ld weights=%ld hierarchy=%p\n",
		    data->numVertices, skin->numBones, skin->numWeights,
		    Skin::getHierarchy(atomic));
		diagnosticCount++;
	}

	PspGeometryVertex *vertices = allocTransientGeometryVertices(data->numVertices);
	Matrix *boneMatrices = rwNewT(Matrix, skin->numBones, MEMDUR_FUNCTION | ID_SKIN);
	if(vertices == nil || boneMatrices == nil){
		if(boneMatrices) rwFree(boneMatrices);
		return;
	}
	HAnimHierarchy *hierarchy = Skin::getHierarchy(atomic);
	Matrix *inverse = reinterpret_cast<Matrix *>(skin->inverseMatrices);
	if(hierarchy && hierarchy->numNodes == skin->numBones){
		if(hierarchy->flags & HAnimHierarchy::LOCALSPACEMATRICES){
			for(int32 i = 0; i < skin->numBones; i++){
				inverse[i].flags = 0;
				Matrix::mult(&boneMatrices[i], &inverse[i], &hierarchy->matrices[i]);
			}
		}else{
			Matrix inverseAtomic, temporary;
			Matrix::invert(&inverseAtomic, atomic->getFrame()->getLTM());
			for(int32 i = 0; i < skin->numBones; i++){
				inverse[i].flags = 0;
				Matrix::mult(&temporary, &hierarchy->matrices[i], &inverseAtomic);
				Matrix::mult(&boneMatrices[i], &inverse[i], &temporary);
			}
		}
	}else
		for(int32 i = 0; i < skin->numBones; i++)
			boneMatrices[i].setIdentity();

	for(uint32 i = 0; i < data->numVertices; i++){
		vertices[i] = data->vertices[i];
		V3d sourcePosition = { data->vertices[i].x, data->vertices[i].y, data->vertices[i].z };
		V3d sourceNormal = { data->vertices[i].nx, data->vertices[i].ny, data->vertices[i].nz };
		V3d position = { 0.0f, 0.0f, 0.0f };
		V3d normal = { 0.0f, 0.0f, 0.0f };
		for(int32 j = 0; j < skin->numWeights && j < 4; j++){
			float32 weight = skin->weights[i*4+j];
			uint8 bone = skin->indices[i*4+j];
			if(weight == 0.0f || bone >= skin->numBones)
				continue;
			V3d p, n;
			V3d::transformPoints(&p, &sourcePosition, 1, &boneMatrices[bone]);
			V3d::transformVectors(&n, &sourceNormal, 1, &boneMatrices[bone]);
			position = add(position, scale(p, weight));
			normal = add(normal, scale(n, weight));
		}
		vertices[i].x = position.x;
		vertices[i].y = position.y;
		vertices[i].z = position.z;
		vertices[i].nx = normal.x;
		vertices[i].ny = normal.y;
		vertices[i].nz = normal.z;
	}
	rwFree(boneMatrices);
	renderMeshes(atomic, data, vertices);
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

ObjPipeline *
makeSkinPipeline(void)
{
	ObjPipeline *pipeline = ObjPipeline::create();
	if(pipeline == nil)
		return nil;
	pipeline->init(PLATFORM_PSP);
	pipeline->impl.instance = instance;
	pipeline->impl.uninstance = uninstance;
	pipeline->impl.render = renderSkin;
	pipeline->pluginID = ID_SKIN;
	pipeline->pluginData = 1;
	return pipeline;
}

} // namespace psp
} // namespace rw

#endif // RW_PSP
