#ifdef RW_PSP

#include <assert.h>

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwengine.h"
#include "rwpsp.h"

namespace rw {
namespace psp {

static void*
driverOpen(void *object, int32, int32)
{
	// Raster and geometry hooks are introduced in the next vertical slice.
	assert(engine->driver[PLATFORM_PSP] != nil);
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
	Driver::registerPlugin(PLATFORM_PSP, 0, PLATFORM_PSP,
	    driverOpen, driverClose);
}

} // namespace psp
} // namespace rw

#endif // RW_PSP
