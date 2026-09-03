#ifdef RW_PSP

#include <stdio.h>
#include <string.h>
#include <pspgu.h>
#include <psputils.h>

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwengine.h"
#include "rwpspimpl.h"

#define PLUGIN_ID ID_DRIVER

namespace rw {
namespace psp {

int32 nativeRasterOffset;

static void *
createNativeRaster(void *object, int32 offset, int32)
{
	PspRaster *native = PLUGINOFFSET(PspRaster, object, offset);
	memset(native, 0, sizeof(*native));
	native->pixelFormat = -1;
	return object;
}

static void *
destroyNativeRaster(void *object, int32 offset, int32)
{
	PspRaster *native = PLUGINOFFSET(PspRaster, object, offset);
	if(native->allocation)
		rwFree(native->allocation);
	if(native->paletteAllocation)
		rwFree(native->paletteAllocation);
	if(native->lockAllocation)
		rwFree(native->lockAllocation);
	memset(native, 0, sizeof(*native));
	return object;
}

void
registerNativeRaster(void)
{
	nativeRasterOffset = Raster::registerPlugin(sizeof(PspRaster), ID_RASTERPSP,
		createNativeRaster, destroyNativeRaster, nil);
}

static int32
getFormatInfo(int32 format, int32 *depth, int32 *pixelFormat)
{
	if(format & Raster::PAL8){
		*depth = 8;
		*pixelFormat = GU_PSM_T8;
		return 1;
	}
	switch(format & 0xF00){
	case Raster::C565:  *depth = 16; *pixelFormat = GU_PSM_5650; return 2;
	case Raster::C1555: *depth = 16; *pixelFormat = GU_PSM_5551; return 2;
	case Raster::C4444: *depth = 16; *pixelFormat = GU_PSM_4444; return 2;
	case Raster::C8888: *depth = 32; *pixelFormat = GU_PSM_8888; return 4;
	default: return 0;
	}
}

Raster *
rasterCreate(Raster *raster)
{
	PspRaster *native = GETPSPRASTEREXT(raster);
	int32 depth, pixelFormat;
	int32 bpp = getFormatInfo(raster->format, &depth, &pixelFormat);
	if((raster->type != Raster::NORMAL && raster->type != Raster::TEXTURE) || bpp == 0){
		RWERROR((ERR_INVRASTER));
		return nil;
	}
	raster->depth = depth;
	raster->stride = raster->width*bpp;
	native->bytesPerPixel = bpp;
	native->pixelFormat = pixelFormat;
	native->size = raster->stride*raster->height;
	native->swizzled = raster->type == Raster::TEXTURE &&
		(raster->stride & 15) == 0 && (raster->height & 7) == 0;
	if(raster->width == 0 || raster->height == 0){
		raster->flags |= Raster::DONTALLOCATE;
		native->size = 0;
	}else if((raster->flags & Raster::DONTALLOCATE) == 0){
		native->allocation = rwMalloc(native->size + 15, MEMDUR_EVENT | ID_RASTERPSP);
		if(native->allocation == nil){
			return nil;
		}
		native->pixels = (uint8*)(((uintptr)native->allocation + 15) & ~(uintptr)15);
		memset(native->pixels, 0, native->size);
		if(raster->format & Raster::PAL8){
			native->paletteSize = 256*4;
			native->paletteAllocation = rwMalloc(native->paletteSize + 15,
				MEMDUR_EVENT | ID_RASTERPSP);
			if(native->paletteAllocation == nil)
				return nil;
			native->palette = (uint8*)(((uintptr)native->paletteAllocation + 15) & ~(uintptr)15);
			memset(native->palette, 0, native->paletteSize);
		}
	}
	raster->originalWidth = raster->width;
	raster->originalHeight = raster->height;
	raster->originalStride = raster->stride;
	raster->originalPixels = nil;
	return raster;
}

uint8 *
rasterLock(Raster *raster, int32 level, int32 lockMode)
{
	PspRaster *native = GETPSPRASTEREXT(raster);
	if(level != 0 || native->pixels == nil || raster->privateFlags != 0)
		return nil;
	if(native->swizzled){
		native->lockAllocation = rwMalloc(native->size + 15,
			MEMDUR_FUNCTION | ID_RASTERPSP);
		if(native->lockAllocation == nil)
			return nil;
		native->lockPixels = (uint8*)(((uintptr)native->lockAllocation + 15) & ~(uintptr)15);
		if((lockMode & Raster::LOCKNOFETCH) == 0){
			const int32 rowBlocks = raster->stride/16;
			for(int32 y = 0; y < raster->height; y++)
				for(int32 x = 0; x < raster->stride; x++){
					int32 block = x/16 + (y/8)*rowBlocks;
					native->lockPixels[y*raster->stride + x] =
						native->pixels[block*128 + (y&7)*16 + (x&15)];
				}
		}
	}else
		native->lockPixels = native->pixels;
	native->lockMode = lockMode;
	raster->pixels = native->lockPixels;
	raster->privateFlags = lockMode & Raster::LOCKWRITE ?
		Raster::PRIVATELOCK_WRITE : Raster::PRIVATELOCK_READ;
	return raster->pixels;
}

void
rasterUnlock(Raster *raster, int32 level)
{
	PspRaster *native = GETPSPRASTEREXT(raster);
	if(level != 0 || raster->privateFlags == 0)
		return;
	if((native->lockMode & Raster::LOCKWRITE) && native->swizzled){
		const int32 rowBlocks = raster->stride/16;
		for(int32 y = 0; y < raster->height; y++)
			for(int32 x = 0; x < raster->stride; x++){
				int32 block = x/16 + (y/8)*rowBlocks;
				native->pixels[block*128 + (y&7)*16 + (x&15)] =
					native->lockPixels[y*raster->stride + x];
			}
	}
	if(native->lockMode & Raster::LOCKWRITE)
		sceKernelDcacheWritebackRange(native->pixels, native->size);
	if(native->lockAllocation)
		rwFree(native->lockAllocation);
	native->lockAllocation = nil;
	native->lockPixels = nil;
	native->lockMode = 0;
	raster->pixels = nil;
	raster->privateFlags = 0;
}

uint8 *
rasterLockPalette(Raster *raster, int32 lockMode)
{
	PspRaster *native = GETPSPRASTEREXT(raster);
	if(native->palette == nil || raster->palette != nil)
		return nil;
	native->paletteLockMode = lockMode;
	raster->palette = native->palette;
	raster->privateFlags |= lockMode & Raster::LOCKWRITE ?
		Raster::PRIVATELOCK_WRITE_PALETTE : Raster::PRIVATELOCK_READ_PALETTE;
	return raster->palette;
}

void
rasterUnlockPalette(Raster *raster)
{
	PspRaster *native = GETPSPRASTEREXT(raster);
	if(raster->palette == nil)
		return;
	if(native->paletteLockMode & Raster::LOCKWRITE)
		sceKernelDcacheWritebackRange(native->palette, native->paletteSize);
	native->paletteLockMode = 0;
	raster->palette = nil;
	raster->privateFlags &= ~(Raster::PRIVATELOCK_READ_PALETTE |
		Raster::PRIVATELOCK_WRITE_PALETTE);
}
int32 rasterNumLevels(Raster *) { return 1; }

bool32
getNativeRaster(Raster *raster, NativeRaster *result)
{
	if(raster == nil || result == nil || raster->platform != PLATFORM_PSP)
		return 0;
	PspRaster *native = GETPSPRASTEREXT(raster);
	if(native->pixels == nil)
		return 0;
	result->pixels = native->pixels;
	result->palette = native->palette;
	result->pixelFormat = native->pixelFormat;
	result->bufferWidth = raster->stride/native->bytesPerPixel;
	result->swizzled = native->swizzled;
	return 1;
}

bool32
imageFindRasterFormat(Image *image, int32 type, int32 *width, int32 *height,
	int32 *depth, int32 *format)
{
	if((type & 0xF) != Raster::TEXTURE)
		return 0;
	*width = image->width;
	*height = image->height;
	if(image->depth == 8){
		*depth = 8;
		*format = Raster::C8888 | Raster::PAL8 | type;
	}else{
		*depth = 16;
		*format = (image->hasAlpha() ? Raster::C4444 : Raster::C565) | type;
	}
	return 1;
}

static uint16 pack565(uint8 r, uint8 g, uint8 b)
{ return (uint16)((r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11)); }
static uint16 pack1555(uint8 r, uint8 g, uint8 b, uint8 a)
{ return (uint16)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | ((a >= 128) << 15)); }
static uint16 pack4444(uint8 r, uint8 g, uint8 b, uint8 a)
{ return (uint16)((r >> 4) | ((g >> 4) << 4) | ((b >> 4) << 8) | ((a >> 4) << 12)); }

bool32
rasterFromImage(Raster *raster, Image *image)
{
	if(raster->type != Raster::TEXTURE ||
	   image->width != raster->width || image->height != raster->height)
		return 0;
	if(raster->format & Raster::PAL8){
		if(image->depth != 8 || image->palette == nil)
			return 0;
		uint8 *pixels = rasterLock(raster, 0, Raster::LOCKWRITE | Raster::LOCKNOFETCH);
		uint8 *palette = rasterLockPalette(raster, Raster::LOCKWRITE | Raster::LOCKNOFETCH);
		if(pixels == nil || palette == nil){
			if(palette) rasterUnlockPalette(raster);
			if(pixels) rasterUnlock(raster, 0);
			return 0;
		}
		for(int32 y = 0; y < image->height; y++)
			memcpy(pixels + y*raster->stride, image->pixels + y*image->stride, image->width);
		memcpy(palette, image->palette, 256*4);
		rasterUnlockPalette(raster);
		rasterUnlock(raster, 0);
		return 1;
	}
	if(image->depth != 32)
		return 0;
	uint8 *destination = rasterLock(raster, 0, Raster::LOCKWRITE | Raster::LOCKNOFETCH);
	if(destination == nil)
		return 0;
	int32 format = raster->format & 0xF00;
	for(int32 y = 0; y < image->height; y++){
		const uint8 *src = image->pixels + y*image->stride;
		uint8 *dst = destination + y*raster->stride;
		for(int32 x = 0; x < image->width; x++, src += 4){
			if(format == Raster::C8888)
				memcpy(dst + x*4, src, 4);
			else{
				uint16 value = format == Raster::C565 ? pack565(src[0], src[1], src[2]) :
					format == Raster::C1555 ? pack1555(src[0], src[1], src[2], src[3]) :
					pack4444(src[0], src[1], src[2], src[3]);
				memcpy(dst + x*2, &value, 2);
			}
		}
	}
	rasterUnlock(raster, 0);
	return 1;
}

static uint8 expand4(uint32 value) { return (uint8)((value << 4) | value); }
static uint8 expand5(uint32 value) { return (uint8)((value << 3) | (value >> 2)); }
static uint8 expand6(uint32 value) { return (uint8)((value << 2) | (value >> 4)); }

Image *
rasterToImage(Raster *raster)
{
	uint8 *source = rasterLock(raster, 0, Raster::LOCKREAD);
	if(source == nil)
		return nil;
	if(raster->format & Raster::PAL8){
		uint8 *palette = rasterLockPalette(raster, Raster::LOCKREAD);
		if(palette == nil){ rasterUnlock(raster, 0); return nil; }
		Image *image = Image::create(raster->width, raster->height, 8);
		if(image == nil){ rasterUnlockPalette(raster); rasterUnlock(raster, 0); return nil; }
		image->allocate();
		for(int32 y = 0; y < raster->height; y++)
			memcpy(image->pixels + y*image->stride, source + y*raster->stride, raster->width);
		memcpy(image->palette, palette, 256*4);
		rasterUnlockPalette(raster);
		rasterUnlock(raster, 0);
		return image;
	}
	Image *image = Image::create(raster->width, raster->height, 32);
	if(image == nil){ rasterUnlock(raster, 0); return nil; }
	image->allocate();
	int32 format = raster->format & 0xF00;
	for(int32 y = 0; y < raster->height; y++){
		const uint8 *src = source + y*raster->stride;
		uint8 *dst = image->pixels + y*image->stride;
		for(int32 x = 0; x < raster->width; x++, dst += 4){
			if(format == Raster::C8888){ memcpy(dst, src + x*4, 4); continue; }
			uint16 value; memcpy(&value, src + x*2, 2);
			if(format == Raster::C565){ dst[0]=expand5(value&31); dst[1]=expand6((value>>5)&63); dst[2]=expand5((value>>11)&31); dst[3]=255; }
			else if(format == Raster::C1555){ dst[0]=expand5(value&31); dst[1]=expand5((value>>5)&31); dst[2]=expand5((value>>10)&31); dst[3]=value&0x8000 ? 255 : 0; }
			else { dst[0]=expand4(value&15); dst[1]=expand4((value>>4)&15); dst[2]=expand4((value>>8)&15); dst[3]=expand4((value>>12)&15); }
		}
	}
	rasterUnlock(raster, 0);
	return image;
}

} // namespace psp
} // namespace rw
#endif
