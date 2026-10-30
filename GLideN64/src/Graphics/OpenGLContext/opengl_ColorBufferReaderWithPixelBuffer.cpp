#include <Config.h>
#include <Graphics/Context.h>
#include "opengl_ColorBufferReaderWithPixelBuffer.h"

using namespace graphics;
using namespace opengl;

ColorBufferReaderWithPixelBuffer::ColorBufferReaderWithPixelBuffer(CachedTexture *_pTexture,
																   CachedBindBuffer *_bindBuffer)
	: ColorBufferReader(_pTexture), m_bindBuffer(_bindBuffer)
	, m_bufferSize(_pTexture->textureBytes)
{
	_initBuffers();
}


ColorBufferReaderWithPixelBuffer::~ColorBufferReaderWithPixelBuffer()
{
	_destroyBuffers();
}

void ColorBufferReaderWithPixelBuffer::_destroyBuffers()
{
	glDeleteBuffers(m_numPBO, m_PBO);

	for (u32 index = 0; index < m_numPBO; ++index)
		m_PBO[index] = 0;
}

void ColorBufferReaderWithPixelBuffer::_initBuffers()
{
	m_numPBO = std::max(1u, config.frameBufferEmulation.copyToRDRAM);
	if (m_numPBO > _maxPBO)
		m_numPBO = _maxPBO;

	// Generate Pixel Buffer Objects
	glGenBuffers(m_numPBO, m_PBO);
	m_curIndex = 0;

	// Initialize Pixel Buffer Objects
	for (u32 i = 0; i < m_numPBO; ++i) {
		/* Define the previous slot before the first asynchronous read. */
		m_bindBuffer->bind(Parameter(GL_PIXEL_PACK_BUFFER), ObjectHandle(m_PBO[i]));
		glBufferData(GL_PIXEL_PACK_BUFFER, m_bufferSize, m_pixelData.data(), GL_DYNAMIC_READ);
	}
	m_bindBuffer->bind(Parameter(GL_PIXEL_PACK_BUFFER), ObjectHandle::null);
}

const u8 * ColorBufferReaderWithPixelBuffer::_readPixels(const ReadColorBufferParams& _params, u32& _heightOffset,
	u32& _stride)
{
	GLenum format = GLenum(_params.colorFormat);
	GLenum type = GLenum(_params.colorType);

	m_bindBuffer->bind(Parameter(GL_PIXEL_PACK_BUFFER), ObjectHandle(m_PBO[m_curIndex]));
	glReadPixels(_params.x0, _params.y0, m_pTexture->width, _params.height, format, type, 0);
	// If Sync, read pixels from the buffer, copy them to RDRAM.
	// If not Sync, read pixels from the buffer, copy pixels from the previous buffer to RDRAM.
	if (!_params.sync) {
		m_curIndex = (m_curIndex + 1) % m_numPBO;
		m_bindBuffer->bind(Parameter(GL_PIXEL_PACK_BUFFER), ObjectHandle(m_PBO[m_curIndex]));
	}

	_heightOffset = 0;
	_stride = m_pTexture->width;

	return reinterpret_cast<u8*>(glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0,
		m_pTexture->width * _params.height * _params.colorFormatBytes, GL_MAP_READ_BIT));
}

void ColorBufferReaderWithPixelBuffer::cleanUp()
{
	glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
	m_bindBuffer->bind(Parameter(GL_PIXEL_PACK_BUFFER), ObjectHandle::null);
}

std::unique_ptr<ColorBufferReader> ColorBufferReaderWithPixelBuffer::clone(CachedTexture * _texture,
	std::unique_ptr<ColorBufferReader> _reuse) const
{
	std::unique_ptr<ColorBufferReaderWithPixelBuffer> copy;
	if (_reuse && _reuse->kind() == kind()) {
		auto *reader = static_cast<ColorBufferReaderWithPixelBuffer *>(_reuse.get());
		if (reader->m_bufferSize == m_bufferSize && reader->m_numPBO == m_numPBO)
			copy.reset(static_cast<ColorBufferReaderWithPixelBuffer *>(_reuse.release()));
	}
	if (!copy)
		copy.reset(new ColorBufferReaderWithPixelBuffer(_texture, m_bindBuffer));
	copy->m_pTexture = _texture;
	if (copy->m_numPBO != m_numPBO)
		return std::unique_ptr<ColorBufferReader>();
	for (u32 i = 0; i < m_numPBO; ++i) {
		if (!m_bindBuffer->copy(ObjectHandle(m_PBO[i]), ObjectHandle(copy->m_PBO[i]), m_bufferSize))
			return std::unique_ptr<ColorBufferReader>();
	}
	copy->m_curIndex = m_curIndex;
	copyState(*copy);
	return std::unique_ptr<ColorBufferReader>(copy.release());
}
