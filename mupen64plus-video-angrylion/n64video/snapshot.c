#include "../../libretro/rdp_snapshot.h"

extern uint32_t retro_screen_width, retro_screen_height, screen_pitch;

/* Lookup tables and worker objects are process resources. Only the active
 * workers' RDP state and the VI's temporal state belong to a timeline. Internal
 * RDP pointers still refer to the same stable state[] objects after restore. */
struct al_state
{
    struct rdp_state *workers;
    uint32_t worker_count, worker_capacity;
    uint64_t generation;
    int pipeline_crashed;
    uint8_t hidden[sizeof(rdram_hidden)];
    uint32_t partial[CMD_MAX_INTS], cmd_pos, cmd_id, cmd_len;
    uint32_t prevvicurrent;
    int32_t emucontrolsvicurrent, oldvstart;
    bool prevserrate, lowerfield, prevwasblank;
    uint32_t tvfadeoutstate[PRESCALE_HEIGHT];
    uint32_t rseed[sizeof(rseed) / sizeof(rseed[0])], zb_address;
    struct rgba prescale[PRESCALE_WIDTH * PRESCALE_HEIGHT];
    uint32_t width, height, pitch;
};

struct al_snapshot { struct al_state *state; };
static struct al_state *snapshot_staging;
static const struct al_state *snapshot_restore;

static bool al_supported(void) { return snapshot_ready; }
static bool al_failed(void) { return false; }

static void *al_create(void)
{
    /* A handle can precede the first VI; reserve presentation before it runs. */
    if (!snapshot_presented)
        snapshot_presented = malloc(sizeof(prescale));
    if (!snapshot_presented)
        return NULL;
    return calloc(1, sizeof(struct al_snapshot));
}

static void al_release(void *opaque)
{
    struct al_snapshot *snapshot = opaque;
    if (snapshot) {
        if (snapshot_restore == snapshot->state)
            snapshot_restore = NULL;
        if (snapshot->state)
            free(snapshot->state->workers);
        free(snapshot->state);
        free(snapshot);
    }
}

static void al_clear(void)
{
    if (snapshot_staging)
        free(snapshot_staging->workers);
    free(snapshot_staging);
    snapshot_staging = NULL;
    snapshot_restore = NULL;
}

static bool al_valid(const void *opaque)
{
    const struct al_snapshot *snapshot = opaque;
    return snapshot_ready && snapshot && snapshot->state &&
        snapshot->state->generation == snapshot_generation &&
        snapshot->state->worker_count == (config.parallel ? parallel_num_workers() : 1);
}

static enum retro_snapshot_result al_flush(void)
{
    if (!snapshot_ready)
        return RETRO_SNAPSHOT_UNAVAILABLE;
    /* parallel_run joins all participating workers before returning. Preserve
     * any incomplete command; only complete buffered commands may execute. */
    cmd_flush();
    return RETRO_SNAPSHOT_SUCCESS;
}

static enum retro_snapshot_result al_save(void *opaque)
{
    struct al_snapshot *snapshot = opaque;
    struct al_state *swap;
    uint32_t count = config.parallel ? parallel_num_workers() : 1;
    if (!snapshot_staging)
        snapshot_staging = calloc(1, sizeof(*snapshot_staging));
    if (!snapshot_staging)
        return RETRO_SNAPSHOT_UNAVAILABLE;
    if (snapshot_staging->worker_capacity < count) {
        void *workers = realloc(snapshot_staging->workers, count * sizeof(struct rdp_state));
        if (!workers)
            return RETRO_SNAPSHOT_UNAVAILABLE;
        snapshot_staging->workers = workers;
        snapshot_staging->worker_capacity = count;
    }
    memcpy(snapshot_staging->workers, state, count * sizeof(struct rdp_state));
    snapshot_staging->worker_count = count;
    snapshot_staging->generation = snapshot_generation;
    snapshot_staging->pipeline_crashed = rdp_pipeline_crashed;
    memcpy(snapshot_staging->hidden, rdram_hidden, sizeof(rdram_hidden));
    memcpy(snapshot_staging->partial, rdp_cmd_buf[0], sizeof(snapshot_staging->partial));
    snapshot_staging->cmd_pos = rdp_cmd_pos;
    snapshot_staging->cmd_id = rdp_cmd_id;
    snapshot_staging->cmd_len = rdp_cmd_len;
    snapshot_staging->prevvicurrent = prevvicurrent;
    snapshot_staging->emucontrolsvicurrent = emucontrolsvicurrent;
    snapshot_staging->oldvstart = oldvstart;
    snapshot_staging->prevserrate = prevserrate;
    snapshot_staging->lowerfield = lowerfield;
    snapshot_staging->prevwasblank = prevwasblank;
    memcpy(snapshot_staging->tvfadeoutstate, tvfadeoutstate, sizeof(tvfadeoutstate));
    memcpy(snapshot_staging->rseed, rseed, sizeof(rseed));
    snapshot_staging->zb_address = zb_address;
    memcpy(snapshot_staging->prescale, prescale, sizeof(prescale));
    snapshot_staging->width = retro_screen_width;
    snapshot_staging->height = retro_screen_height;
    snapshot_staging->pitch = screen_pitch;
    /* All allocations precede publication; failure leaves the previous capture. */
    swap = snapshot->state;
    snapshot->state = snapshot_staging;
    snapshot_staging = swap;
    return RETRO_SNAPSHOT_SUCCESS;
}

static enum retro_snapshot_result al_prepare(const void *opaque)
{
    snapshot_restore = NULL;
    if (!al_valid(opaque))
        return RETRO_SNAPSHOT_UNAVAILABLE;
    if (al_flush() != RETRO_SNAPSHOT_SUCCESS)
        return RETRO_SNAPSHOT_UNAVAILABLE;
    snapshot_restore = ((const struct al_snapshot *)opaque)->state;
    return RETRO_SNAPSHOT_SUCCESS;
}

static enum retro_snapshot_result al_commit(void)
{
    const struct al_state *snapshot = snapshot_restore;
    if (!snapshot)
        return RETRO_SNAPSHOT_FATAL;
    memcpy(state, snapshot->workers, snapshot->worker_count * sizeof(struct rdp_state));
    rdp_pipeline_crashed = snapshot->pipeline_crashed;
    memcpy(rdram_hidden, snapshot->hidden, sizeof(rdram_hidden));
    memcpy(rdp_cmd_buf[0], snapshot->partial, sizeof(snapshot->partial));
    rdp_cmd_buf_pos = 0;
    rdp_cmd_pos = snapshot->cmd_pos;
    rdp_cmd_id = snapshot->cmd_id;
    rdp_cmd_len = snapshot->cmd_len;
    prevvicurrent = snapshot->prevvicurrent;
    emucontrolsvicurrent = snapshot->emucontrolsvicurrent;
    oldvstart = snapshot->oldvstart;
    prevserrate = snapshot->prevserrate;
    lowerfield = snapshot->lowerfield;
    prevwasblank = snapshot->prevwasblank;
    memcpy(tvfadeoutstate, snapshot->tvfadeoutstate, sizeof(tvfadeoutstate));
    memcpy(rseed, snapshot->rseed, sizeof(rseed));
    zb_address = snapshot->zb_address;
    memcpy(prescale, snapshot->prescale, sizeof(prescale));
    retro_screen_width = snapshot->width;
    retro_screen_height = snapshot->height;
    screen_pitch = snapshot->pitch;
    snapshot_restore = NULL;
    return RETRO_SNAPSHOT_SUCCESS;
}

const struct rdp_snapshot_interface angrylion_snapshot = {
    al_supported, al_create, al_release, al_valid, al_flush, al_save,
    al_prepare, al_commit, al_clear, al_failed
};

const void *angrylion_snapshot_frame(void)
{
    if (!snapshot_presented || !screen_pitch ||
        retro_screen_height > sizeof(prescale) / screen_pitch ||
        retro_screen_width > screen_pitch / sizeof(struct rgba))
        return NULL;
    /* The frontend may redraw this image after restoration. VI history in
     * prescale rewinds independently; the presented pixels stay live. */
    memcpy(snapshot_presented, prescale, (size_t)retro_screen_height * screen_pitch);
    return snapshot_presented;
}
