#ifndef GLIDEN64_SNAPSHOT_H
#define GLIDEN64_SNAPSHOT_H

#include <libretro.h>

#ifdef __cplusplus
extern "C" {
#endif

bool gln64_snapshot_supported(void);
bool gln64_snapshot_valid(const void *snapshot);
void gln64_snapshot_context_destroy(void);
void gln64_snapshot_context_reset(void);
void gln64_snapshot_new_instance(void);
void *gln64_snapshot_create(void);
void gln64_snapshot_release(void *snapshot);
void gln64_snapshot_clear(void);
enum retro_snapshot_result gln64_snapshot_save(void *snapshot);
enum retro_snapshot_result gln64_snapshot_flush(void);
enum retro_snapshot_result gln64_snapshot_prepare(const void *snapshot);
enum retro_snapshot_result gln64_snapshot_commit(void);
bool gln64_snapshot_failed(void);

#ifdef __cplusplus
}
#endif
#endif
