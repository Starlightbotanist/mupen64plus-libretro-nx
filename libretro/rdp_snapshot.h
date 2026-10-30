#ifndef MUPEN_RDP_SNAPSHOT_H
#define MUPEN_RDP_SNAPSHOT_H

#include <libretro.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Process-local renderer state. prepare reserves everything commit needs;
 * commit runs only after the shared CPU/device state has been restored. */
struct rdp_snapshot_interface
{
    bool (*supported)(void);
    void *(*create)(void);
    void (*release)(void *state);
    bool (*valid)(const void *state);
    enum retro_snapshot_result (*flush)(void);
    enum retro_snapshot_result (*save)(void *state);
    enum retro_snapshot_result (*prepare)(const void *state);
    enum retro_snapshot_result (*commit)(void);
    void (*clear)(void);
    bool (*failed)(void);
};

#ifdef HAVE_THR_AL
extern const struct rdp_snapshot_interface angrylion_snapshot;
const void *angrylion_snapshot_frame(void);
#endif
#ifdef HAVE_PARALLEL_RDP
extern const struct rdp_snapshot_interface parallel_snapshot;
#endif

#ifdef __cplusplus
}
#endif
#endif
