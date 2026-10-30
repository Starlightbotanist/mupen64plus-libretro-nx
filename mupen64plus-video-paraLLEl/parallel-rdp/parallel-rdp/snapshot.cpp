#include "snapshot.hpp"
#include <cstring>

namespace RDP
{
void Snapshot::live_buffers(CommandProcessor &p, Vulkan::Buffer *(&result)[NumBuffers])
{
	result[0] = p.rdram.get();
	result[1] = p.hidden_rdram.get();
	result[2] = p.tmem.get();
	result[3] = p.renderer.upscaling_reference_rdram.get();
	result[4] = p.renderer.upscaling_multisampled_rdram.get();
	result[5] = p.renderer.upscaling_multisampled_hidden_rdram.get();
}

bool Snapshot::submit(Vulkan::Device &device, Vulkan::CommandBufferHandle &cmd)
{
	Vulkan::Fence fence;
	device.submit(cmd, &fence);
	if (device.get_error() != VK_SUCCESS || !fence)
		return false;
	fence->wait();
	return device.get_error() == VK_SUCCESS;
}

bool Snapshot::capture(CommandProcessor &p)
{
	std::lock_guard<std::mutex> lock(p.renderer.idle_lock);
	Vulkan::Buffer *live[NumBuffers];
	live_buffers(p, live);
	// The integration uses immutable, shader-readable scanout images. Keeping a
	// reference preserves the previous field without copying or rewinding the
	// frontend's presentation slots. Exported images have a different owner.
	if (p.vi.prev_image_is_external || (p.vi.prev_scanout_image &&
	    p.vi.prev_image_layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL))
		return false;
	for (unsigned i = 0; i < NumBuffers; i++)
	{
		if (!live[i])
		{
			buffers[i].reset();
			continue;
		}
		if (!buffers[i] || buffers[i]->get_create_info().size != live[i]->get_create_info().size)
		{
			Vulkan::BufferCreateInfo info = {};
			info.size = live[i]->get_create_info().size;
			info.domain = Vulkan::BufferDomain::Device;
			info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
			buffers[i] = p.device.create_buffer(info);
			if (!buffers[i])
				return false;
		}
	}
	auto cmd = p.device.request_command_buffer();
	cmd->barrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT,
	             VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT,
	             VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
	for (unsigned i = 0; i < NumBuffers; i++)
		if (live[i])
			cmd->copy_buffer(*buffers[i], *live[i]);
	cmd->barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
	             VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
	if (!submit(p.device, cmd))
		return false;

	scissor = p.scissor_state;
	raster = p.static_state;
	depth_blend = p.depth_blend;
	texture_image = p.texture_image;
	quirks = p.quirks;
	auto &r = p.renderer;
	framebuffer = r.fb;
	constants = r.constants;
	stream_scissor = r.stream.scissor_state;
	stream_raster = r.stream.static_raster_state;
	stream_depth_blend = r.stream.depth_blend_state;
	std::memcpy(tiles, r.tiles, sizeof(tiles));
	primitive_index = r.base_primitive_index;
	auto &vi = p.vi;
	std::memcpy(vi_registers, vi.vi_registers, sizeof(vi_registers));
	per_line_state = vi.per_line_state;
	frame_count = vi.frame_count;
	last_valid_frame_count = vi.last_valid_frame_count;
	previous_frame_blank = vi.previous_frame_blank;
	previous_image = vi.prev_scanout_image;
	return true;
}

bool Snapshot::prepare(CommandProcessor &p, Vulkan::CommandBufferHandle &cmd) const
{
	std::lock_guard<std::mutex> lock(p.renderer.idle_lock);
	Vulkan::Buffer *live[NumBuffers];
	live_buffers(p, live);
	for (unsigned i = 0; i < NumBuffers; i++)
		if (bool(live[i]) != bool(buffers[i]) || (live[i] &&
		    live[i]->get_create_info().size != buffers[i]->get_create_info().size))
			return false;
	cmd = p.device.request_command_buffer();
	cmd->barrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT,
	             VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT,
	             VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
	for (unsigned i = 0; i < NumBuffers; i++)
		if (live[i])
			cmd->copy_buffer(*live[i], *buffers[i]);
	cmd->barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
	             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT,
	             VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT |
	             VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT);
	return p.device.get_error() == VK_SUCCESS;
}

bool Snapshot::commit(CommandProcessor &p, Vulkan::CommandBufferHandle &cmd) const
{
	std::lock_guard<std::mutex> lock(p.renderer.idle_lock);
	if (!submit(p.device, cmd))
		return false;
	p.scissor_state = scissor;
	p.static_state = raster;
	p.depth_blend = depth_blend;
	p.texture_image = texture_image;
	p.quirks = quirks;
	auto &r = p.renderer;
	// All deferred layers have completed. Invalidate derived stream indices;
	// never rewind allocation rings, worker counters or synchronization objects.
	r.reset_context();
	r.fb = framebuffer;
	r.constants = constants;
	r.stream.scissor_state = stream_scissor;
	r.stream.static_raster_state = stream_raster;
	r.stream.depth_blend_state = stream_depth_blend;
	std::memcpy(r.tiles, tiles, sizeof(tiles));
	r.base_primitive_index = primitive_index;
	auto &vi = p.vi;
	std::memcpy(vi.vi_registers, vi_registers, sizeof(vi_registers));
	vi.per_line_state = per_line_state;
	vi.frame_count = frame_count;
	vi.last_valid_frame_count = last_valid_frame_count;
	vi.previous_frame_blank = previous_frame_blank;
	vi.prev_scanout_image = previous_image;
	vi.prev_image_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	vi.prev_image_is_external = false;
	return true;
}
}
