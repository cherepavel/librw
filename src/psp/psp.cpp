#ifdef RW_PSP

#include <assert.h>

#include "../rwbase.h"
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

static void*
skinOpen(void *object, int32, int32)
{
	skinGlobals.pipelines[PLATFORM_PSP] = makeSkinPipeline();
	return object;
}

static void*
skinClose(void *object, int32, int32)
{
	ObjPipeline *pipeline = skinGlobals.pipelines[PLATFORM_PSP];
	if(pipeline && pipeline != skinGlobals.dummypipe)
		pipeline->destroy();
	skinGlobals.pipelines[PLATFORM_PSP] = nil;
	return object;
}

void
initSkin(void)
{
	Driver::registerPlugin(PLATFORM_PSP, 0, ID_SKIN, skinOpen, skinClose);
}

/* MatFX pipeline for PSP.
 * GTA III vehicle DFFs carry ENVMAP/DUAL MatFX chunks on their materials.
 * Without a registered PSP pipeline the MatFX plugin sets
 * atomic->pipeline = matFXGlobals.dummypipe, which has no render callback
 * and silently drops every vehicle atomic.  We register a real pipeline that
 * routes MatFX atomics through the standard PSP geometry renderer.  The
 * env-map pass is skipped because the PSP backend has no camera-texture path
 * yet; the diffuse layer is correct. */
static void*
matfxOpen(void *object, int32, int32)
{
	matFXGlobals.pipelines[PLATFORM_PSP] = makeDefaultPipeline();
	return object;
}

static void*
matfxClose(void *object, int32, int32)
{
	ObjPipeline *pipeline =
	    static_cast<ObjPipeline *>(matFXGlobals.pipelines[PLATFORM_PSP]);
	if(pipeline && pipeline != matFXGlobals.dummypipe)
		pipeline->destroy();
	matFXGlobals.pipelines[PLATFORM_PSP] = nil;
	return object;
}

void
initMatFX(void)
{
	Driver::registerPlugin(PLATFORM_PSP, 0, ID_MATFX, matfxOpen, matfxClose);
}

static void*
driverOpen(void *object, int32, int32)
{
	assert(engine->driver[PLATFORM_PSP] != nil);
	Driver *driver = engine->driver[PLATFORM_PSP];
	driver->defaultPipeline = makeDefaultPipeline();
	driver->rasterNativeOffset = nativeRasterOffset;
	driver->rasterCreate = rasterCreate;
	driver->rasterLock = rasterLock;
	driver->rasterUnlock = rasterUnlock;
	driver->rasterLockPalette = rasterLockPalette;
	driver->rasterUnlockPalette = rasterUnlockPalette;
	driver->rasterNumLevels = rasterNumLevels;
	driver->imageFindRasterFormat = imageFindRasterFormat;
	driver->rasterFromImage = rasterFromImage;
	driver->rasterToImage = rasterToImage;
	return object;
}

static void*
driverClose(void *object, int32, int32)
{
	Driver *driver = engine->driver[PLATFORM_PSP];
	if(driver->defaultPipeline && driver->defaultPipeline != engine->dummyDefaultPipeline){
		driver->defaultPipeline->destroy();
		driver->defaultPipeline = engine->dummyDefaultPipeline;
	}
	return object;
}

void
registerPlatformPlugins(void)
{
	registerNativeRaster();
	Driver::registerPlugin(PLATFORM_PSP, 0, PLATFORM_PSP,
	    driverOpen, driverClose);
}

} // namespace psp
} // namespace rw

#endif // RW_PSP
