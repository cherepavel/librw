#ifdef RW_PSP

#include <stdio.h>
#include <string.h>

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwengine.h"
#include "rwpsp.h"

#define PLUGIN_ID ID_DRIVER

namespace rw {
namespace psp {

Texture*
readNativeTexture(Stream *stream)
{
	if(!findChunk(stream, ID_STRUCT, nil, nil)){
		RWERROR((ERR_CHUNK, "STRUCT"));
		return nil;
	}
	uint32 platform = stream->readU32();
	if(platform != PLATFORM_D3D8){
		RWERROR((ERR_PLATFORM, platform));
		return nil;
	}

	Texture *texture = Texture::create(nil);
	if(texture == nil)
		return nil;
	texture->filterAddressing = stream->readU32();
	stream->read8(texture->name, 32);
	stream->read8(texture->mask, 32);

	int32 format = stream->readI32();
	stream->readI32(); // hasAlpha; source pixels/palette contain authoritative alpha
	int32 width = stream->readU16();
	int32 height = stream->readU16();
	int32 depth = stream->readU8();
	int32 numLevels = stream->readU8();
	int32 type = stream->readU8();
	int32 compression = stream->readU8();
	bool32 pal8 = depth == 8 &&
		(format & (Raster::PAL4 | Raster::PAL8)) == Raster::PAL8;
	bool32 bgra8888 = depth == 32 && (format & 0xF00) == Raster::C8888 &&
		(format & (Raster::PAL4 | Raster::PAL8)) == 0;
	if(width <= 0 || height <= 0 || numLevels <= 0 ||
	   type != Raster::TEXTURE || compression != 0 || (!pal8 && !bgra8888)){
		texture->destroy();
		return nil;
	}

	int32 destinationFormat;
	if(pal8)
		destinationFormat = (format & ~Raster::PAL4) | Raster::PAL8;
	else
		destinationFormat = (format & ~(0xF00 | Raster::PAL4 | Raster::PAL8)) |
			Raster::C4444;
	if(numLevels > 1)
		destinationFormat |= Raster::MIPMAP;
	else
		destinationFormat &= ~Raster::MIPMAP;
	Raster *raster = Raster::create(width, height, pal8 ? 8 : 16,
		destinationFormat | type,
		PLATFORM_PSP);
	if(raster == nil){
		texture->destroy();
		return nil;
	}
	texture->raster = raster;

	if(pal8){
		uint8 *palette = raster->lockPalette(Raster::LOCKWRITE | Raster::LOCKNOFETCH);
		if(palette == nil || stream->read8(palette, 256*4) != 256*4){
			if(palette) raster->unlockPalette();
			texture->destroy();
			return nil;
		}
		raster->unlockPalette();
	}

	for(int32 level = 0; level < numLevels; level++){
		uint32 size = stream->readU32();
		if(level >= raster->getNumLevels()){
			stream->seek(size);
			continue;
		}
		if(pal8){
			uint8 *pixels = raster->lock(level,
				Raster::LOCKWRITE | Raster::LOCKNOFETCH);
			uint32 expected = pixels ? raster->stride*raster->height : 0;
			if(pixels == nil || size != expected || stream->read8(pixels, size) != size){
				if(pixels) raster->unlock(level);
				texture->destroy();
				return nil;
			}
			raster->unlock(level);
		}else{
			uint32 sourceSize = raster->width*raster->height*4;
			uint8 *source = (uint8*)rwMalloc(sourceSize, MEMDUR_FUNCTION | ID_RASTERPSP);
			uint8 *pixels = raster->lock(level,
				Raster::LOCKWRITE | Raster::LOCKNOFETCH);
			if(source == nil || pixels == nil || size != sourceSize ||
			   stream->read8(source, sourceSize) != sourceSize){
				if(pixels) raster->unlock(level);
				if(source) rwFree(source);
				texture->destroy();
				return nil;
			}
			for(int32 y = 0; y < raster->height; y++)
				for(int32 x = 0; x < raster->width; x++){
					uint8 *bgra = source + (y*raster->width + x)*4;
					uint16 rgba = (uint16)((bgra[2] >> 4) | ((bgra[1] >> 4) << 4) |
						((bgra[0] >> 4) << 8) | ((bgra[3] >> 4) << 12));
					memcpy(pixels + y*raster->stride + x*2, &rgba, 2);
				}
			raster->unlock(level);
			rwFree(source);
		}
	}
	return texture;
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
