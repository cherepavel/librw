#ifdef RW_PSP

#include <pspdisplay.h>
#include <pspgu.h>
#include <pspgum.h>
#include <psputils.h>
#include <stdio.h>
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

enum {
	BUFFER_WIDTH = 512,
	SCREEN_WIDTH = 480,
	SCREEN_HEIGHT = 272,
	FRAMEBUFFER_SIZE = BUFFER_WIDTH*SCREEN_HEIGHT*2,
	DEPTHBUFFER_OFFSET = FRAMEBUFFER_SIZE*2,
	DISPLAY_LIST_WORDS = 64*1024
};

alignas(16) static uint32 displayList[DISPLAY_LIST_WORDS];
static bool32 guInitialized;
static bool32 listActive;
static bool32 clearPending;
static uint32 pendingClearMode;
static uint32 pendingClearColor;

struct GuIm3DVertex {
	float32 u, v;
	uint32 color;
	float32 nx, ny, nz;
	float32 x, y, z;
};

static_assert(sizeof(GuIm3DVertex) == 36, "Unexpected PSP GU Im3D vertex layout");
static GuIm3DVertex *im3DVertices;
static int32 im3DVertexCount;

PspGeometryVertex *
allocTransientGeometryVertices(int32 count)
{
	if(!listActive || count <= 0)
		return nil;
	return static_cast<PspGeometryVertex *>(
	    sceGuGetMemory(sizeof(PspGeometryVertex)*count));
}

static void
updateCameraMatrices(Camera *camera)
{
	float32 view[16];
	float32 projection[16];
	Matrix inverse;
	Matrix::invert(&inverse, camera->getFrame()->getLTM());
	view[0] = -inverse.right.x; view[1] = inverse.right.y; view[2] = inverse.right.z; view[3] = 0.0f;
	view[4] = -inverse.up.x; view[5] = inverse.up.y; view[6] = inverse.up.z; view[7] = 0.0f;
	view[8] = -inverse.at.x; view[9] = inverse.at.y; view[10] = inverse.at.z; view[11] = 0.0f;
	view[12] = -inverse.pos.x; view[13] = inverse.pos.y; view[14] = inverse.pos.z; view[15] = 1.0f;
	memcpy(&camera->devView, view, sizeof(view));

	memset(projection, 0, sizeof(projection));
	float32 inverseWindowX = 1.0f/camera->viewWindow.x;
	float32 inverseWindowY = 1.0f/camera->viewWindow.y;
	float32 inverseDepth = 1.0f/(camera->farPlane-camera->nearPlane);
	projection[0] = inverseWindowX;
	projection[5] = inverseWindowY;
	projection[8] = camera->viewOffset.x*inverseWindowX;
	projection[9] = camera->viewOffset.y*inverseWindowY;
	projection[12] = -projection[8];
	projection[13] = -projection[9];
	if(camera->projection == Camera::PERSPECTIVE){
		projection[10] = (camera->farPlane+camera->nearPlane)*inverseDepth;
		projection[11] = 1.0f;
		projection[14] = -2.0f*camera->nearPlane*camera->farPlane*inverseDepth;
	}else{
		projection[10] = 2.0f*inverseDepth;
		projection[14] = -(camera->farPlane+camera->nearPlane)*inverseDepth;
		projection[15] = 1.0f;
	}
	memcpy(&camera->devProj, projection, sizeof(projection));
}

struct GuIm2DVertex {
	float32 u, v;
	uint32 color;
	float32 x, y, z;
};

static_assert(sizeof(GuIm2DVertex) == 24, "Unexpected PSP GU Im2D vertex layout");

struct RenderStateCache {
	Raster *texture;
	uint32 addressU, addressV;
	uint32 filter;
	uint32 vertexAlpha;
	uint32 srcBlend, dstBlend;
	uint32 zTest, zWrite;
	uint32 fogEnable, fogColor;
	uint32 cullMode;
	uint32 alphaTestFunc, alphaTestRef;
};

static RenderStateCache state = {
	nil, Texture::WRAP, Texture::WRAP, Texture::LINEAR,
	0, BLENDSRCALPHA, BLENDINVSRCALPHA,
	1, 1, 0, 0, CULLBACK, ALPHAALWAYS, 0
};
static Camera *activeCamera;

static int32
guPrimitive(PrimitiveType type)
{
	switch(type){
	case PRIMTYPELINELIST: return GU_LINES;
	case PRIMTYPEPOLYLINE: return GU_LINE_STRIP;
	case PRIMTYPETRILIST: return GU_TRIANGLES;
	case PRIMTYPETRISTRIP: return GU_TRIANGLE_STRIP;
	case PRIMTYPETRIFAN: return GU_TRIANGLE_FAN;
	case PRIMTYPEPOINTLIST: return GU_POINTS;
	default: return -1;
	}
}

static int32
guBlend(uint32 blend)
{
	switch(blend){
	case BLENDZERO: return GU_FIX;
	case BLENDONE: return GU_FIX;
	case BLENDSRCCOLOR: return GU_SRC_COLOR;
	case BLENDINVSRCCOLOR: return GU_ONE_MINUS_SRC_COLOR;
	case BLENDSRCALPHA: return GU_SRC_ALPHA;
	case BLENDINVSRCALPHA: return GU_ONE_MINUS_SRC_ALPHA;
	case BLENDDESTALPHA: return GU_DST_ALPHA;
	case BLENDINVDESTALPHA: return GU_ONE_MINUS_DST_ALPHA;
	case BLENDDESTCOLOR: return GU_DST_COLOR;
	case BLENDINVDESTCOLOR: return GU_ONE_MINUS_DST_COLOR;
	case BLENDSRCALPHASAT: return GU_SRC_ALPHA;
	default: return GU_SRC_ALPHA;
	}
}

static void
applyBlend(void)
{
	uint32 srcFix = state.srcBlend == BLENDZERO ? 0 : 0xFFFFFFFF;
	uint32 dstFix = state.dstBlend == BLENDZERO ? 0 : 0xFFFFFFFF;
	sceGuBlendFunc(GU_ADD, guBlend(state.srcBlend), guBlend(state.dstBlend),
	    srcFix, dstFix);
}

static void
applyAlphaTest(void)
{
	if(state.alphaTestFunc == ALPHAALWAYS){
		sceGuDisable(GU_ALPHA_TEST);
		return;
	}
	int32 function = state.alphaTestFunc == ALPHALESS ? GU_LESS : GU_GEQUAL;
	sceGuAlphaFunc(function, state.alphaTestRef, 0xFF);
	sceGuEnable(GU_ALPHA_TEST);
}

static void
applyFog(void)
{
	if(!listActive)
		return;
	if(!state.fogEnable || activeCamera == nil){
		sceGuDisable(GU_FOG);
		return;
	}
	float32 fogNear = activeCamera->fogPlane;
	float32 fogFar = activeCamera->farPlane;
	if(fogNear < activeCamera->nearPlane)
		fogNear = activeCamera->nearPlane;
	if(fogNear >= fogFar)
		fogNear = fogFar - 0.001f;
	sceGuFog(fogNear, fogFar, state.fogColor);
	sceGuEnable(GU_FOG);
}

static void
applyTexture(bool32 replace)
{
	if(state.texture == nil){
		sceGuDisable(GU_TEXTURE_2D);
		return;
	}
	sceGuEnable(GU_TEXTURE_2D);
	NativeRaster native;
	if(!getNativeRaster(state.texture, &native)){
		sceGuDisable(GU_TEXTURE_2D);
		return;
	}
	if(native.palette){
		sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
		sceGuClutLoad(32, native.palette);
	}
	sceGuTexMode(native.pixelFormat, 0, 0, native.swizzled);
	sceGuTexImage(0, state.texture->originalWidth, state.texture->originalHeight,
	    native.bufferWidth, native.pixels);
	/* The full game switches resident TXD rasters many times per frame.  The GE
	 * texture cache is not coherent with a new system-memory image/CLUT binding;
	 * the single-texture smoke tests did not expose this. */
	sceGuTexFlush();
	sceGuTexMapMode(GU_TEXTURE_COORDS, 0, 0);
	sceGuTexProjMapMode(GU_UV);
	sceGuTexFunc(replace ? GU_TFX_REPLACE : GU_TFX_MODULATE, GU_TCC_RGBA);
	/* PSP GU consumes the normalized UVs supplied by RenderWare here.  Im2D
	 * expands its normalized UVs to texels in copyVertices instead. */
	sceGuTexScale(1.0f, 1.0f);
	sceGuTexOffset(0.0f, 0.0f);
	sceGuTexFilter(state.filter == Texture::NEAREST ? GU_NEAREST : GU_LINEAR,
	    state.filter == Texture::NEAREST ? GU_NEAREST : GU_LINEAR);
	sceGuTexWrap(state.addressU == Texture::CLAMP ? GU_CLAMP : GU_REPEAT,
	    state.addressV == Texture::CLAMP ? GU_CLAMP : GU_REPEAT);
}

static GuIm2DVertex *
copyVertices(void *vertices, int32 count)
{
	if(vertices == nil || count <= 0)
		return nil;
	Im2DVertex *source = static_cast<Im2DVertex *>(vertices);
	GuIm2DVertex *destination = static_cast<GuIm2DVertex *>(
	    sceGuGetMemory(sizeof(GuIm2DVertex)*count));
	if(destination == nil)
		return nil;
	float32 uScale = state.texture ? state.texture->originalWidth : 1.0f;
	float32 vScale = state.texture ? state.texture->originalHeight : 1.0f;
	for(int32 i = 0; i < count; i++){
		destination[i].u = source[i].u*uScale;
		destination[i].v = source[i].v*vScale;
		destination[i].color = source[i].r | source[i].g << 8 |
		    source[i].b << 16 | source[i].a << 24;
		destination[i].x = source[i].x;
		destination[i].y = source[i].y;
		destination[i].z = source[i].z;
	}
	return destination;
}

static void
drawIm2D(PrimitiveType type, void *vertices, int32 numVertices,
    void *indices, int32 numIndices)
{
	int32 primitive = guPrimitive(type);
	if(primitive < 0 || numVertices <= 0)
		return;
	GuIm2DVertex *guVertices = copyVertices(vertices, numVertices);
	if(guVertices == nil)
		return;
	applyTexture(0);
	int32 vertexType = GU_TEXTURE_32BITF | GU_COLOR_8888 |
	    GU_VERTEX_32BITF | GU_TRANSFORM_2D;
	if(indices && numIndices > 0){
		uint16 *guIndices = static_cast<uint16 *>(sceGuGetMemory(sizeof(uint16)*numIndices));
		if(guIndices == nil)
			return;
		memcpy(guIndices, indices, sizeof(uint16)*numIndices);
		sceGuDrawArray(primitive, vertexType | GU_INDEX_16BIT,
		    numIndices, guIndices, guVertices);
	}else
		sceGuDrawArray(primitive, vertexType, numVertices, nil, guVertices);
}

static void
beginUpdate(Camera *camera)
{
	if(!guInitialized || listActive)
		return;
	sceGuStart(GU_DIRECT, displayList);
	listActive = 1;
	if(clearPending){
		uint32 flags = 0;
		if(pendingClearMode & Camera::CLEARIMAGE){
			sceGuClearColor(pendingClearColor);
			flags |= GU_COLOR_BUFFER_BIT;
		}
		if(pendingClearMode & Camera::CLEARZ){
			sceGuClearDepth(0);
			flags |= GU_DEPTH_BUFFER_BIT;
		}
		if(flags)
			sceGuClear(flags);
		clearPending = 0;
	}
	if(camera){
		activeCamera = camera;
		updateCameraMatrices(camera);
		sceGumMatrixMode(GU_PROJECTION);
		sceGumLoadMatrix(reinterpret_cast<ScePspFMatrix4 *>(&camera->devProj));
		sceGumMatrixMode(GU_VIEW);
		sceGumLoadMatrix(reinterpret_cast<ScePspFMatrix4 *>(&camera->devView));
	}
	applyFog();
}

static void
endUpdate(Camera*)
{
	if(!listActive)
		return;
	sceGuFinish();
	sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
	listActive = 0;
}

static void
clearCamera(Camera*, RGBA *color, uint32 mode)
{
	pendingClearMode = mode;
	pendingClearColor = color ? color->red | color->green << 8 |
	    color->blue << 16 | color->alpha << 24 : 0;
	clearPending = 1;
}

static void
showRaster(Raster*, uint32 flags)
{
	if(listActive)
		endUpdate(nil);
	if(flags & Raster::FLIPWAITVSYNCH)
		sceDisplayWaitVblankStart();
	sceGuSwapBuffers();
}
static bool32 rasterRenderFast(Raster*, int32, int32) { return 0; }
static void
setRenderState(int32 renderState, void *pointer)
{
	uint32 value = (uint32)(uintptr)pointer;
	switch(renderState){
	case TEXTURERASTER: state.texture = static_cast<Raster *>(pointer); break;
	case TEXTUREADDRESS: state.addressU = state.addressV = value; break;
	case TEXTUREADDRESSU: state.addressU = value; break;
	case TEXTUREADDRESSV: state.addressV = value; break;
	case TEXTUREFILTER: state.filter = value; break;
	case VERTEXALPHA:
		state.vertexAlpha = value;
		if(value) sceGuEnable(GU_BLEND); else sceGuDisable(GU_BLEND);
		break;
	case SRCBLEND: state.srcBlend = value; applyBlend(); break;
	case DESTBLEND: state.dstBlend = value; applyBlend(); break;
	case ZTESTENABLE:
		state.zTest = value;
		if(value) sceGuEnable(GU_DEPTH_TEST); else sceGuDisable(GU_DEPTH_TEST);
		break;
	case ZWRITEENABLE:
		state.zWrite = value;
		sceGuDepthMask(value ? GU_FALSE : GU_TRUE);
		break;
	case FOGENABLE:
		state.fogEnable = value;
		applyFog();
		break;
	case FOGCOLOR: state.fogColor = value; applyFog(); break;
	case CULLMODE:
		state.cullMode = value;
		if(value == CULLNONE) sceGuDisable(GU_CULL_FACE);
		else { sceGuEnable(GU_CULL_FACE); sceGuFrontFace(value == CULLBACK ? GU_CCW : GU_CW); }
		break;
	case ALPHATESTFUNC: state.alphaTestFunc = value; applyAlphaTest(); break;
	case ALPHATESTREF: state.alphaTestRef = value; applyAlphaTest(); break;
	default: break;
	}
}

static void *
getRenderState(int32 renderState)
{
	switch(renderState){
	case TEXTURERASTER: return state.texture;
	case TEXTUREADDRESS: return (void*)(uintptr)(state.addressU == state.addressV ? state.addressU : 0);
	case TEXTUREADDRESSU: return (void*)(uintptr)state.addressU;
	case TEXTUREADDRESSV: return (void*)(uintptr)state.addressV;
	case TEXTUREFILTER: return (void*)(uintptr)state.filter;
	case VERTEXALPHA: return (void*)(uintptr)state.vertexAlpha;
	case SRCBLEND: return (void*)(uintptr)state.srcBlend;
	case DESTBLEND: return (void*)(uintptr)state.dstBlend;
	case ZTESTENABLE: return (void*)(uintptr)state.zTest;
	case ZWRITEENABLE: return (void*)(uintptr)state.zWrite;
	case FOGENABLE: return (void*)(uintptr)state.fogEnable;
	case FOGCOLOR: return (void*)(uintptr)state.fogColor;
	case CULLMODE: return (void*)(uintptr)state.cullMode;
	case ALPHATESTFUNC: return (void*)(uintptr)state.alphaTestFunc;
	case ALPHATESTREF: return (void*)(uintptr)state.alphaTestRef;
	default: return nil;
	}
}

static void im2DRenderLine(void *vertices, int32 numVertices, int32 first, int32 second)
{
	uint16 indices[2] = { (uint16)first, (uint16)second };
	drawIm2D(PRIMTYPELINELIST, vertices, numVertices, indices, 2);
}
static void im2DRenderTriangle(void *vertices, int32 numVertices, int32 a, int32 b, int32 c)
{
	uint16 indices[3] = { (uint16)a, (uint16)b, (uint16)c };
	drawIm2D(PRIMTYPETRILIST, vertices, numVertices, indices, 3);
}
static void im2DRenderPrimitive(PrimitiveType type, void *vertices, int32 numVertices)
{
	drawIm2D(type, vertices, numVertices, nil, 0);
}
static void im2DRenderIndexedPrimitive(PrimitiveType type, void *vertices,
    int32 numVertices, void *indices, int32 numIndices)
{
	drawIm2D(type, vertices, numVertices, indices, numIndices);
}
static void
im3DTransform(void *vertices, int32 numVertices, Matrix *world, uint32 flags)
{
	im3DVertices = nil;
	im3DVertexCount = 0;
	if(vertices == nil || numVertices <= 0 || !listActive)
		return;
	Im3DVertex *source = static_cast<Im3DVertex *>(vertices);
	im3DVertices = static_cast<GuIm3DVertex *>(
	    sceGuGetMemory(sizeof(GuIm3DVertex)*numVertices));
	if(im3DVertices == nil)
		return;
	for(int32 i = 0; i < numVertices; i++){
		im3DVertices[i].u = source[i].u;
		im3DVertices[i].v = source[i].v;
		im3DVertices[i].color = source[i].r | source[i].g << 8 |
		    source[i].b << 16 | source[i].a << 24;
		im3DVertices[i].nx = source[i].normal.x;
		im3DVertices[i].ny = source[i].normal.y;
		im3DVertices[i].nz = source[i].normal.z;
		im3DVertices[i].x = source[i].position.x;
		im3DVertices[i].y = source[i].position.y;
		im3DVertices[i].z = source[i].position.z;
	}
	ScePspFMatrix4 model;
	memset(&model, 0, sizeof(model));
	if(world){
		model.x.x = world->right.x; model.x.y = world->right.y; model.x.z = world->right.z;
		model.y.x = world->up.x;    model.y.y = world->up.y;    model.y.z = world->up.z;
		model.z.x = world->at.x;    model.z.y = world->at.y;    model.z.z = world->at.z;
		model.w.x = world->pos.x;   model.w.y = world->pos.y;   model.w.z = world->pos.z;
	}else{
		model.x.x = model.y.y = model.z.z = 1.0f;
	}
	model.w.w = 1.0f;
	sceGumMatrixMode(GU_MODEL);
	sceGumLoadMatrix(&model);
	if((flags & im3d::VERTEXUV) == 0)
		setRenderState(TEXTURERASTER, nil);
	// Lighting state and light upload are implemented with the static geometry
	// pipeline. Keep immediate-mode geometry deterministic and unlit for now.
	sceGuDisable(GU_LIGHTING);
	im3DVertexCount = numVertices;
}

static void
im3DRenderPrimitive(PrimitiveType type)
{
	int32 primitive = guPrimitive(type);
	if(primitive < 0 || im3DVertices == nil || im3DVertexCount <= 0)
		return;
	applyTexture(1);
	sceGumDrawArray(primitive, GU_TEXTURE_32BITF | GU_COLOR_8888 |
	    GU_NORMAL_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_3D,
	    im3DVertexCount, nil, im3DVertices);
}

static void
im3DRenderIndexedPrimitive(PrimitiveType type, void *indices, int32 numIndices)
{
	int32 primitive = guPrimitive(type);
	if(primitive < 0 || im3DVertices == nil || indices == nil || numIndices <= 0)
		return;
	uint16 *guIndices = static_cast<uint16 *>(sceGuGetMemory(sizeof(uint16)*numIndices));
	if(guIndices == nil)
		return;
	memcpy(guIndices, indices, sizeof(uint16)*numIndices);
	applyTexture(1);
	sceGumDrawArray(primitive, GU_TEXTURE_32BITF | GU_COLOR_8888 |
	    GU_NORMAL_32BITF | GU_VERTEX_32BITF | GU_INDEX_16BIT | GU_TRANSFORM_3D,
	    numIndices, guIndices, im3DVertices);
}

static void
im3DEnd(void)
{
	im3DVertices = nil;
	im3DVertexCount = 0;
	sceGuDisable(GU_LIGHTING);
}

void
drawGeometry(const Matrix *world, PrimitiveType type,
    const PspGeometryVertex *vertices, int32 numVertices,
    const uint16 *indices, int32 numIndices)
{
	int32 primitive = guPrimitive(type);
	if(primitive < 0 || vertices == nil || numVertices <= 0 || !listActive ||
	   (indices != nil && numIndices <= 0))
		return;
	ScePspFMatrix4 model;
	memset(&model, 0, sizeof(model));
	if(world){
		model.x.x = world->right.x; model.x.y = world->right.y; model.x.z = world->right.z;
		model.y.x = world->up.x;    model.y.y = world->up.y;    model.y.z = world->up.z;
		model.z.x = world->at.x;    model.z.y = world->at.y;    model.z.z = world->at.z;
		model.w.x = world->pos.x;   model.w.y = world->pos.y;   model.w.z = world->pos.z;
	}else
		model.x.x = model.y.y = model.z.z = 1.0f;
	model.w.w = 1.0f;
	sceGumMatrixMode(GU_MODEL);
	sceGumLoadMatrix(&model);
	applyTexture(1);
	sceGuDisable(GU_LIGHTING);
	int32 vertexType = GU_TEXTURE_32BITF | GU_COLOR_8888 |
	    GU_NORMAL_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_3D;
	if(indices)
		vertexType |= GU_INDEX_16BIT;
	sceGumDrawArray(primitive, vertexType,
	    indices ? numIndices : numVertices, indices, vertices);
}

int
deviceSystem(DeviceReq req, void *arg, int32 n)
{
	switch(req) {
	case DEVICEOPEN: {
		EngineOpenParams *params = static_cast<EngineOpenParams *>(arg);
		return params != nil && params->width == SCREEN_WIDTH &&
		    params->height == SCREEN_HEIGHT;
	}
	case DEVICEINIT:
		sceGuInit();
		sceGuStart(GU_DIRECT, displayList);
		sceGuDrawBuffer(GU_PSM_5650, reinterpret_cast<void *>(0), BUFFER_WIDTH);
		sceGuDispBuffer(SCREEN_WIDTH, SCREEN_HEIGHT,
		    reinterpret_cast<void *>(FRAMEBUFFER_SIZE), BUFFER_WIDTH);
		sceGuDepthBuffer(reinterpret_cast<void *>(DEPTHBUFFER_OFFSET), BUFFER_WIDTH);
		sceGuOffset(2048 - SCREEN_WIDTH/2, 2048 - SCREEN_HEIGHT/2);
		sceGuViewport(2048, 2048, SCREEN_WIDTH, SCREEN_HEIGHT);
		sceGuDepthRange(65535, 0);
		sceGuScissor(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
		sceGuEnable(GU_SCISSOR_TEST);
		sceGuDepthFunc(GU_GEQUAL);
		sceGuShadeModel(GU_SMOOTH);
		applyBlend();
		sceGuFinish();
		sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
		sceDisplayWaitVblankStart();
		sceGuDisplay(GU_TRUE);
		guInitialized = 1;
		listActive = 0;
		clearPending = 0;
		return 1;
	case DEVICETERM:
		if(listActive)
			endUpdate(nil);
		sceGuDisplay(GU_FALSE);
		sceGuTerm();
		guInitialized = 0;
		return 1;
	case DEVICECLOSE:
	case DEVICEFINALIZE:
		return 1;
	case DEVICEGETNUMSUBSYSTEMS:
		return 1;
	case DEVICEGETCURRENTSUBSYSTEM:
		return 0;
	case DEVICESETSUBSYSTEM:
		return n == 0;
	case DEVICEGETSUBSSYSTEMINFO:
		strncpy(static_cast<SubSystemInfo *>(arg)->name, "Sony PSP GU",
		    sizeof(static_cast<SubSystemInfo *>(arg)->name));
		return 1;
	case DEVICEGETNUMVIDEOMODES:
		return 1;
	case DEVICEGETCURRENTVIDEOMODE:
		return 0;
	case DEVICESETVIDEOMODE:
		return n == 0;
	case DEVICEGETVIDEOMODEINFO: {
		VideoMode *mode = static_cast<VideoMode *>(arg);
		mode->width = 480;
		mode->height = 272;
		mode->depth = 16;
		mode->flags = VIDEOMODEEXCLUSIVE;
		return 1;
	}
	case DEVICEGETMAXMULTISAMPLINGLEVELS:
	case DEVICEGETMULTISAMPLINGLEVELS:
		return 1;
	case DEVICESETMULTISAMPLINGLEVELS:
		return n == 1;
	}
	return 0;
}

Device renderdevice = {
	65535.0f, 0.0f,
	beginUpdate,
	endUpdate,
	clearCamera,
	showRaster,
	rasterRenderFast,
	setRenderState,
	getRenderState,
	im2DRenderLine,
	im2DRenderTriangle,
	im2DRenderPrimitive,
	im2DRenderIndexedPrimitive,
	im3DTransform,
	im3DRenderPrimitive,
	im3DRenderIndexedPrimitive,
	im3DEnd,
	deviceSystem
};

} // namespace psp
} // namespace rw

#endif // RW_PSP
