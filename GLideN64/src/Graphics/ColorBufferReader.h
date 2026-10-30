#pragma once
#include <vector>
#include <memory>
#include <Types.h>
#include <Textures.h>

namespace graphics {

class ColorBufferReader
{
public:
	ColorBufferReader(CachedTexture * _pTexture);
	virtual ~ColorBufferReader() = default;

	virtual const u8 * readPixels(s32 _x0, s32 _y0, u32 _width, u32 _height, u32 _size, bool _sync);
	virtual void cleanUp() = 0;
	enum class Kind { Unsupported, ReadPixels, PixelBuffer, BufferStorage };
	virtual Kind kind() const { return Kind::Unsupported; }
	virtual std::unique_ptr<ColorBufferReader> clone(CachedTexture * _texture,
		std::unique_ptr<ColorBufferReader> _reuse) const
	{
		return std::unique_ptr<ColorBufferReader>();
	}

protected:
	struct ReadColorBufferParams {
		s32 x0;
		s32 y0;
		u32 width;
		u32 height;
		bool sync;
		ColorFormatParam colorFormat;
		DatatypeParam colorType;
		u32 colorFormatBytes;
	};

	CachedTexture * m_pTexture;
	std::vector<u8> m_pixelData;
	std::vector<u8> m_tempPixelData;
	void copyState(ColorBufferReader & _reader) const
	{
		_reader.m_pixelData = m_pixelData;
		_reader.m_tempPixelData = m_tempPixelData;
	}

private:
	const u8* _convertFloatTextureBuffer(const u8* _gpuData, u32 _width, u32 _height, u32 _heightOffset, u32 _stride);
	const u8* _convertIntegerTextureBuffer(const u8* _gpuData, u32 _width, u32 _height,u32 _heightOffset, u32 _stride, u32 _colorsPerPixel);
	virtual const u8 * _readPixels(const ReadColorBufferParams& _params, u32& _heightOffset, u32& _stride) = 0;
};

}
