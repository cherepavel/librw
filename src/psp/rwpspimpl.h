#pragma once

#include "rwpsp.h"

namespace rw {
namespace psp {

int deviceSystem(DeviceReq req, void *arg, int32 n);

extern int32 nativeRasterOffset;
struct PspRaster {
	struct MipLevel {
		uint8 *pixels;
		void *allocation;
		uint32 size;
		int32 width, height, stride;
		bool32 swizzled;
	} mipmaps[9];
	uint8 *pixels;
	void *allocation;
	uint8 *palette;
	void *paletteAllocation;
	uint32 size;
	uint32 paletteSize;
	int32 pixelFormat;
	int32 bytesPerPixel;
	int32 lockMode;
	int32 paletteLockMode;
	void *lockAllocation;
	uint8 *lockPixels;
	bool32 swizzled;
	int32 numLevels;
	int32 lockLevel;
};
#define GETPSPRASTEREXT(raster) (PLUGINOFFSET(PspRaster, raster, nativeRasterOffset))
void registerNativeRaster(void);
Raster *rasterCreate(Raster *raster);
uint8 *rasterLock(Raster *raster, int32 level, int32 lockMode);
void rasterUnlock(Raster *raster, int32 level);
uint8 *rasterLockPalette(Raster *raster, int32 lockMode);
void rasterUnlockPalette(Raster *raster);
int32 rasterNumLevels(Raster *raster);
bool32 imageFindRasterFormat(Image *image, int32 type, int32 *width,
	int32 *height, int32 *depth, int32 *format);
bool32 rasterFromImage(Raster *raster, Image *image);
Image *rasterToImage(Raster *raster);
ObjPipeline *makeDefaultPipeline(void);
ObjPipeline *makeSkinPipeline(void);
void *destroyNativeGeometry(void *object, int32 offset, int32 size);

struct PspGeometryVertex {
	float32 u, v;
	uint32 color;
	float32 nx, ny, nz;
	float32 x, y, z;
};
static_assert(sizeof(PspGeometryVertex) == 36,
	"Unexpected PSP geometry vertex layout");
PspGeometryVertex *allocTransientGeometryVertices(int32 count);

void drawGeometry(const Matrix *world, PrimitiveType primitive,
	const PspGeometryVertex *vertices, int32 numVertices,
	const uint16 *indices, int32 numIndices);

} // namespace psp
} // namespace rw
