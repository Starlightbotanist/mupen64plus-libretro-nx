#pragma once
#include <Graphics/ColorBufferReader.h>
#include "opengl_CachedFunctions.h"

namespace opengl {

class ColorBufferReaderWithPixelBuffer :
		public graphics::ColorBufferReader
{
public:
	ColorBufferReaderWithPixelBuffer(CachedTexture * _pTexture,
			CachedBindBuffer * _bindBuffer);
	~ColorBufferReaderWithPixelBuffer();

	const u8 * _readPixels(const ReadColorBufferParams& _params, u32& _heightOffset, u32& _stride) override;
	void cleanUp() override;
	Kind kind() const override { return Kind::PixelBuffer; }
	std::unique_ptr<graphics::ColorBufferReader> clone(CachedTexture * _texture,
		std::unique_ptr<graphics::ColorBufferReader> _reuse) const override;

private:
	void _initBuffers();
	void _destroyBuffers();

	CachedBindBuffer * m_bindBuffer;
	size_t m_bufferSize;

	u32 m_numPBO;
	static const int _maxPBO = 3;
	GLuint m_PBO[_maxPBO];
	u32 m_curIndex;
};

}
