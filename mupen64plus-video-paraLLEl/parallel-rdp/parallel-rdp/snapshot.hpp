#pragma once

#include "rdp_device.hpp"

namespace RDP
{
// A timeline owns RDP registers, native/enhanced memory and VI history. Device
// queues, fences, frame contexts and coherency workers remain process resources.
class Snapshot
{
public:
	bool capture(CommandProcessor &processor);
	bool prepare(CommandProcessor &processor, Vulkan::CommandBufferHandle &cmd) const;
	bool commit(CommandProcessor &processor, Vulkan::CommandBufferHandle &cmd) const;

private:
	static constexpr unsigned NumBuffers = 6;
	Vulkan::BufferHandle buffers[NumBuffers];
	static void live_buffers(CommandProcessor &processor, Vulkan::Buffer *(&result)[NumBuffers]);
	static bool submit(Vulkan::Device &device, Vulkan::CommandBufferHandle &cmd);

	decltype(CommandProcessor::scissor_state) scissor;
	decltype(CommandProcessor::static_state) raster;
	decltype(CommandProcessor::depth_blend) depth_blend;
	decltype(CommandProcessor::texture_image) texture_image;
	decltype(CommandProcessor::quirks) quirks;
	decltype(Renderer::fb) framebuffer;
	decltype(Renderer::constants) constants;
	decltype(Renderer::stream.scissor_state) stream_scissor;
	decltype(Renderer::stream.static_raster_state) stream_raster;
	decltype(Renderer::stream.depth_blend_state) stream_depth_blend;
	TileInfo tiles[Limits::MaxNumTiles];
	uint32_t primitive_index;
	uint32_t vi_registers[unsigned(VIRegister::Count)];
	decltype(VideoInterface::per_line_state) per_line_state;
	uint32_t frame_count, last_valid_frame_count;
	bool previous_frame_blank;
	Vulkan::ImageHandle previous_image;
};
}
