// Included by rdp.cpp inside RDP: the command parser and presentation lifetime
// belong to this integration, rather than the reusable command processor.
struct IntegrationSnapshot
{
	IntegrationSnapshot *next = nullptr;
	std::unique_ptr<Snapshot> state;
	uint64_t generation = 0;
	// The largest RDP command is 22 64-bit words. Completed commands are
	// already owned by CommandProcessor; retain only the unfinished tail.
	uint32_t partial[44];
	int ptr = 0;
	unsigned width = 0, height = 0;
};

static IntegrationSnapshot *snapshot_handles;
static uint64_t snapshot_generation = 1;
static std::unique_ptr<Snapshot> snapshot_staging;
static Vulkan::CommandBufferHandle snapshot_commands;
static const IntegrationSnapshot *snapshot_restore;
static bool snapshot_mutation_failed;

static bool snapshot_supported() { return bool(frontend) && bool(device); }
static bool snapshot_failed()
{
	return snapshot_mutation_failed || (device && device->get_error() != VK_SUCCESS);
}

static void discard_snapshot_commands()
{
	if (snapshot_commands)
		device->submit_discard(snapshot_commands);
	snapshot_restore = nullptr;
}

static void *snapshot_create()
{
	auto *handle = new (std::nothrow) IntegrationSnapshot;
	if (handle)
	{
		handle->next = snapshot_handles;
		snapshot_handles = handle;
	}
	return handle;
}

static void snapshot_release(void *opaque)
{
	auto *handle = static_cast<IntegrationSnapshot *>(opaque);
	auto **link = &snapshot_handles;
	while (*link && *link != handle)
		link = &(*link)->next;
	if (*link)
	{
		if (snapshot_restore == handle)
			discard_snapshot_commands();
		*link = handle->next;
		delete handle;
	}
}

static bool snapshot_valid(const void *opaque)
{
	const auto *handle = static_cast<const IntegrationSnapshot *>(opaque);
	return snapshot_supported() && !snapshot_failed() && handle && handle->state &&
	       handle->generation == snapshot_generation;
}

static void snapshot_clear()
{
	discard_snapshot_commands();
	snapshot_staging.reset();
}

static void snapshot_context_destroy()
{
	snapshot_clear();
	for (auto *handle = snapshot_handles; handle; handle = handle->next)
		handle->state.reset();
	++snapshot_generation;
	snapshot_mutation_failed = false;
}

static retro_snapshot_result snapshot_flush()
{
	if (snapshot_failed())
		return RETRO_SNAPSHOT_FATAL;
	if (!snapshot_supported())
		return RETRO_SNAPSHOT_UNAVAILABLE;
	// Complete both the command ring and the GPU-to-CPU coherency worker before
	// the shared core captures or overwrites guest memory. No guest instruction.
	try { frontend->idle(); }
	catch (const std::bad_alloc &) { snapshot_mutation_failed = true; }
	return snapshot_failed() ? RETRO_SNAPSHOT_FATAL : RETRO_SNAPSHOT_SUCCESS;
}

static retro_snapshot_result snapshot_save(void *opaque)
{
	auto *handle = static_cast<IntegrationSnapshot *>(opaque);
	int remaining = cmd_ptr - cmd_cur;
	if (cmd_cur < 0 || remaining < 0 || remaining > 22)
		return RETRO_SNAPSHOT_UNAVAILABLE;
	try
	{
		if (!snapshot_staging)
			snapshot_staging.reset(new Snapshot);
		if (!snapshot_staging->capture(*frontend))
			return snapshot_failed() ? RETRO_SNAPSHOT_FATAL : RETRO_SNAPSHOT_UNAVAILABLE;
	}
	catch (const std::bad_alloc &)
	{
		return snapshot_failed() ? RETRO_SNAPSHOT_FATAL : RETRO_SNAPSHOT_UNAVAILABLE;
	}
	handle->state.swap(snapshot_staging);
	handle->generation = snapshot_generation;
	handle->ptr = remaining;
	std::memcpy(handle->partial, cmd_data + cmd_cur * 2, (size_t)remaining * 2 * sizeof(uint32_t));
	handle->width = width;
	handle->height = height;
	return RETRO_SNAPSHOT_SUCCESS;
}

static retro_snapshot_result snapshot_prepare(const void *opaque)
{
	discard_snapshot_commands();
	if (!snapshot_valid(opaque))
		return snapshot_failed() ? RETRO_SNAPSHOT_FATAL : RETRO_SNAPSHOT_UNAVAILABLE;
	auto result = snapshot_flush();
	if (result != RETRO_SNAPSHOT_SUCCESS)
		return result;
	const auto *handle = static_cast<const IntegrationSnapshot *>(opaque);
	try
	{
		if (!handle->state->prepare(*frontend, snapshot_commands))
		{
			discard_snapshot_commands();
			return snapshot_failed() ? RETRO_SNAPSHOT_FATAL : RETRO_SNAPSHOT_UNAVAILABLE;
		}
	}
	catch (const std::bad_alloc &)
	{
		discard_snapshot_commands();
		return snapshot_failed() ? RETRO_SNAPSHOT_FATAL : RETRO_SNAPSHOT_UNAVAILABLE;
	}
	snapshot_restore = handle;
	return RETRO_SNAPSHOT_SUCCESS;
}

static retro_snapshot_result snapshot_commit()
{
	const auto *handle = snapshot_restore;
	snapshot_restore = nullptr;
	try
	{
		if (handle && handle->state->commit(*frontend, snapshot_commands))
		{
			cmd_cur = 0;
			cmd_ptr = handle->ptr;
			std::memcpy(cmd_data, handle->partial, (size_t)cmd_ptr * 2 * sizeof(uint32_t));
			width = handle->width;
			height = handle->height;
			return RETRO_SNAPSHOT_SUCCESS;
		}
	}
	catch (const std::bad_alloc &) {}
	snapshot_mutation_failed = true;
	return RETRO_SNAPSHOT_FATAL;
}
