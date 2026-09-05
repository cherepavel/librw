#ifdef RW_PSP
#include <stdio.h>
#include <psputils.h>
#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwengine.h"
#include "rwpspimpl.h"
#define PLUGIN_ID ID_DRIVER
namespace rw { namespace psp {
enum { PTX1_MAGIC = 0x31585450, PTX1_SWIZZLED = 1, GU_PSM_T8_VALUE = 5 };
static bool32 readExact(Stream *s, void *p, uint32 n) { return s->read8(p, n) == n; }

Texture *readNativeTexture(Stream *stream)
{
	if(!findChunk(stream, ID_STRUCT, nil, nil)){ RWERROR((ERR_CHUNK, "STRUCT")); return nil; }
	uint32 platform = stream->readU32();
	if(platform != PLATFORM_PSP){
		printf("PSP_NATIVE_TEXTURE_REJECTED platform=%lu expected=%d\n", platform, PLATFORM_PSP);
		RWERROR((ERR_PLATFORM, platform)); return nil;
	}
	Texture *texture = Texture::create(nil);
	if(texture == nil) return nil;
	texture->filterAddressing = stream->readU32();
	stream->read8(texture->name, 32); stream->read8(texture->mask, 32);
	uint32 magic = stream->readU32(), format = stream->readU32();
	uint32 hasAlpha = stream->readU32();
	int32 width = stream->readU16(), height = stream->readU16();
	int32 pixelFormat = stream->readU8(), numLevels = stream->readU8();
	int32 type = stream->readU8(); uint32 flags = stream->readU8();
	uint32 paletteSize = stream->readU32();
	if(magic != PTX1_MAGIC || width <= 0 || width > 512 || height <= 0 || height > 512 ||
	   pixelFormat != GU_PSM_T8_VALUE || numLevels != 1 || type != Raster::TEXTURE ||
	   paletteSize != 256*4 || (format & Raster::PAL8) == 0){
		printf("PSP_NATIVE_TEXTURE_INVALID name=%s magic=%08lx format=%08lx size=%ldx%ld psm=%ld levels=%ld type=%ld palette=%lu\n",
		    texture->name, magic, format, width, height, pixelFormat, numLevels, type, paletteSize);
		texture->destroy(); return nil;
	}
	Raster *raster = Raster::create(width, height, 8,
	    (format & ~(Raster::PAL4 | Raster::MIPMAP | Raster::AUTOMIPMAP)) |
	    Raster::PAL8 | Raster::TEXTURE, PLATFORM_PSP);
	if(raster == nil){ texture->destroy(); return nil; }
	texture->raster = raster;
	PspRaster *native = GETPSPRASTEREXT(raster);
	if(!readExact(stream, native->palette, paletteSize)){
		printf("PSP_NATIVE_TEXTURE_PALETTE_FAILED name=%s\n", texture->name);
		texture->destroy(); return nil;
	}
	uint32 levelWidth = stream->readU16(), levelHeight = stream->readU16();
	uint32 stride = stream->readU16(); stream->readU16();
	uint32 dataSize = stream->readU32(), storedSize = stream->readU32();
	bool32 swizzled = (flags & PTX1_SWIZZLED) != 0;
	if(levelWidth != (uint32)width || levelHeight != (uint32)height ||
	   stride != (uint32)raster->stride || dataSize != native->size ||
	   storedSize < dataSize || (storedSize & 15) != 0 || swizzled != native->swizzled){
		printf("PSP_NATIVE_TEXTURE_LEVEL_INVALID name=%s level=%lux%lu stride=%lu data=%lu stored=%lu expected_stride=%ld expected_data=%lu swizzled=%ld/%ld\n",
		    texture->name, levelWidth, levelHeight, stride, dataSize, storedSize,
		    raster->stride, native->size, swizzled, native->swizzled);
		texture->destroy(); return nil;
	}
	if(!readExact(stream, native->pixels, dataSize)){
		printf("PSP_NATIVE_TEXTURE_DATA_FAILED name=%s\n", texture->name);
		texture->destroy(); return nil;
	}
	if(storedSize > dataSize) stream->seek(storedSize-dataSize);
	native->pixelFormat = pixelFormat; native->swizzled = swizzled;
	sceKernelDcacheWritebackRange(native->palette, native->paletteSize);
	sceKernelDcacheWritebackRange(native->pixels, native->size);
	(void)hasAlpha;
	return texture;
}

void writeNativeTexture(Texture *texture, Stream *stream)
{
	Raster *raster = texture->raster; PspRaster *native = GETPSPRASTEREXT(raster);
	uint32 storedSize = (native->size+15) & ~15;
	writeChunkHeader(stream, ID_STRUCT, getSizeNativeTexture(texture)-12);
	stream->writeU32(PLATFORM_PSP); stream->writeU32(texture->filterAddressing);
	stream->write8(texture->name, 32); stream->write8(texture->mask, 32);
	stream->writeU32(PTX1_MAGIC); stream->writeU32(raster->format);
	bool32 hasAlpha = 0;
	for(uint32 i = 3; i < native->paletteSize; i += 4) if(native->palette[i] != 255){ hasAlpha = 1; break; }
	stream->writeU32(hasAlpha); stream->writeU16(raster->width); stream->writeU16(raster->height);
	stream->writeU8(GU_PSM_T8_VALUE); stream->writeU8(1); stream->writeU8(Raster::TEXTURE);
	stream->writeU8(native->swizzled ? PTX1_SWIZZLED : 0); stream->writeU32(native->paletteSize);
	stream->write8(native->palette, native->paletteSize);
	stream->writeU16(raster->width); stream->writeU16(raster->height); stream->writeU16(raster->stride);
	stream->writeU16(0); stream->writeU32(native->size); stream->writeU32(storedSize);
	stream->write8(native->pixels, native->size);
	uint8 padding[15] = { 0 }; if(storedSize > native->size) stream->write8(padding, storedSize-native->size);
}

uint32 getSizeNativeTexture(Texture *texture)
{
	PspRaster *native = GETPSPRASTEREXT(texture->raster);
	return 12 + 96 + native->paletteSize + 16 + ((native->size+15) & ~15);
}
} }
#endif
