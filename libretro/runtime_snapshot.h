#ifndef MUPEN_LIBRETRO_SNAPSHOT_H
#define MUPEN_LIBRETRO_SNAPSHOT_H

#include <libretro.h>

#ifdef __cplusplus
extern "C" {
#endif

void mupen_snapshot_register(retro_environment_t environment, const char *plugin_option);
void mupen_snapshot_invalidate(void);
void mupen_snapshot_shutdown(void);
void mupen_snapshot_context_destroy(void);
bool mupen_snapshot_failed(void);
void mupen_snapshot_abort(void);
bool mupen_snapshot_speculative(void);
bool mupen_snapshot_active(void);
void mupen_snapshot_begin_frame(void);
bool mupen_snapshot_flush_options(void);
void mupen_snapshot_set_boundary(bool ready);
bool mupen_snapshot_ready(void);
bool mupen_snapshot_initializing(void);
bool mupen_snapshot_recover_reset(void);
unsigned mupen_snapshot_audio_frequency(void);
void mupen_snapshot_restore_audio_frequency(unsigned frequency);

#ifdef __cplusplus
}
#endif

#endif
