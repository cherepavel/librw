#ifdef RW_PSP

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwengine.h"
#include "rwpsp.h"

namespace rw {
namespace psp {

Texture*
readNativeTexture(Stream*)
{
	// The on-disk PSP native texture format will be introduced with the
	// swizzled raster implementation. Do not silently accept desktop rasters.
	return nil;
}

void
writeNativeTexture(Texture*, Stream*)
{
}

uint32
getSizeNativeTexture(Texture*)
{
	return 0;
}

} // namespace psp
} // namespace rw

#endif // RW_PSP
