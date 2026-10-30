#include <Config.h>
#include <Graphics/Context.h>
#include "opengl_ColorBufferReaderWithBufferStorage.h"

using namespace graphics;
using namespace opengl;

ColorBufferReaderWithBufferStorage::ColorBufferReaderWithBufferStorage(CachedTexture * _pTexture,
	CachedBindBuffer * _bindBuffer)
	: ColorBufferReader(_pTexture), m_bindBuffer(_bindBuffer)
	, m_bufferSize(_pTexture->textureBytes)
{
	_initBuffers();
}

ColorBufferReaderWithBufferStorage::~ColorBufferReaderWithBufferStorage()
{
	_destroyBuffers();
}

void ColorBufferReaderWithBufferStorage::_initBuffers()
{
	m_numPBO = std::max(1u, config.frameBufferEmulation.copyToRDRAM);
	if (m_numPBO > _maxPBO)
		m_numPBO = _maxPBO;

	// Generate Pixel Buffer Objects
	glGenBuffers(m_numPBO, m_PBO);
	m_curIndex = 0;

	// Initialize Pixel Buffer Objects
	for (u32 index = 0; index < m_numPBO; ++index) {
		/* The first asynchronous read consumes the previous ring slot. */
		m_bindBuffer->bind(Parameter(GL_PIXEL_PACK_BUFFER), ObjectHandle(m_PBO[index]));
		glBufferStorage(GL_PIXEL_PACK_BUFFER, m_bufferSize, m_pixelData.data(), GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT | GL_CLIENT_STORAGE_BIT);
		m_PBOData[index] = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, m_bufferSize, GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
	}

	m_bindBuffer->bind(Parameter(GL_PIXEL_PACK_BUFFER), ObjectHandle::null);
}

void ColorBufferReaderWithBufferStorage::_destroyBuffers()
{
	glDeleteBuffers(m_numPBO, m_PBO);

	for (u32 index = 0; index < m_numPBO; ++index) {
		if (m_fences[index]) {
			glDeleteSync(m_fences[index]);
			m_fences[index] = nullptr;
		}
		m_PBO[index] = 0;
	}
}

const u8 * ColorBufferReaderWithBufferStorage::_readPixels(const ReadColorBufferParams& _params, u32& _heightOffset,
	u32& _stride)
{
	GLenum format = GLenum(_params.colorFormat);
	GLenum type = GLenum(_params.colorType);

	m_bindBuffer->bind(Parameter(GL_PIXEL_PACK_BUFFER), ObjectHandle(m_PBO[m_curIndex]));

	glReadPixels(_params.x0, _params.y0, m_pTexture->width, _params.height, format, type, nullptr);
	if (m_fences[m_curIndex])
		glDeleteSync(m_fences[m_curIndex]);
	m_fences[m_curIndex] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	if (!m_fences[m_curIndex])
		return nullptr;

	if (!_params.sync)
		m_curIndex = (m_curIndex + 1) % m_numPBO;
	/* Coherent mapping makes completed writes visible; it does not complete
	 * GPU work. Wait only for the ring slot whose bytes the CPU consumes. */
	if (m_fences[m_curIndex]) {
		const GLenum status = glClientWaitSync(m_fences[m_curIndex], GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
		if (status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED)
			return nullptr;
		glDeleteSync(m_fences[m_curIndex]);
		m_fences[m_curIndex] = nullptr;
	}

	_heightOffset = 0;
	_stride = m_pTexture->width;

	return reinterpret_cast<u8*>(m_PBOData[m_curIndex]);
}

void ColorBufferReaderWithBufferStorage::cleanUp()
{
	m_bindBuffer->bind(Parameter(GL_PIXEL_PACK_BUFFER), ObjectHandle::null);
}

std::unique_ptr<ColorBufferReader> ColorBufferReaderWithBufferStorage::clone(CachedTexture * _texture,
	std::unique_ptr<ColorBufferReader> _reuse) const
{
	std::unique_ptr<ColorBufferReaderWithBufferStorage> copy;
	if (_reuse && _reuse->kind() == kind()) {
		auto *reader = static_cast<ColorBufferReaderWithBufferStorage *>(_reuse.get());
		if (reader->m_bufferSize == m_bufferSize && reader->m_numPBO == m_numPBO)
			copy.reset(static_cast<ColorBufferReaderWithBufferStorage *>(_reuse.release()));
	}
	if (!copy)
		copy.reset(new ColorBufferReaderWithBufferStorage(_texture, m_bindBuffer));
	copy->m_pTexture = _texture;
	if (copy->m_numPBO != m_numPBO)
		return std::unique_ptr<ColorBufferReader>();
	for (u32 i = 0; i < m_numPBO; ++i) {
		if (copy->m_fences[i]) {
			glDeleteSync(copy->m_fences[i]);
			copy->m_fences[i] = nullptr;
		}
		if (!m_bindBuffer->copy(ObjectHandle(m_PBO[i]), ObjectHandle(copy->m_PBO[i]), m_bufferSize))
			return std::unique_ptr<ColorBufferReader>();
		copy->m_fences[i] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
		if (!copy->m_fences[i])
			return std::unique_ptr<ColorBufferReader>();
	}
	copy->m_curIndex = m_curIndex;
	copyState(*copy);
	return std::unique_ptr<ColorBufferReader>(copy.release());
}
