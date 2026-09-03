#ifdef RW_PSP

#include <stdio.h>

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
	stream->readI32(); // hasAlpha; the palette contains authoritative alpha
	int32 width = stream->readU16();
	int32 height = stream->readU16();
	int32 depth = stream->readU8();
	int32 numLevels = stream->readU8();
	int32 type = stream->readU8();
	int32 compression = stream->readU8();
	if(width <= 0 || height <= 0 || numLevels <= 0 || depth != 8 ||
	   type != Raster::TEXTURE || compression != 0 ||
	   (format & (Raster::PAL4 | Raster::PAL8)) != Raster::PAL8){
		texture->destroy();
		return nil;
	}

	format &= ~Raster::PAL4;
	format |= Raster::PAL8;
	if(numLevels > 1)
		format |= Raster::MIPMAP;
	else
		format &= ~Raster::MIPMAP;
	Raster *raster = Raster::create(width, height, depth, format | type,
		PLATFORM_PSP);
	if(raster == nil){
		texture->destroy();
		return nil;
	}
	texture->raster = raster;

	uint8 *palette = raster->lockPalette(Raster::LOCKWRITE | Raster::LOCKNOFETCH);
	if(palette == nil || stream->read8(palette, 256*4) != 256*4){
		if(palette) raster->unlockPalette();
		texture->destroy();
		return nil;
	}
	raster->unlockPalette();

	for(int32 level = 0; level < numLevels; level++){
		uint32 size = stream->readU32();
		if(level >= raster->getNumLevels()){
			stream->seek(size);
			continue;
		}
		uint8 *pixels = raster->lock(level,
			Raster::LOCKWRITE | Raster::LOCKNOFETCH);
		uint32 expected = pixels ? raster->stride*raster->height : 0;
		if(pixels == nil || size != expected || stream->read8(pixels, size) != size){
			if(pixels) raster->unlock(level);
			texture->destroy();
			return nil;
		}
		raster->unlock(level);
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
