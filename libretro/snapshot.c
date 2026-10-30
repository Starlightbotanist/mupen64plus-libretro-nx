#include "runtime_snapshot.h"
#include "rdp_snapshot.h"
#include "libretro_memory.h"
#include "mupen64plus-next_common.h"
#include "GLideN64/Snapshot.h"
#include "device/device.h"
#include "device/r4300/interrupt.h"
#include "main/main.h"
#include "main/savestates.h"
#include "plugin/emulate_game_controller_via_input_plugin.h"
#include "plugin/plugin.h"
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <glsm/glsmsym.h>

struct snapshot_devices
{
    struct rdp_core dp;
    struct rsp_core sp;
    struct ai_controller ai;
    struct mi_controller mi;
    struct pi_controller pi;
    struct ri_controller ri;
    struct si_controller si;
    struct vi_controller vi;
    struct pif pif;
    struct cart cart;
    struct is_viewer is;
    struct game_controller controllers[GAME_CONTROLLERS_COUNT];
    struct biopak biopaks[GAME_CONTROLLERS_COUNT];
    struct mempak mempaks[GAME_CONTROLLERS_COUNT];
    struct rumblepak rumblepaks[GAME_CONTROLLERS_COUNT];
    struct transferpak transferpaks[GAME_CONTROLLERS_COUNT];
    struct interrupt_queue events;
    unsigned int delay_slot, reset_hard_job, next_interrupt, last_addr;
    uint32_t skip_jump;
    int cycle_count;
    unsigned audio_frequency;
    save_memory_data persistence;
};

struct memory_snapshot
{
    struct memory_snapshot *next;
    unsigned char *data;
    void *main_state, *rsp_state, *gpu;
    size_t main_capacity, rsp_capacity;
    enum rsp_plugin_type rsp_type;
    const struct rdp_snapshot_interface *renderer;
    struct snapshot_devices devices;
    uint32_t buttons[GAME_CONTROLLERS_COUNT];
    uint64_t epoch;
    bool captured;
};

static struct memory_snapshot *snapshots;
static struct memory_snapshot staging;
static uint64_t epoch = 1;
static bool failed;
static atomic_bool captures_active = ATOMIC_VAR_INIT(false);
static atomic_uint threaded_context = ATOMIC_VAR_INIT(RETRO_SNAPSHOT_ORDINARY);
static enum retro_snapshot_execution_context (RETRO_CALLCONV *execution_context)(void);

static const struct rdp_snapshot_interface glide_snapshot = {
    gln64_snapshot_supported, gln64_snapshot_create, gln64_snapshot_release,
    gln64_snapshot_valid, gln64_snapshot_flush, gln64_snapshot_save,
    gln64_snapshot_prepare, gln64_snapshot_commit, gln64_snapshot_clear,
    gln64_snapshot_failed
};

static const struct rdp_snapshot_interface *renderer(void)
{
    switch (current_rdp_type) {
    case RDP_PLUGIN_GLIDEN64: return &glide_snapshot;
#ifdef HAVE_THR_AL
    case RDP_PLUGIN_ANGRYLION: return &angrylion_snapshot;
#endif
#ifdef HAVE_PARALLEL_RDP
    case RDP_PLUGIN_PARALLEL: return &parallel_snapshot;
#endif
    default: return NULL;
    }
}

static void bind_renderer(bool bind)
{
    if (current_rdp_type == RDP_PLUGIN_GLIDEN64)
        glsm_ctl(bind ? GLSM_CTL_STATE_BIND : GLSM_CTL_STATE_UNBIND, NULL);
}

static bool owned(const struct memory_snapshot *state)
{
    const struct memory_snapshot *entry;
    for (entry = snapshots; entry; entry = entry->next)
        if (entry == state)
            return true;
    return false;
}

bool mupen_snapshot_failed(void)
{
    const struct rdp_snapshot_interface *rdp = renderer();
    return failed || (rdp && rdp->failed());
}

void mupen_snapshot_abort(void)
{
    failed = true;
    mupen_snapshot_invalidate();
}

bool mupen_snapshot_speculative(void)
{
    if (current_rdp_type == RDP_PLUGIN_GLIDEN64 && EnableThreadedRenderer)
        return atomic_load_explicit(&threaded_context, memory_order_acquire) ==
            RETRO_SNAPSHOT_SPECULATIVE;
    return execution_context && execution_context() == RETRO_SNAPSHOT_SPECULATIVE;
}

bool mupen_snapshot_active(void)
{
    return atomic_load_explicit(&captures_active, memory_order_acquire);
}

void mupen_snapshot_begin_frame(void)
{
    /* Only the frontend thread calls its execution-context callback. The
     * emulation thread reads this value while running the requested frame. */
    atomic_store_explicit(&threaded_context,
        execution_context ? execution_context() : RETRO_SNAPSHOT_ORDINARY,
        memory_order_release);
}

bool mupen_snapshot_flush_options(void)
{
    const struct rdp_snapshot_interface *rdp = renderer();
    enum retro_snapshot_result result;
    /* GLideN64's renderer policies are startup options. The low-level RDPs
     * also have live VI/synchronization settings: finish their old work first. */
    if (!rdp || current_rdp_type == RDP_PLUGIN_GLIDEN64 ||
        mupen_snapshot_initializing())
        return !mupen_snapshot_failed();
    result = rdp->flush();
    if (result == RETRO_SNAPSHOT_FATAL)
        failed = true;
    return result == RETRO_SNAPSHOT_SUCCESS;
}

static bool supported(uint32_t max_live_states)
{
    unsigned i;
    const struct rdp_snapshot_interface *rdp = renderer();
    if (!execution_context || !max_live_states || !rdp)
        return false;
    /* The frontend may allocate handles before the first retro_run initializes
     * devices and GLideN64. This query is advisory; save still requires the
     * completed boundary and actual device/renderer capabilities. */
    if (mupen_snapshot_initializing())
        return true;
    if (!rsp.snapshotSize || !rsp.snapshotSave || !rsp.snapshotLoad ||
        g_dev.dd.rom_size || !rdp->supported())
        return false;
    /* Other peripherals and Transfer Pak cartridges have separate mutable
     * state/storage backends which this adapter does not own. */
    for (i = 0; i < GAME_CONTROLLERS_COUNT; ++i)
        if (g_dev.transferpaks[i].gb_cart ||
            (Controls[i].Present && Controls[i].Type != CONT_TYPE_STANDARD))
            return false;
    return true;
}

static bool valid(const void *snapshot)
{
    const struct memory_snapshot *state = snapshot;
    return owned(state) && state->captured && state->epoch == epoch &&
        state->rsp_type == current_rsp_type &&
        state->renderer == renderer() && state->renderer->valid(state->gpu);
}

static bool reserve(struct memory_snapshot *state)
{
    size_t main_size = main_snapshot_size();
    size_t rsp_size = rsp.snapshotSize ? rsp.snapshotSize() : 0;
    if (!rsp_size)
        return false;
    if (!state->data)
        state->data = malloc(retro_serialize_size());
    if (state->main_capacity < main_size) {
        void *data = realloc(state->main_state, main_size);
        if (!data)
            return false;
        state->main_state = data;
        state->main_capacity = main_size;
    }
    if (state->rsp_capacity < rsp_size) {
        void *data = realloc(state->rsp_state, rsp_size);
        if (!data)
            return false;
        state->rsp_state = data;
        state->rsp_capacity = rsp_size;
    }
    return state->data && state->main_state && state->rsp_state;
}

static void discard(struct memory_snapshot *state)
{
    if (state->renderer)
        state->renderer->release(state->gpu);
    free(state->data);
    free(state->main_state);
    free(state->rsp_state);
    state->gpu = state->main_state = state->rsp_state = NULL;
    state->data = NULL;
    state->main_capacity = 0;
    state->rsp_capacity = 0;
    state->captured = false;
}

static enum retro_snapshot_result create(void **snapshot)
{
    struct memory_snapshot *state;
    if (!snapshot || *snapshot)
        return RETRO_SNAPSHOT_UNAVAILABLE;
    if (mupen_snapshot_failed())
        return RETRO_SNAPSHOT_FATAL;
    state = calloc(1, sizeof(*state));
    if (!state)
        return RETRO_SNAPSHOT_UNAVAILABLE;
    state->renderer = renderer();
    state->gpu = state->renderer ? state->renderer->create() : NULL;
    /* Handles can precede plugin initialization. Payload storage is reserved
     * by save, once the active CPU/RSP sizes and a completed VI are known. */
    if (!state->gpu) {
        discard(state);
        free(state);
        return RETRO_SNAPSHOT_UNAVAILABLE;
    }
    state->next = snapshots;
    snapshots = state;
    *snapshot = state;
    atomic_store_explicit(&captures_active, true, memory_order_release);
    return RETRO_SNAPSHOT_SUCCESS;
}

static void release(void *snapshot)
{
    struct memory_snapshot **link = &snapshots;
    struct memory_snapshot *state = snapshot;
    bool bind;
    while (*link && *link != state)
        link = &(*link)->next;
    if (!*link)
        return;
    *link = state->next;
    bind = state->renderer->supported();
    if (bind)
        bind_renderer(true);
    discard(state);
    if (!snapshots) {
        discard(&staging);
        state->renderer->clear();
    }
    if (bind)
        bind_renderer(false);
    free(state);
    atomic_store_explicit(&captures_active, snapshots != NULL, memory_order_release);
}

static void save_devices(struct snapshot_devices *state)
{
    state->dp = g_dev.dp;
    state->sp = g_dev.sp;
    state->ai = g_dev.ai;
    state->mi = g_dev.mi;
    state->pi = g_dev.pi;
    state->ri = g_dev.ri;
    state->si = g_dev.si;
    state->vi = g_dev.vi;
    state->pif = g_dev.pif;
    state->cart = g_dev.cart;
    state->is = g_dev.is;
    memcpy(state->controllers, g_dev.controllers, sizeof(state->controllers));
    memcpy(state->biopaks, g_dev.biopaks, sizeof(state->biopaks));
    memcpy(state->mempaks, g_dev.mempaks, sizeof(state->mempaks));
    memcpy(state->rumblepaks, g_dev.rumblepaks, sizeof(state->rumblepaks));
    memcpy(state->transferpaks, g_dev.transferpaks, sizeof(state->transferpaks));
    state->events = g_dev.r4300.cp0.q;
    state->delay_slot = g_dev.r4300.delay_slot;
    state->skip_jump = g_dev.r4300.skip_jump;
    state->reset_hard_job = g_dev.r4300.reset_hard_job;
    state->next_interrupt = *r4300_cp0_next_interrupt(&g_dev.r4300.cp0);
    state->last_addr = *r4300_cp0_last_addr(&g_dev.r4300.cp0);
    state->cycle_count = *r4300_cp0_cycle_count(&g_dev.r4300.cp0);
    state->persistence = saved_memory;
    state->audio_frequency = mupen_snapshot_audio_frequency();
}

static void load_devices(const struct snapshot_devices *state)
{
    unprotect_framebuffers(&g_dev.dp.fb);
    g_dev.dp = state->dp;
    g_dev.sp = state->sp;
    g_dev.ai = state->ai;
    g_dev.mi = state->mi;
    g_dev.pi = state->pi;
    g_dev.ri = state->ri;
    g_dev.si = state->si;
    g_dev.vi = state->vi;
    g_dev.pif = state->pif;
    g_dev.cart = state->cart;
    g_dev.is = state->is;
    memcpy(g_dev.controllers, state->controllers, sizeof(state->controllers));
    memcpy(g_dev.biopaks, state->biopaks, sizeof(state->biopaks));
    memcpy(g_dev.mempaks, state->mempaks, sizeof(state->mempaks));
    memcpy(g_dev.rumblepaks, state->rumblepaks, sizeof(state->rumblepaks));
    memcpy(g_dev.transferpaks, state->transferpaks, sizeof(state->transferpaks));
    g_dev.r4300.cp0.q = state->events;
    g_dev.r4300.delay_slot = state->delay_slot;
    g_dev.r4300.skip_jump = state->skip_jump;
    g_dev.r4300.reset_hard_job = state->reset_hard_job;
    *r4300_cp0_next_interrupt(&g_dev.r4300.cp0) = state->next_interrupt;
    *r4300_cp0_last_addr(&g_dev.r4300.cp0) = state->last_addr;
    *r4300_cp0_cycle_count(&g_dev.r4300.cp0) = state->cycle_count;
    saved_memory = state->persistence;
    mupen_snapshot_restore_audio_frequency(state->audio_frequency);
    restore_framebuffer_mappings(&g_dev.dp.fb);
}

static enum retro_snapshot_result save(void *snapshot)
{
    struct memory_snapshot *state = snapshot;
    struct snapshot_devices old_devices;
    unsigned char *old_data;
    void *old_main, *old_rsp;
    size_t old_capacity, old_rsp_capacity;
    uint32_t old_buttons[GAME_CONTROLLERS_COUNT];
    enum retro_snapshot_result result;
    if (mupen_snapshot_failed())
        return RETRO_SNAPSHOT_FATAL;
    if (!owned(state) || !supported(1) || !mupen_snapshot_ready())
        return RETRO_SNAPSHOT_UNAVAILABLE;
    if (!reserve(&staging))
        return RETRO_SNAPSHOT_UNAVAILABLE;
    bind_renderer(true);
    result = state->renderer->flush();
    if (result == RETRO_SNAPSHOT_SUCCESS) {
        save_devices(&staging.devices);
        main_snapshot_save(staging.main_state);
        rsp.snapshotSave(staging.rsp_state);
        egcvip_snapshot_save(staging.buttons);
        if (!savestates_save_snapshot(&g_dev, staging.data))
            result = RETRO_SNAPSHOT_UNAVAILABLE;
    }
    if (result == RETRO_SNAPSHOT_SUCCESS)
        result = state->renderer->save(state->gpu);
    bind_renderer(false);
    if (result != RETRO_SNAPSHOT_SUCCESS) {
        if (result == RETRO_SNAPSHOT_FATAL)
            failed = true;
        return result;
    }
    old_data = state->data; state->data = staging.data; staging.data = old_data;
    old_main = state->main_state; state->main_state = staging.main_state; staging.main_state = old_main;
    old_capacity = state->main_capacity; state->main_capacity = staging.main_capacity; staging.main_capacity = old_capacity;
    old_rsp = state->rsp_state; state->rsp_state = staging.rsp_state; staging.rsp_state = old_rsp;
    old_rsp_capacity = state->rsp_capacity; state->rsp_capacity = staging.rsp_capacity; staging.rsp_capacity = old_rsp_capacity;
    old_devices = state->devices; state->devices = staging.devices; staging.devices = old_devices;
    memcpy(old_buttons, state->buttons, sizeof(old_buttons));
    memcpy(state->buttons, staging.buttons, sizeof(old_buttons));
    memcpy(staging.buttons, old_buttons, sizeof(old_buttons));
    state->epoch = epoch;
    state->rsp_type = current_rsp_type;
    state->captured = true;
    return RETRO_SNAPSHOT_SUCCESS;
}

static enum retro_snapshot_result load(const void *snapshot)
{
    const struct memory_snapshot *state = snapshot;
    enum retro_snapshot_result result;
    const struct rdp_snapshot_interface *rdp = renderer();
    if (rdp && rdp->failed()) {
        failed = true;
        return RETRO_SNAPSHOT_FATAL;
    }
    if (!valid(state) || !supported(1) || !mupen_snapshot_ready() ||
        state->main_capacity < main_snapshot_size() ||
        state->rsp_capacity < rsp.snapshotSize())
        return RETRO_SNAPSHOT_UNAVAILABLE;
    if (!reserve(&staging))
        return RETRO_SNAPSHOT_UNAVAILABLE;
    bind_renderer(true);
    result = state->renderer->prepare(state->gpu);
    if (result == RETRO_SNAPSHOT_SUCCESS) {
        /* From this point live state changes. Any failure is fatal. */
        if (!savestates_load_snapshot(&g_dev, state->data, staging.data))
            result = RETRO_SNAPSHOT_FATAL;
        else {
            load_devices(&state->devices);
            main_snapshot_load(state->main_state);
            rsp.snapshotLoad(state->rsp_state);
            egcvip_snapshot_load(state->buttons);
            result = state->renderer->commit();
        }
    }
    bind_renderer(false);
    if (result == RETRO_SNAPSHOT_SUCCESS)
        failed = false;
    else if (result == RETRO_SNAPSHOT_FATAL)
        failed = true;
    return result;
}

void mupen_snapshot_register(retro_environment_t environment, const char *plugin_option)
{
    struct retro_variable plugin = { plugin_option, NULL };
    struct retro_snapshot_interface iface = {
        RETRO_SNAPSHOT_INTERFACE_VERSION, sizeof(iface),
        supported, create, save, load, release, valid, NULL
    };
    failed = false;
    execution_context = NULL;
    gln64_snapshot_new_instance();
    /* Unadapted renderers retain their existing frontend state path. The RDP
     * option requires restarting content, so its selection is stable here. */
    if (environment(RETRO_ENVIRONMENT_GET_VARIABLE, &plugin) && plugin.value &&
        strcmp(plugin.value, "gliden64") && strcmp(plugin.value, "angrylion") &&
        strcmp(plugin.value, "parallel"))
        return;
    execution_context = environment(RETRO_ENVIRONMENT_SET_SNAPSHOT_INTERFACE, &iface) ?
        iface.get_execution_context : NULL;
}

void mupen_snapshot_invalidate(void)
{
    struct memory_snapshot *state;
    ++epoch;
    for (state = snapshots; state; state = state->next) {
        state->captured = false;
    }
}

void mupen_snapshot_context_destroy(void)
{
    bool bind = snapshots && gln64_snapshot_supported();
    mupen_snapshot_invalidate();
    if (bind)
        glsm_ctl(GLSM_CTL_STATE_BIND, NULL);
    gln64_snapshot_context_destroy();
    if (bind)
        glsm_ctl(GLSM_CTL_STATE_UNBIND, NULL);
}

bool mupen_snapshot_recover_reset(void)
{
    const struct rdp_snapshot_interface *rdp = renderer();
    if (!rdp || rdp->failed() || !mupen_snapshot_ready())
        return false;
    mupen_snapshot_invalidate();
    if (failed) {
        reset_hard_handler(&g_dev);
        failed = false;
    }
    return true;
}

void mupen_snapshot_shutdown(void)
{
    const struct rdp_snapshot_interface *rdp = renderer();
    while (snapshots)
        release(snapshots);
    discard(&staging);
    if (rdp)
        rdp->clear();
    execution_context = NULL;
}
