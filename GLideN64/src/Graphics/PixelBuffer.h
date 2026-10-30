#pragma once
#include "Parameter.h"
#include <memory>

namespace graphics {

	class PixelReadBuffer
	{
	public:
		virtual ~PixelReadBuffer() {}
		virtual void readPixels(s32 _x,s32 _y, u32 _width, u32 _height, Parameter _format, Parameter _type) = 0;
		virtual void * getDataRange(u32 _offset, u32 _range) = 0;
		virtual void closeReadBuffer() = 0;
		virtual void bind() = 0;
		virtual void unbind() = 0;
		enum class Kind { Unsupported, PixelBuffer };
		virtual Kind kind() const { return Kind::Unsupported; }
		virtual std::unique_ptr<PixelReadBuffer> clone(std::unique_ptr<PixelReadBuffer> _reuse) const
		{
			return std::unique_ptr<PixelReadBuffer>();
		}
	};

	template<class T>
	class PixelBufferBinder
	{
	public:
		PixelBufferBinder(T * _buffer)
			: m_buffer(_buffer) {
			m_buffer->bind();
		}

		~PixelBufferBinder() {
			m_buffer->unbind();
			m_buffer = nullptr;
		}
	private:
		T * m_buffer;
	};
}
