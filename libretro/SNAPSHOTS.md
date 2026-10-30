# Runtime snapshots

The optional version 2 interface captures complete state for run-ahead and
preemptive frames. `snapshot.c` shares core state; `rdp_snapshot.h` connects each
renderer's private state. No N64-specific frontend code is needed. Registration
does not enable prediction or allocate state images.

GLideN64, Angrylion and ParaLLEl-RDP support standard controllers and their
existing HLE/CXD4/ParaLLEl-RSP selections. Angrylion is native-resolution in this
core; its retained RDP/RSP state still needs correct restoration. Core GL
threading and frontend video threading are independent.

## Retained state

- Shared: CPU/memory codec, devices, interrupts, persistence, controllers,
  cheats and RSP state. Capture/restore executes no guest instructions.
- GLideN64: enhanced color/depth surfaces, framebuffer ownership/attachments,
  decoder and readback history. Derived caches are invalidated; required
  transfers finish before their storage is read or reused.
- Angrylion: worker RDP state, hidden coverage, partial commands and VI history.
- ParaLLEl-RDP: native/hidden/TMEM and enhanced buffers, partial commands and
  immutable scanout history. Host frame rings, timelines and fences stay live.

Saving stages components before replacing a capture. Loading validates and
reserves before mutating CPU/GPU state. Failed submission or failure after
mutation is fatal: execution, persistence and memory reads stop until confirmed
recovery. A lost context cannot restore its old GPU resources.

Reset, ordinary state loading, option/controller/cheat changes and context
teardown invalidate captures. Invalid snapshots remain releasable; rejected
ordinary loads retain the recovery anchor. Manual state formats stay unchanged.

64DD, loaded Transfer Pak cartridges and unsupported controller/resource types
are declined. GL copying needs desktop GL 4.3/ARB_copy_image or GLES 3.2 with
required entry points; EGLImage readback is unsupported. Incomplete temporary
serialization is rejected instead of substituting native pixels.

Snapshot images and GPU copies exist only while prediction owns handles. The
last presented software image stays allocated until renderer shutdown so a
paused frontend can still read it; copying stops when prediction stops. Inactive
GLideN64 retains allocation metadata, not snapshot images. Matching allocations
are reused; active cost grows with resolution, MSAA and retained state count.
ParaLLEl-RDP enhanced storage grows with the square of the upscale factor.
Support does not guarantee real-time speed at every resolution/depth.

`tests/snapshots` supplies an original native Angrylion/RSP regression fixture;
it does not certify every hardware path or private state field.
