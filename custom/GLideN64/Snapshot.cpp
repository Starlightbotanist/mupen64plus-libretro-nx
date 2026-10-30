#include "Snapshot.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <new>
#include <stdexcept>
#include <unordered_map>
#include <vector>
#include <FrameBuffer.h>
#include <DepthBuffer.h>
#include <FrameBufferInfo.h>
#include <DisplayWindow.h>
#include <GBI.h>
#include <RDP.h>
#include <RSP.h>
#include <VI.h>
#include <N64.h>
#include <Config.h>
#include <BufferCopy/ColorBufferToRDRAM.h>
#include <BufferCopy/DepthBufferToRDRAM.h>
#include <BufferCopy/RDRAMtoColorBuffer.h>
#include <Graphics/ColorBufferReader.h>
#include <Graphics/PixelBuffer.h>
#include <Graphics/Context.h>
#include <Graphics/Parameters.h>
#include <Graphics/OpenGLContext/GLFunctions.h>
#include <Graphics/OpenGLContext/ThreadedOpenGl/opengl_Wrapper.h>
#include <uCodes/SnapshotState.h>
#include <glsm/glsm.h>

using namespace graphics;

namespace {
/* GLSM deliberately defers framebuffer binding. Queries must bind the real
 * object and restore the actual bindings without changing GLSM's desired state. */
PFNGLBINDFRAMEBUFFERPROC bindFramebuffer;
PFNGLFRAMEBUFFERTEXTURE2DPROC attachTexture;
PFNGLDRAWBUFFERSPROC drawBuffers;
PFNGLREADBUFFERPROC readBuffer;

struct FramebufferBindings {
	GLint read, draw;
	FramebufferBindings() {
		ptrGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
		ptrGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
	}
	~FramebufferBindings() {
		bindFramebuffer(GL_READ_FRAMEBUFFER, GLuint(read));
		bindFramebuffer(GL_DRAW_FRAMEBUFFER, GLuint(draw));
	}
};
}

static std::array<UcodeSnapshotBlock, 6> ucodeBlocks()
{
	return {{ S2DEX_SnapshotState(), F5Rogue_SnapshotState(),
		T3DUX_SnapshotState(), ZSortBOSS_SnapshotState(),
		F5Indi_SnapshotState(), F5Naboo_SnapshotState() }};
}

/* Semantic state and owned surfaces are copied separately. No resource in a
 * capture aliases a mutable live surface or readback ring. */
class RendererSnapshot
{
public:
	~RendererSnapshot()
	{
		m_overscan.destroy();
		gfxContext.deleteFramebuffer(m_depthCopyFBO);
		textureCache().removeFrameBufferTexture(m_depthCopyColor);
		textureCache().removeFrameBufferTexture(m_depthCopyDepth);
		textureCache().removeFrameBufferTexture(m_rdramTexture);
	}

	bool capture() { return copyFrom(nullptr); }
	bool copy(const RendererSnapshot & _source) { return copyFrom(&_source); }
	void commit();

private:
	using Frames = FrameBufferList::FrameBuffers;
	using Depths = DepthBufferList::DepthBuffers;
	using FrameMap = std::unordered_map<const FrameBuffer *, FrameBuffer *>;
	using DepthMap = std::unordered_map<const DepthBuffer *, DepthBuffer *>;
	using TextureMap = std::unordered_map<GLuint, const CachedTexture *>;
	Frames m_frames;
	Depths m_depths;
	FrameBuffer *m_current = nullptr, *m_copy = nullptr;
	DepthBuffer *m_depthCurrent = nullptr;
	u32 m_previousHeight = 0, m_swapCount = 0;
	FrameBufferList::OverscanBuffer m_overscan;
	FrameBufferList::RdpUpdate m_rdpUpdate;
	decltype(FBInfo::FBInfo::m_readBuffers) m_reads{}, m_writes{};
	bool m_fbInfoSupported = false;
	FrameBuffer *m_colorCurrent = nullptr, *m_depthCopyCurrent = nullptr, *m_rdramCurrent = nullptr;
	u32 m_blueNoise = 0, m_depthCopyFrame = 0;
	ObjectHandle m_depthCopyFBO;
	CachedTexture *m_depthCopyColor = nullptr, *m_depthCopyDepth = nullptr, *m_rdramTexture = nullptr;
	std::unique_ptr<PixelReadBuffer> m_depthReadback;
	std::vector<u8> m_rdramPixels;
	std::vector<u32> m_rdramWrites;
	gDPInfo m_dp;
	gSPInfo m_sp;
	RSPInfo m_rsp;
	VIInfo m_vi;
	u64 m_tmem[512];
	u32 m_rdpWords[8];
	std::vector<u32> m_rdpCommands;
	u32 m_depthClear = 0, m_depthCopyRectFrame = 0;
	GBIInfo::Microcodes m_microcodes;
	MicrocodeInfo *m_microcode = nullptr;
	bool m_hwl = false;
	std::array<std::vector<u8>, 6> m_ucodes;
	decltype(GraphicsDrawer::triangles) m_triangles;
	std::vector<SPVertex> m_vertices;
	u32 m_verticesCount = 0, m_modifyVertices = 0;
	GraphicsDrawer::TexturedRectParams m_texrect{0,0,0,0,0,0,0,0,false,false,false,nullptr};
	RectVertex m_rect[4];
	bool m_flatColors = false, m_bgMode = false;
	GraphicsDrawer::BgDepthCopyMode m_depthMode;

	bool copyFrom(const RendererSnapshot * _source);
	static bool copyTexture(CachedTexture *& _destination, const CachedTexture * _source);
	static void attach(ObjectHandle & _fbo, CachedTexture * _texture, BufferAttachmentParam _attachment);
	static bool copyFrame(FrameBuffer & _destination, const FrameBuffer & _source);
	static bool copyDepth(DepthBuffer & _destination, const DepthBuffer & _source);
	static bool copyFramebuffer(ObjectHandle & _destination, ObjectHandle _source, const TextureMap & _textures);
};

bool RendererSnapshot::copyFramebuffer(ObjectHandle & _destination, ObjectHandle _source, const TextureMap & _textures)
{
	if (!_source.isNotNull() || _source == ObjectHandle::defaultFramebuffer) {
		gfxContext.deleteFramebuffer(_destination);
		_destination = ObjectHandle::null;
		return true;
	}
	if (!_destination.isNotNull() || _destination == ObjectHandle::defaultFramebuffer)
		_destination = gfxContext.createFramebuffer();
	FramebufferBindings bindings;
	bindFramebuffer(GL_FRAMEBUFFER, GLuint(_source));
	GLint colors = 0, draws = 0, read = GL_NONE;
	ptrGetIntegerv(GL_MAX_COLOR_ATTACHMENTS, &colors);
	ptrGetIntegerv(GL_MAX_DRAW_BUFFERS, &draws);
	ptrGetIntegerv(GL_READ_BUFFER, &read);
	if (colors < 1 || draws < 1)
		return false;
	std::vector<GLenum> selections(draws);
	for (GLint i = 0; i < draws; ++i) {
		GLint buffer;
		ptrGetIntegerv(GL_DRAW_BUFFER0 + i, &buffer);
		selections[i] = GLenum(buffer);
	}
	for (GLint i = 0; i < colors + 2; ++i) {
		const GLenum attachment = i < colors ? GL_COLOR_ATTACHMENT0 + i :
			(i == colors ? GL_DEPTH_ATTACHMENT : GL_STENCIL_ATTACHMENT);
		GLint type = GL_NONE, name = 0;
		bindFramebuffer(GL_FRAMEBUFFER, GLuint(_source));
		ptrGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, attachment, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
		const CachedTexture *texture = nullptr;
		if (type != GL_NONE) {
			if (type != GL_TEXTURE) {
				LOG(LOG_WARNING, "Snapshot framebuffer has unsupported attachment type %x", type);
				return false;
			}
			ptrGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, attachment, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &name);
			auto found = _textures.find(GLuint(name));
			if (found == _textures.end()) {
				LOG(LOG_WARNING, "Snapshot framebuffer attachment %u has no texture owner", GLuint(name));
				return false;
			}
			texture = found->second;
		}
		bindFramebuffer(GL_FRAMEBUFFER, GLuint(_destination));
		attachTexture(GL_FRAMEBUFFER, attachment, texture ? texture->storage.target : GL_TEXTURE_2D,
			texture ? GLuint(texture->name) : 0, 0);
	}
	bindFramebuffer(GL_FRAMEBUFFER, GLuint(_destination));
	drawBuffers(draws, selections.data());
	readBuffer(GLenum(read));
	return true;
}

bool RendererSnapshot::copyTexture(CachedTexture *& _destination, const CachedTexture * _source)
{
	if (_source == nullptr) {
		textureCache().removeFrameBufferTexture(_destination);
		_destination = nullptr;
		return true;
	}
	const CachedTexture::Storage & storage = _source->storage;
	if (storage.width == 0 || storage.height == 0 || storage.target == 0)
		return false;
	const bool allocate = _destination == nullptr ||
		std::memcmp(&_destination->storage, &storage, sizeof(storage)) != 0;
	if (_destination != nullptr && _destination->storage.target != storage.target) {
		textureCache().removeFrameBufferTexture(_destination);
		_destination = nullptr;
	}
	if (_destination == nullptr)
		_destination = textureCache().addFrameBufferTexture(Parameter(storage.target));
	const ObjectHandle name = _destination->name;
	std::memcpy(_destination, _source, sizeof(*_source));
	_destination->name = name;
	if (allocate) {
		Context::InitTextureParams params;
		params.handle = name;
		params.target = TextureTargetParam(storage.target);
		params.width = storage.width;
		params.height = storage.height;
		params.msaaLevel = storage.samples;
		params.format = ColorFormatParam(storage.format);
		params.internalFormat = InternalColorFormatParam(storage.internalFormat);
		params.dataType = DatatypeParam(storage.dataType);
		gfxContext.initFramebufferTexture(_destination, params);
		if (storage.samples == 0) {
			Context::TexParameters filter;
			filter.handle = name;
			filter.target = TextureTargetParam(storage.target);
			filter.textureUnitIndex = textureIndices::Tex[0];
			filter.minFilter = textureParameters::FILTER_NEAREST;
			filter.magFilter = textureParameters::FILTER_NEAREST;
			gfxContext.setTextureParameters(filter);
		}
	}
	ptrCopyImageSubData(GLuint(_source->name), storage.target, 0, 0, 0, 0,
		GLuint(name), storage.target, 0, 0, 0, 0, storage.width, storage.height, 1);
	return true;
}

void RendererSnapshot::attach(ObjectHandle & _fbo, CachedTexture * _texture, BufferAttachmentParam _attachment)
{
	if (_texture == nullptr && (!_fbo.isNotNull() || _fbo == ObjectHandle::defaultFramebuffer))
		return;
	if (!_fbo.isNotNull() || _fbo == ObjectHandle::defaultFramebuffer)
		_fbo = gfxContext.createFramebuffer();
	Context::FrameBufferRenderTarget target;
	target.bufferHandle = _fbo;
	target.bufferTarget = bufferTarget::FRAMEBUFFER;
	target.textureHandle = _texture ? _texture->name : ObjectHandle::null;
	target.textureTarget = _texture ? Parameter(_texture->storage.target) : textureTarget::TEXTURE_2D;
	target.attachment = _attachment;
	gfxContext.addFrameBufferRenderTarget(target);
}

bool RendererSnapshot::copyFrame(FrameBuffer & _destination, const FrameBuffer & _source)
{
	static_cast<FrameBufferState &>(_destination) = _source;
	if (!copyTexture(_destination.m_pTexture, _source.m_pTexture) ||
		!copyTexture(_destination.m_pDepthTexture, _source.m_pDepthTexture) ||
		!copyTexture(_destination.m_pResolveTexture, _source.m_pResolveTexture) ||
		!copyTexture(_destination.m_pColorBufferTexture, _source.m_pColorBufferTexture) ||
		!copyTexture(_destination.m_pSubTexture, _source.m_pSubTexture) ||
		!copyTexture(_destination.m_pFrameBufferCopyTexture, _source.m_pFrameBufferCopyTexture))
		return false;
	_destination.m_resolved = _source.m_resolved;
	attach(_destination.m_FBO, _destination.m_pTexture, bufferAttachment::COLOR_ATTACHMENT0);
	attach(_destination.m_depthFBO, _destination.m_pDepthTexture, bufferAttachment::DEPTH_ATTACHMENT);
	attach(_destination.m_resolveFBO, _destination.m_pResolveTexture, bufferAttachment::COLOR_ATTACHMENT0);
	attach(_destination.m_ColorBufferFBO, _destination.m_pColorBufferTexture, bufferAttachment::COLOR_ATTACHMENT0);
	attach(_destination.m_SubFBO, _destination.m_pSubTexture, bufferAttachment::COLOR_ATTACHMENT0);
	attach(_destination.m_copyFBO, _destination.m_pFrameBufferCopyTexture, bufferAttachment::COLOR_ATTACHMENT0);
	if (_source.m_bufferReader) {
		_destination.m_bufferReader = _source.m_bufferReader->clone(_destination.m_pColorBufferTexture,
			std::move(_destination.m_bufferReader));
		if (!_destination.m_bufferReader)
			return false;
	} else
		_destination.m_bufferReader.reset();
	return true;
}

bool RendererSnapshot::copyDepth(DepthBuffer & _destination, const DepthBuffer & _source)
{
	if (_source.m_depthRenderbuffer.isNotNull())
		return false;
	static_cast<DepthBufferState &>(_destination) = _source;
	if (!copyTexture(_destination.m_pDepthBufferTexture, _source.m_pDepthBufferTexture) ||
		!copyTexture(_destination.m_pDepthImageZTexture, _source.m_pDepthImageZTexture) ||
		!copyTexture(_destination.m_pDepthImageDeltaZTexture, _source.m_pDepthImageDeltaZTexture) ||
		!copyTexture(_destination.m_pResolveDepthBufferTexture, _source.m_pResolveDepthBufferTexture) ||
		!copyTexture(_destination.m_pDepthBufferCopyTexture, _source.m_pDepthBufferCopyTexture))
		return false;
	attach(_destination.m_ZTextureClearFBO, _destination.m_pDepthImageZTexture, bufferAttachment::COLOR_ATTACHMENT0);
	attach(_destination.m_DeltaZTextureClearFBO, _destination.m_pDepthImageDeltaZTexture, bufferAttachment::COLOR_ATTACHMENT0);
	attach(_destination.m_copyFBO, _destination.m_pDepthBufferCopyTexture, bufferAttachment::DEPTH_ATTACHMENT);
	return true;
}

bool RendererSnapshot::copyFrom(const RendererSnapshot * _source)
{
	FrameBufferList & frames = frameBufferList();
	DepthBufferList & depths = depthBufferList();
	ColorBufferToRDRAM & colorCopy = ColorBufferToRDRAM::get();
	DepthBufferToRDRAM & depthCopy = DepthBufferToRDRAM::get();
	RDRAMtoColorBuffer & rdramCopy = RDRAMtoColorBuffer::get();
	const Frames & sourceFrames = _source ? _source->m_frames : frames.m_list;
	const Depths & sourceDepths = _source ? _source->m_depths : depths.m_list;
	if (Context::ImageTextures)
		ptrMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT);
	FrameMap frameMap;
	DepthMap depthMap;
	frameMap[nullptr] = nullptr;
	depthMap[nullptr] = nullptr;
	auto destinationDepth = m_depths.begin();
	for (const DepthBuffer & sourceDepth : sourceDepths) {
		if (destinationDepth == m_depths.end()) {
			m_depths.emplace_back();
			destinationDepth = std::prev(m_depths.end());
		}
		if (!copyDepth(*destinationDepth, sourceDepth))
			return false;
		depthMap[&sourceDepth] = &*destinationDepth++;
	}
	m_depths.erase(destinationDepth, m_depths.end());
	auto destinationFrame = m_frames.begin();
	for (const FrameBuffer & sourceFrame : sourceFrames) {
		if (destinationFrame == m_frames.end()) {
			m_frames.emplace_back();
			destinationFrame = std::prev(m_frames.end());
		}
		if (!copyFrame(*destinationFrame, sourceFrame))
			return false;
		auto depth = depthMap.find(sourceFrame.m_pDepthBuffer);
		if (depth == depthMap.end())
			return false;
		destinationFrame->m_pDepthBuffer = depth->second;
		if (depth->second)
			depth->second->setDepthAttachment(destinationFrame->m_FBO, bufferTarget::FRAMEBUFFER);
		else
			attach(destinationFrame->m_FBO, nullptr, bufferAttachment::DEPTH_ATTACHMENT);
		frameMap[&sourceFrame] = &*destinationFrame++;
	}
	m_frames.erase(destinationFrame, m_frames.end());
	auto frame = [&frameMap](const FrameBuffer * _frame) -> FrameBuffer * { return frameMap.at(_frame); };
	m_current = frame(_source ? _source->m_current : frames.m_pCurrent);
	m_copy = frame(_source ? _source->m_copy : frames.m_pCopy);
	m_depthCurrent = depthMap.at(_source ? _source->m_depthCurrent : depths.m_pCurrent);
	m_previousHeight = _source ? _source->m_previousHeight : frames.m_prevColorImageHeight;
	m_rdpUpdate = _source ? _source->m_rdpUpdate : frames.m_rdpUpdate;
	m_swapCount = _source ? _source->m_swapCount : dwnd().m_buffersSwapCount;

	const FrameBufferList::OverscanBuffer & overscan = _source ? _source->m_overscan : frames.m_overscan;
	ObjectHandle overscanFbo = m_overscan.m_FBO;
	CachedTexture *overscanColor = m_overscan.m_pTexture, *overscanDepth = m_overscan.m_pDepthTexture;
	m_overscan = overscan;
	m_overscan.m_FBO = overscanFbo;
	m_overscan.m_pTexture = overscanColor;
	m_overscan.m_pDepthTexture = overscanDepth;
	if (!copyTexture(m_overscan.m_pTexture, overscan.m_pTexture) ||
		!copyTexture(m_overscan.m_pDepthTexture, overscan.m_pDepthTexture))
		return false;
	attach(m_overscan.m_FBO, m_overscan.m_pTexture, bufferAttachment::COLOR_ATTACHMENT0);
	attach(m_overscan.m_FBO, m_overscan.m_pDepthTexture, bufferAttachment::DEPTH_ATTACHMENT);

	for (size_t i = 0; i < m_reads.size(); ++i) {
		m_reads[i] = frame(_source ? _source->m_reads[i] : FBInfo::fbInfo.m_readBuffers[i]);
		m_writes[i] = frame(_source ? _source->m_writes[i] : FBInfo::fbInfo.m_writeBuffers[i]);
	}
	m_fbInfoSupported = _source ? _source->m_fbInfoSupported : FBInfo::fbInfo.m_supported;
	m_colorCurrent = frame(_source ? _source->m_colorCurrent : colorCopy.m_pCurFrameBuffer);
	m_depthCopyCurrent = frame(_source ? _source->m_depthCopyCurrent : depthCopy.m_pCurFrameBuffer);
	m_rdramCurrent = frame(_source ? _source->m_rdramCurrent : rdramCopy.m_pCurBuffer);
	m_blueNoise = _source ? _source->m_blueNoise : ColorBufferToRDRAM::m_blueNoiseIdx;
	m_depthCopyFrame = _source ? _source->m_depthCopyFrame : depthCopy.m_frameCount;
	if (!copyTexture(m_depthCopyColor, _source ? _source->m_depthCopyColor : depthCopy.m_pColorTexture) ||
		!copyTexture(m_depthCopyDepth, _source ? _source->m_depthCopyDepth : depthCopy.m_pDepthTexture) ||
		!copyTexture(m_rdramTexture, _source ? _source->m_rdramTexture : rdramCopy.m_pTexture))
		return false;
	attach(m_depthCopyFBO, m_depthCopyColor, bufferAttachment::COLOR_ATTACHMENT0);
	attach(m_depthCopyFBO, m_depthCopyDepth, bufferAttachment::DEPTH_ATTACHMENT);
	const auto & readback = _source ? _source->m_depthReadback : depthCopy.m_pbuf;
	m_depthReadback = readback ? readback->clone(std::move(m_depthReadback)) : std::unique_ptr<PixelReadBuffer>();
	if (readback && !m_depthReadback)
		return false;
	m_rdramWrites = _source ? _source->m_rdramWrites : rdramCopy.m_vecAddress;
	if (_source)
		m_rdramPixels = _source->m_rdramPixels;
	else if (rdramCopy.m_pbuf)
		m_rdramPixels.assign(rdramCopy.m_pbuf, rdramCopy.m_pbuf + rdramCopy.m_pTexture->textureBytes);
	else
		m_rdramPixels.clear();

	/* Copy actual attachments and read/draw selections, including the additional
	 * depth-compare targets. Reconstructing a guessed default FBO loses state. */
	TextureMap textures;
	auto mapTexture = [&textures](const CachedTexture *source, const CachedTexture *destination) {
		if (source)
			textures[GLuint(source->name)] = destination;
	};
	auto sourceDepth = sourceDepths.begin();
	for (const DepthBuffer & depth : m_depths) {
		mapTexture(sourceDepth->m_pDepthBufferTexture, depth.m_pDepthBufferTexture);
		mapTexture(sourceDepth->m_pDepthImageZTexture, depth.m_pDepthImageZTexture);
		mapTexture(sourceDepth->m_pDepthImageDeltaZTexture, depth.m_pDepthImageDeltaZTexture);
		mapTexture(sourceDepth->m_pResolveDepthBufferTexture, depth.m_pResolveDepthBufferTexture);
		mapTexture(sourceDepth->m_pDepthBufferCopyTexture, depth.m_pDepthBufferCopyTexture);
		++sourceDepth;
	}
	auto sourceFrame = sourceFrames.begin();
	for (const FrameBuffer & buffer : m_frames) {
		mapTexture(sourceFrame->m_pTexture, buffer.m_pTexture);
		mapTexture(sourceFrame->m_pDepthTexture, buffer.m_pDepthTexture);
		mapTexture(sourceFrame->m_pResolveTexture, buffer.m_pResolveTexture);
		mapTexture(sourceFrame->m_pColorBufferTexture, buffer.m_pColorBufferTexture);
		mapTexture(sourceFrame->m_pSubTexture, buffer.m_pSubTexture);
		mapTexture(sourceFrame->m_pFrameBufferCopyTexture, buffer.m_pFrameBufferCopyTexture);
		++sourceFrame;
	}
	mapTexture(overscan.m_pTexture, m_overscan.m_pTexture);
	mapTexture(overscan.m_pDepthTexture, m_overscan.m_pDepthTexture);
	mapTexture(_source ? _source->m_depthCopyColor : depthCopy.m_pColorTexture, m_depthCopyColor);
	mapTexture(_source ? _source->m_depthCopyDepth : depthCopy.m_pDepthTexture, m_depthCopyDepth);
	mapTexture(_source ? _source->m_rdramTexture : rdramCopy.m_pTexture, m_rdramTexture);
	sourceFrame = sourceFrames.begin();
	for (FrameBuffer & buffer : m_frames) {
		if (!copyFramebuffer(buffer.m_FBO, sourceFrame->m_FBO, textures) ||
			!copyFramebuffer(buffer.m_depthFBO, sourceFrame->m_depthFBO, textures) ||
			!copyFramebuffer(buffer.m_resolveFBO, sourceFrame->m_resolveFBO, textures) ||
			!copyFramebuffer(buffer.m_ColorBufferFBO, sourceFrame->m_ColorBufferFBO, textures) ||
			!copyFramebuffer(buffer.m_SubFBO, sourceFrame->m_SubFBO, textures) ||
			!copyFramebuffer(buffer.m_copyFBO, sourceFrame->m_copyFBO, textures))
			return false;
		++sourceFrame;
	}
	sourceDepth = sourceDepths.begin();
	for (DepthBuffer & depth : m_depths) {
		if (!copyFramebuffer(depth.m_ZTextureClearFBO, sourceDepth->m_ZTextureClearFBO, textures) ||
			!copyFramebuffer(depth.m_DeltaZTextureClearFBO, sourceDepth->m_DeltaZTextureClearFBO, textures) ||
			!copyFramebuffer(depth.m_copyFBO, sourceDepth->m_copyFBO, textures))
			return false;
		++sourceDepth;
	}
	if (!copyFramebuffer(m_overscan.m_FBO, overscan.m_FBO, textures) ||
		!copyFramebuffer(m_depthCopyFBO, _source ? _source->m_depthCopyFBO : depthCopy.m_FBO, textures))
		return false;

	GraphicsDrawer & drawer = dwnd().getDrawer();
	m_dp = _source ? _source->m_dp : gDP;
	m_sp = _source ? _source->m_sp : gSP;
	m_rsp = _source ? _source->m_rsp : RSP;
	m_vi = _source ? _source->m_vi : VI;
	std::memcpy(m_tmem, _source ? _source->m_tmem : TMEM, sizeof(m_tmem));
	if (_source) {
		std::memcpy(m_rdpWords, _source->m_rdpWords, sizeof(m_rdpWords));
		m_rdpCommands = _source->m_rdpCommands;
	} else {
		if (RDP.cmd_ptr > MAXCMD + 32 || RDP.cmd_cur > RDP.cmd_ptr)
			return false;
		u32 words[] = { RDP.w0, RDP.w1, RDP.w2, RDP.w3, RDP.cmd_ptr, RDP.cmd_cur,
			RDP.lastLoadBlock[0], RDP.lastLoadBlock[1] };
		std::memcpy(m_rdpWords, words, sizeof(words));
		m_rdpCommands.assign(RDP.cmd_data, RDP.cmd_data + RDP.cmd_ptr);
	}
	m_depthClear = _source ? _source->m_depthClear : DepthClearColor;
	m_depthCopyRectFrame = _source ? _source->m_depthCopyRectFrame : rectDepthBufferCopyFrame;
	/* Detection is cached by guest address, whose contents can change across
	 * timelines. Preserve the cache as well as the selected decoder. */
	const auto &microcodes = _source ? _source->m_microcodes : GBI.m_list;
	const auto *selected = _source ? _source->m_microcode : GBI.m_pCurrent;
	m_microcodes = microcodes;
	m_microcode = nullptr;
	auto copy = m_microcodes.begin();
	for (const auto &entry : microcodes) {
		if (&entry == selected) {
			m_microcode = &*copy;
			break;
		}
		++copy;
	}
	m_hwl = _source ? _source->m_hwl : GBI.m_hwlSupported;
	const auto blocks = ucodeBlocks();
	for (size_t i = 0; i < blocks.size(); ++i) {
		if (_source)
			m_ucodes[i] = _source->m_ucodes[i];
		else {
			const auto *data = static_cast<const u8 *>(blocks[i].data);
			m_ucodes[i].assign(data, data + blocks[i].size);
		}
	}
	m_triangles = _source ? _source->m_triangles : drawer.triangles;
	m_vertices = _source ? _source->m_vertices : drawer.m_dmaVertices;
	m_verticesCount = _source ? _source->m_verticesCount : drawer.m_dmaVerticesNum;
	m_modifyVertices = _source ? _source->m_modifyVertices : drawer.m_modifyVertices;
	m_texrect = _source ? _source->m_texrect : drawer.m_texrectParams;
	m_texrect.pBuffer = frame(m_texrect.pBuffer);
	std::memcpy(m_rect, _source ? _source->m_rect : drawer.m_rect, sizeof(m_rect));
	m_flatColors = _source ? _source->m_flatColors : drawer.m_bFlatColors;
	m_bgMode = _source ? _source->m_bgMode : drawer.m_bBGMode;
	m_depthMode = _source ? _source->m_depthMode : drawer.m_depthCopyMode;
	return true;
}

void RendererSnapshot::commit()
{
	FrameBufferList & frames = frameBufferList();
	DepthBufferList & depths = depthBufferList();
	GraphicsDrawer & drawer = dwnd().getDrawer();
	ColorBufferToRDRAM & colorCopy = ColorBufferToRDRAM::get();
	DepthBufferToRDRAM & depthCopy = DepthBufferToRDRAM::get();
	RDRAMtoColorBuffer & rdramCopy = RDRAMtoColorBuffer::get();
	using std::swap;
	m_frames.swap(frames.m_list);
	m_depths.swap(depths.m_list);
	swap(m_current, frames.m_pCurrent);
	swap(m_copy, frames.m_pCopy);
	swap(m_depthCurrent, depths.m_pCurrent);
	swap(m_previousHeight, frames.m_prevColorImageHeight);
	swap(m_rdpUpdate, frames.m_rdpUpdate);
	swap(m_overscan, frames.m_overscan);
	swap(m_swapCount, dwnd().m_buffersSwapCount);
	swap(m_reads, FBInfo::fbInfo.m_readBuffers);
	swap(m_writes, FBInfo::fbInfo.m_writeBuffers);
	swap(m_fbInfoSupported, FBInfo::fbInfo.m_supported);
	swap(m_colorCurrent, colorCopy.m_pCurFrameBuffer);
	swap(m_blueNoise, ColorBufferToRDRAM::m_blueNoiseIdx);
	swap(m_depthCopyCurrent, depthCopy.m_pCurFrameBuffer);
	swap(m_depthCopyFrame, depthCopy.m_frameCount);
	swap(m_depthCopyFBO, depthCopy.m_FBO);
	swap(m_depthCopyColor, depthCopy.m_pColorTexture);
	swap(m_depthCopyDepth, depthCopy.m_pDepthTexture);
	m_depthReadback.swap(depthCopy.m_pbuf);
	swap(m_rdramCurrent, rdramCopy.m_pCurBuffer);
	swap(m_rdramTexture, rdramCopy.m_pTexture);
	m_rdramWrites.swap(rdramCopy.m_vecAddress);
	if (!m_rdramPixels.empty())
		std::memcpy(rdramCopy.m_pbuf, m_rdramPixels.data(), m_rdramPixels.size());
	m_microcodes.swap(GBI.m_list);
	GBI.m_pCurrent = nullptr;
	if (m_microcode)
		GBI._makeCurrent(m_microcode);
	else
		GBI._flushCommands();
	GBI.m_hwlSupported = m_hwl;
	gSPSetupFunctions();
	const auto blocks = ucodeBlocks();
	for (size_t i = 0; i < blocks.size(); ++i)
		std::memcpy(blocks[i].data, m_ucodes[i].data(), blocks[i].size);
	gDP = m_dp;
	gSP = m_sp;
	RSP = m_rsp;
	VI = m_vi;
	std::memcpy(TMEM, m_tmem, sizeof(m_tmem));
	RDP.w0 = m_rdpWords[0]; RDP.w1 = m_rdpWords[1];
	RDP.w2 = m_rdpWords[2]; RDP.w3 = m_rdpWords[3];
	RDP.cmd_ptr = m_rdpWords[4]; RDP.cmd_cur = m_rdpWords[5];
	RDP.lastLoadBlock[0] = m_rdpWords[6]; RDP.lastLoadBlock[1] = m_rdpWords[7];
	if (!m_rdpCommands.empty())
		std::memcpy(RDP.cmd_data, m_rdpCommands.data(), m_rdpCommands.size() * sizeof(u32));
	DepthClearColor = m_depthClear;
	rectDepthBufferCopyFrame = m_depthCopyRectFrame;
	drawer.triangles = m_triangles;
	drawer.m_dmaVertices.swap(m_vertices);
	drawer.m_dmaVerticesNum = m_verticesCount;
	drawer.m_modifyVertices = m_modifyVertices;
	drawer.m_texrectParams = m_texrect;
	std::memcpy(drawer.m_rect, m_rect, sizeof(m_rect));
	drawer.m_bFlatColors = m_flatColors;
	drawer.m_bBGMode = m_bgMode;
	drawer.m_depthCopyMode = m_depthMode;
	/* Decoded textures are keyed by restored TMEM/RDRAM contents and texture
	 * parameters. Retain that bounded cache; only its live bindings can refer
	 * to surfaces from the discarded timeline. */
	textureCache().current[0] = textureCache().current[1] = nullptr;
}

namespace {
struct SnapshotHandle {
	std::unique_ptr<RendererSnapshot> state;
	SnapshotHandle *next = nullptr;
};
SnapshotHandle *handles;
std::unique_ptr<RendererSnapshot> staging;
bool failed;
bool contextReady;

bool collectErrors()
{
	bool clean = true;
	for (unsigned i = 0; i < 32; ++i) {
		const GLenum error = ptrGetError();
		if (error == GL_NO_ERROR)
			return clean;
		clean = false;
		LOG(LOG_WARNING, "Snapshot GL error %x", error);
		if (error == GL_OUT_OF_MEMORY || error == GL_CONTEXT_LOST) {
			failed = true;
			return false;
		}
	}
	failed = true;
	return false;
}

void restoreBindings()
{
	gfxContext.invalidateState();
	frameBufferList().setCurrentDrawBuffer();
	FrameBuffer *current = frameBufferList().getCurrent();
	if (current && current->m_pDepthBuffer && config.frameBufferEmulation.N64DepthCompare != Config::dcDisable)
		current->m_pDepthBuffer->bindDepthImageTexture(current->m_FBO);
	dwnd().getDrawer().dropRenderState();
	/* Invalidate derived GL state without inventing CPU framebuffer writes
	 * or replacing an explicitly loaded combined matrix. */
	gDP.changed |= CHANGED_RENDERMODE | CHANGED_CYCLETYPE | CHANGED_SCISSOR |
		CHANGED_TMEM | CHANGED_TILE | CHANGED_COMBINE | CHANGED_ALPHACOMPARE |
		CHANGED_FOGCOLOR | CHANGED_BLENDCOLOR | CHANGED_FB_TEXTURE;
	gSP.changed |= CHANGED_VIEWPORT | CHANGED_TEXTURE | CHANGED_GEOMETRYMODE |
		CHANGED_FOGPOSITION | CHANGED_TEXTURESCALE | CHANGED_HW_LIGHT;
}

retro_snapshot_result copyState(SnapshotHandle * target, const SnapshotHandle * source)
{
	if (failed)
		return RETRO_SNAPSHOT_FATAL;
	collectErrors(); // Ordinary inherited errors do not belong to this operation.
	if (failed)
		return RETRO_SNAPSHOT_FATAL;
	bool okay = false;
	try {
		if (!staging)
			staging.reset(new RendererSnapshot);
		okay = source ? staging->copy(*source->state) : staging->capture();
		if (source)
			ptrFinish(); // Restored mapped readback history may be consumed by the CPU.
	} catch (const std::bad_alloc &) {
		okay = false;
	} catch (const std::out_of_range &) {
		okay = false;
	}
	okay = collectErrors() && okay;
	restoreBindings();
	if (failed)
		return RETRO_SNAPSHOT_FATAL;
	if (!okay)
		return RETRO_SNAPSHOT_UNAVAILABLE;
	if (target)
		target->state.swap(staging);
	return RETRO_SNAPSHOT_SUCCESS;
}
}

extern "C" bool gln64_snapshot_supported(void)
{
	return contextReady && Context::CopyImage && Context::DepthFramebufferTextures && !Context::EglImage &&
		ptrCopyImageSubData != nullptr && ptrCopyBufferSubData != nullptr &&
		ptrGetFramebufferAttachmentParameteriv != nullptr &&
		bindFramebuffer && attachTexture && drawBuffers && readBuffer;
}

extern "C" void *gln64_snapshot_create(void)
{
	auto *handle = new (std::nothrow) SnapshotHandle;
	if (handle) {
		handle->next = handles;
		handles = handle;
	}
	return handle;
}

extern "C" void gln64_snapshot_release(void *snapshot)
{
	auto *handle = static_cast<SnapshotHandle *>(snapshot);
	SnapshotHandle **link = &handles;
	while (*link && *link != handle)
		link = &(*link)->next;
	if (*link) {
		*link = handle->next;
		delete handle;
	}
}

extern "C" bool gln64_snapshot_valid(const void *snapshot)
{
	const auto *handle = static_cast<const SnapshotHandle *>(snapshot);
	return contextReady && !failed && handle && handle->state;
}

extern "C" void gln64_snapshot_context_destroy(void)
{
	contextReady = false;
	for (auto *handle = handles; handle; handle = handle->next)
		handle->state.reset();
	staging.reset();
}

static void resetContextFunctions()
{
	bindFramebuffer = reinterpret_cast<PFNGLBINDFRAMEBUFFERPROC>(glsm_get_proc_address("glBindFramebuffer"));
	attachTexture = reinterpret_cast<PFNGLFRAMEBUFFERTEXTURE2DPROC>(glsm_get_proc_address("glFramebufferTexture2D"));
	drawBuffers = reinterpret_cast<PFNGLDRAWBUFFERSPROC>(glsm_get_proc_address("glDrawBuffers"));
	readBuffer = reinterpret_cast<PFNGLREADBUFFERPROC>(glsm_get_proc_address("glReadBuffer"));
	const bool recovering = failed;
	failed = false;
	contextReady = true;
	if (recovering)
		collectErrors();
}

extern "C" void gln64_snapshot_context_reset(void)
{
	/* TextureCache initialization can run on the emulation thread. Resolve
	 * context-specific entry points on the actual GL owner. */
	opengl::FunctionWrapper::runOnContextThread(resetContextFunctions);
}

extern "C" void gln64_snapshot_new_instance(void)
{
	failed = false;
	contextReady = false;
}

extern "C" void gln64_snapshot_clear(void)
{
	staging.reset();
}

extern "C" bool gln64_snapshot_failed(void) { return failed; }

extern "C" retro_snapshot_result gln64_snapshot_flush(void)
{
	if (failed)
		return RETRO_SNAPSHOT_FATAL;
	collectErrors();
	if (failed)
		return RETRO_SNAPSHOT_FATAL;
	dwnd().getDrawer().drawTriangles();
	dwnd().getDrawer().flush();
	if (!collectErrors()) {
		failed = true;
		return RETRO_SNAPSHOT_FATAL;
	}
	return RETRO_SNAPSHOT_SUCCESS;
}

extern "C" retro_snapshot_result gln64_snapshot_save(void *snapshot)
{
	return copyState(static_cast<SnapshotHandle *>(snapshot), nullptr);
}

extern "C" retro_snapshot_result gln64_snapshot_prepare(const void *snapshot)
{
	const auto *handle = static_cast<const SnapshotHandle *>(snapshot);
	if (handle == nullptr || !handle->state)
		return RETRO_SNAPSHOT_UNAVAILABLE;
	return copyState(nullptr, handle);
}

extern "C" retro_snapshot_result gln64_snapshot_commit(void)
{
	staging->commit();
	restoreBindings();
	if (!collectErrors())
		failed = true;
	return failed ? RETRO_SNAPSHOT_FATAL : RETRO_SNAPSHOT_SUCCESS;
}
