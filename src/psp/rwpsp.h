#pragma once

namespace rw {

#ifdef RW_PSP
struct EngineOpenParams
{
	int32 width;
	int32 height;
};
#endif

namespace psp {

void registerPlatformPlugins(void);
void initSkin(void);
extern Device renderdevice;

Texture *readNativeTexture(Stream *stream);
void writeNativeTexture(Texture *texture, Stream *stream);
uint32 getSizeNativeTexture(Texture *texture);

struct NativeRaster
{
	const void *pixels;
	const void *palette;
	int32 pixelFormat;
	int32 bufferWidth;
	bool32 swizzled;
};

bool32 getNativeRaster(Raster *raster, NativeRaster *native);
bool32 getNativeRasterLevel(Raster *raster, int32 level, NativeRaster *native);

struct Im2DVertex
{
	float32 x, y, z, cameraZ;
	uint8 r, g, b, a;
	float32 u, v;
	float32 recipCameraZ;

	void setScreenX(float32 value) { x = value; }
	void setScreenY(float32 value) { y = value; }
	void setScreenZ(float32 value) { z = value; }
	void setCameraZ(float32 value) { cameraZ = value; }
	void setRecipCameraZ(float32 value) { recipCameraZ = value; }
	void setColor(uint8 red, uint8 green, uint8 blue, uint8 alpha) {
		r = red; g = green; b = blue; a = alpha;
	}
	void setU(float32 value, float32) { u = value; }
	void setV(float32 value, float32) { v = value; }

	float32 getScreenX(void) { return x; }
	float32 getScreenY(void) { return y; }
	float32 getScreenZ(void) { return z; }
	float32 getCameraZ(void) { return cameraZ; }
	float32 getRecipCameraZ(void) { return recipCameraZ; }
	RGBA getColor(void) { return makeRGBA(r, g, b, a); }
	float32 getU(void) { return u; }
	float32 getV(void) { return v; }
};

struct Im3DVertex
{
	V3d position;
	V3d normal;
	uint8 r, g, b, a;
	float32 u, v;

	void setX(float32 value) { position.x = value; }
	void setY(float32 value) { position.y = value; }
	void setZ(float32 value) { position.z = value; }
	void setNormalX(float32 value) { normal.x = value; }
	void setNormalY(float32 value) { normal.y = value; }
	void setNormalZ(float32 value) { normal.z = value; }
	void setColor(uint8 red, uint8 green, uint8 blue, uint8 alpha) {
		r = red; g = green; b = blue; a = alpha;
	}
	void setU(float32 value) { u = value; }
	void setV(float32 value) { v = value; }

	float32 getX(void) { return position.x; }
	float32 getY(void) { return position.y; }
	float32 getZ(void) { return position.z; }
	float32 getNormalX(void) { return normal.x; }
	float32 getNormalY(void) { return normal.y; }
	float32 getNormalZ(void) { return normal.z; }
	RGBA getColor(void) { return makeRGBA(r, g, b, a); }
	float32 getU(void) { return u; }
	float32 getV(void) { return v; }
};

static_assert(sizeof(Im2DVertex) == 32, "Unexpected PSP Im2D vertex layout");
static_assert(sizeof(Im3DVertex) == 36, "Unexpected PSP Im3D vertex layout");

} // namespace psp
} // namespace rw
