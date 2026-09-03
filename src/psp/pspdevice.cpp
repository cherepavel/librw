#ifdef RW_PSP

#include <stdio.h>
#include <string.h>

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwengine.h"
#include "rwpspimpl.h"

namespace rw {
namespace psp {

static void beginUpdate(Camera*) { }
static void endUpdate(Camera*) { }
static void clearCamera(Camera*, RGBA*, uint32) { }
static void showRaster(Raster*, uint32) { }
static bool32 rasterRenderFast(Raster*, int32, int32) { return 0; }
static void setRenderState(int32, void*) { }
static void *getRenderState(int32) { return nil; }
static void im2DRenderLine(void*, int32, int32, int32) { }
static void im2DRenderTriangle(void*, int32, int32, int32, int32) { }
static void im2DRenderPrimitive(PrimitiveType, void*, int32) { }
static void im2DRenderIndexedPrimitive(PrimitiveType, void*, int32, void*, int32) { }
static void im3DTransform(void*, int32, Matrix*, uint32) { }
static void im3DRenderPrimitive(PrimitiveType) { }
static void im3DRenderIndexedPrimitive(PrimitiveType, void*, int32) { }
static void im3DEnd(void) { }

int
deviceSystem(DeviceReq req, void *arg, int32 n)
{
	switch(req) {
	case DEVICEINIT:
	case DEVICETERM:
	case DEVICEOPEN:
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
