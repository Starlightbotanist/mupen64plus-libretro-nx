/******************************************************************************\
* Project:  Module Subsystem Interface to SP Interpreter Core                  *
* Authors:  Iconoclast                                                         *
* Release:  2016.03.23                                                         *
* License:  CC0 Public Domain Dedication                                       *
*                                                                              *
* To the extent possible under law, the author(s) have dedicated all copyright *
* and related and neighboring rights to this software to the public domain     *
* worldwide. This software is distributed without any warranty.                *
*                                                                              *
* You should have received a copy of the CC0 Public Domain Dedication along    *
* with this software.                                                          *
* If not, see <http://creativecommons.org/publicdomain/zero/1.0/>.             *
\******************************************************************************/

#define LIBRETRO

#if defined(USE_SSE2NEON) && defined(__ARM_NEON__)
#include "sse2neon/SSE2NEON.h"
#define ARCH_MIN_SSE2
#endif

#include "vu/add.c"
#include "vu/divide.c"
#include "vu/logical.c"
#include "vu/multiply.c"
#include "vu/select.c"
#include "vu/vu.c"
#include "su.c"
#include "module.c"

unsigned char rsp_conf[32];

struct cxd4_snapshot
{
    u32 scalar[32];
    i16 vector[32][N << VR_STATIC_WRAPAROUND];
    i16 accumulator[3][N];
    i16 carry[N], not_equal[N], clip[N], compare[N], extension[N];
    s32 divide_in, divide_out;
    int divide_high, branch_target, status_timeout;
#ifndef EMULATE_STATIC_PC
    int branch_stage;
#endif
#ifdef WAIT_FOR_CPU_HOST
    short status_reads[32];
#endif
};

size_t cxd4_snapshot_size(void)
{
    return sizeof(struct cxd4_snapshot);
}

void cxd4_snapshot_save(void *data)
{
    struct cxd4_snapshot state = {0};
    memcpy(state.scalar, SR, sizeof(SR));
    memcpy(state.vector, VR, sizeof(VR));
    memcpy(state.accumulator, VACC, sizeof(VACC));
    memcpy(state.carry, cf_co, sizeof(cf_co));
    memcpy(state.not_equal, cf_ne, sizeof(cf_ne));
    memcpy(state.clip, cf_clip, sizeof(cf_clip));
    memcpy(state.compare, cf_comp, sizeof(cf_comp));
    memcpy(state.extension, cf_vce, sizeof(cf_vce));
    state.divide_in = DivIn;
    state.divide_out = DivOut;
    state.divide_high = DPH;
#ifndef EMULATE_STATIC_PC
    state.branch_stage = stage;
#endif
    state.branch_target = temp_PC;
    state.status_timeout = MF_SP_STATUS_TIMEOUT;
#ifdef WAIT_FOR_CPU_HOST
    memcpy(state.status_reads, MFC0_count, sizeof(MFC0_count));
#endif
    memcpy(data, &state, sizeof(state));
}

void cxd4_snapshot_load(const void *data)
{
    struct cxd4_snapshot state;
    memcpy(&state, data, sizeof(state));
    memcpy(SR, state.scalar, sizeof(SR));
    memcpy(VR, state.vector, sizeof(VR));
    memcpy(VACC, state.accumulator, sizeof(VACC));
    memcpy(cf_co, state.carry, sizeof(cf_co));
    memcpy(cf_ne, state.not_equal, sizeof(cf_ne));
    memcpy(cf_clip, state.clip, sizeof(cf_clip));
    memcpy(cf_comp, state.compare, sizeof(cf_comp));
    memcpy(cf_vce, state.extension, sizeof(cf_vce));
    DivIn = state.divide_in;
    DivOut = state.divide_out;
    DPH = state.divide_high;
#ifndef EMULATE_STATIC_PC
    stage = state.branch_stage;
#endif
    temp_PC = state.branch_target;
    MF_SP_STATUS_TIMEOUT = state.status_timeout;
#ifdef WAIT_FOR_CPU_HOST
    memcpy(MFC0_count, state.status_reads, sizeof(MFC0_count));
#endif
}
