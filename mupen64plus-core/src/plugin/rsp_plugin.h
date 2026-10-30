#ifndef M64P_RSP_PLUGIN_H
#define M64P_RSP_PLUGIN_H

#include <stddef.h>
#include "api/m64p_plugin.h"

/* Shared by the core and libretro's plugin compatibility header. */
typedef struct _rsp_plugin_functions
{
    ptr_PluginGetVersion getVersion;
    ptr_DoRspCycles doRspCycles;
    ptr_InitiateRSP initiateRSP;
    ptr_RomClosed romClosed;
    size_t (*snapshotSize)(void);
    void (*snapshotSave)(void *data);
    void (*snapshotLoad)(const void *data);
} rsp_plugin_functions;

extern rsp_plugin_functions rsp;

#endif
