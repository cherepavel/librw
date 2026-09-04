#ifdef RW_PSP

#include <pspdisplay.h>
#include <pspgu.h>
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
applyTexture(void)
{
	if(state.texture == nil){
		sceGuDisable(GU_TEXTURE_2D);
		return;
	}
	NativeRaster native;
	if(!getNativeRaster(state.texture, &native)){
		sceGuDisable(GU_TEXTURE_2D);
		return;
	}
	sceGuEnable(GU_TEXTURE_2D);
	if(native.palette){
		sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
		sceGuClutLoad(32, native.palette);
	}
	sceGuTexMode(native.pixelFormat, 0, 0, native.swizzled);
	sceGuTexImage(0, state.texture->originalWidth, state.texture->originalHeight,
	    native.bufferWidth, native.pixels);
	sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
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
	applyTexture();
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
beginUpdate(Camera*)
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
		if(value) sceGuEnable(GU_FOG); else sceGuDisable(GU_FOG);
		break;
	case FOGCOLOR: state.fogColor = value; break;
	case CULLMODE:
		state.cullMode = value;
		if(value == CULLNONE) sceGuDisable(GU_CULL_FACE);
		else { sceGuEnable(GU_CULL_FACE); sceGuFrontFace(value == CULLBACK ? GU_CCW : GU_CW); }
		break;
	case ALPHATESTFUNC: state.alphaTestFunc = value; break;
	case ALPHATESTREF: state.alphaTestRef = value; break;
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
static void im3DTransform(void*, int32, Matrix*, uint32) { }
static void im3DRenderPrimitive(PrimitiveType) { }
static void im3DRenderIndexedPrimitive(PrimitiveType, void*, int32) { }
static void im3DEnd(void) { }

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
