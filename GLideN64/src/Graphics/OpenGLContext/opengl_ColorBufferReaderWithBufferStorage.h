#pragma once

#include <Graphics/ColorBufferReader.h>
#include "opengl_CachedFunctions.h"
#include <Graphics/Parameter.h>

namespace opengl {

	class ColorBufferReaderWithBufferStorage :
		public graphics::ColorBufferReader
	{
	public:
		ColorBufferReaderWithBufferStorage(CachedTexture * _pTexture,
			CachedBindBuffer * _bindBuffer);
		virtual ~ColorBufferReaderWithBufferStorage();

		const u8 * _readPixels(const ReadColorBufferParams& _params, u32& _heightOffset, u32& _stride) override;

		void cleanUp() override;
		Kind kind() const override { return Kind::BufferStorage; }
		std::unique_ptr<graphics::ColorBufferReader> clone(CachedTexture * _texture,
			std::unique_ptr<graphics::ColorBufferReader> _reuse) const override;

	private:
		void _initBuffers();
		void _destroyBuffers();

		CachedBindBuffer * m_bindBuffer;
		size_t m_bufferSize;

		static const int _maxPBO = 3;
		u32 m_numPBO;
		GLuint m_PBO[_maxPBO];
		void* m_PBOData[_maxPBO];
		GLsync m_fences[_maxPBO] = {};
		u32 m_curIndex;
	};

}
