#ifdef RW_PSP

#include <assert.h>

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwengine.h"
#include "rwpspimpl.h"

namespace rw {
namespace psp {

static void*
driverOpen(void *object, int32, int32)
{
	assert(engine->driver[PLATFORM_PSP] != nil);
	Driver *driver = engine->driver[PLATFORM_PSP];
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
