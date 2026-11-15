# V4L2 M2M + DRM/KMS Extreme Low-Latency Roadmap & Checklist

This document tracks the phased implementation of build/packaging tooling, bitstream/decoder latency optimizations, zero-copy V4L2/DRM pipeline improvements, and real-time deterministic scheduling for Raspberry Pi 3B (`bcm2837` / `bcm2835-codec` + `vc4-kms-v3d` on Debian 13 Trixie `arm64`).

---

## Phase 1: Cross-Compilation, Cortex-A53 + LTO Tuning & `.deb` Packaging (Debian Trixie `arm64`)
*Goal: Build `arm64` `.deb` packages natively on x86_64 using Podman/Docker multiarch cross-compilation with Cortex-A53 + LTO optimizations in seconds and deploy to the Raspberry Pi with automatic RT capability setup.*

- [x] **Modernize CMake & Configure CPack `.deb` Packaging** (`CMakeLists.txt`, `.gitignore`)
  - Update `cmake_minimum_required(VERSION 3.16)` and `set(CMAKE_C_STANDARD 11)` (required for `<stdatomic.h>` and CMake 3.31 compatibility on Debian Trixie).
  - Default `CMAKE_INSTALL_PREFIX` to `/usr` and install `moonlight.conf` to `${CMAKE_INSTALL_FULL_SYSCONFDIR}` (`/etc/moonlight.conf`).
  - Configure `CPack` (`DEB` generator, `arm64` architecture detection, `CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON`, `CPACK_DEBIAN_PACKAGE_GENERATE_SHLIBS ON`, `conffiles`, and a `postinst` script running `ldconfig` and `setcap cap_sys_nice,cap_ipc_lock+ep /usr/bin/moonlight || true`).
- [x] **Create Cortex-A53 + LTO CMake Toolchain & Cross-Build Container** (`cmake/Toolchain-aarch64-linux-gnu.cmake`, `docker/Dockerfile.cross-trixie-arm64`)
  - Configure `aarch64-linux-gnu-gcc`, `aarch64-linux-gnu-gcc-ar`, and `aarch64-linux-gnu-gcc-ranlib` for LTO support.
  - Set `CMAKE_C_FLAGS_RELEASE` with `-O3 -DNDEBUG -mcpu=cortex-a53 -mtune=cortex-a53 -flto=auto -fomit-frame-pointer -fno-math-errno -fno-trapping-math`.
  - Build `debian:trixie-slim` multiarch cross-compilation image with `arm64` development headers (`libdrm-dev:arm64`, `libopus-dev:arm64`, `libevdev-dev:arm64`, `libudev-dev:arm64`, `libasound2-dev:arm64`, `libpulse-dev:arm64`, `libssl-dev:arm64`, `libcurl4-openssl-dev:arm64`, `libexpat1-dev:arm64`, `libavcodec-dev:arm64`, `libavutil-dev:arm64`, `libcec-dev:arm64`, `libsdl2-dev:arm64`).
- [x] **Create Automated Build & Deploy Scripts** (`scripts/build-deb.sh`, `scripts/deploy-pi.sh`)
  - Implement `scripts/build-deb.sh` (Podman/Docker `Release` build + `cpack -G DEB` verifying `LC_DEBUG` is disabled).
  - Implement `scripts/deploy-pi.sh` (`scp` + `ssh pi@10.42.0.92 sudo dpkg -i` + verification of installed package and capabilities).

---

## Phase 2: H.264 SPS VUI Fixup & V4L2 Hot-Path Zero-Intermediate-Copy
*Goal: Eliminate DPB frame reordering delays (up to 66–83 ms), cut V4L2 queue bufferbloat (~20 MB CMA saved), remove double CPU `memcpy` on every packet, and prevent `state_mutex` stalls.*

- [x] **Fix H.264 SPS VUI Low-Latency Patching** (`libgamestream/sps.c`)
  - Explicitly set `h264_stream->sps->vui_parameters_present_flag = 1`.
  - Force `num_reorder_frames = 0` and `max_dec_frame_buffering = 1` unconditionally even if the host encoder already sets `bitstream_restriction_flag = 1`.
  - Enable `low_delay_hrd_flag = 1` when HRD parameters (`nal_hrd_parameters_present_flag` or `vcl_hrd_parameters_present_flag`) are present.
  - Clamp `level_idc` by macroblock count (`<= 3600 -> 32`, `<= 8160 -> 42`) and unconditionally cap `level_idc <= 42` so Level 5.0 streams never fail on `bcm2835-codec`.
  - Increase `write_nal_unit()` buffer bound from `128` to `256` with error fallback and fix `h264_stream` re-init leak in `gs_sps_init()`.
- [x] **Set Single Slice Per Frame (`CAPABILITY_SLICES_PER_FRAME(1)`)** (`src/video/v4l2.c`)
  - Replace `CAPABILITY_SLICES_PER_FRAME(4)` with `CAPABILITY_SLICES_PER_FRAME(1)` to avoid redundant slice headers and CABAC context resets per frame on VideoCore IV.
- [x] **Eliminate Intermediate `bitstream_buf` Copy & Mutex Poll Contention** (`src/video/v4l2.h`, `src/video/v4l2.c`, `src/video/v4l2_codec.h`, `src/video/v4l2_codec.c`)
  - Acquire a free `OUTPUT_MPLANE` mmap buffer slot first, assemble `decodeUnit->bufferList` NALUs and `gs_sps_fix()` directly into `ctx->output_bufs[idx].start` with strict bounds checking, and enqueue via `VIDIOC_QBUF`.
  - Avoid holding `state_mutex` while polling `v4l2_fd` for `POLLOUT`, using `<stdatomic.h>` (`atomic_bool running`, `atomic_bool streaming`) and allowing a longer initial wait (up to 50 ms) during initial DRC (`!ctx->capture_streaming` or `FRAME_TYPE_IDR`).
  - Return `DR_NEED_IDR` if an `FRAME_TYPE_IDR` unit is dropped or on fatal `-EINVAL`, while returning `DR_OK` on transient P-frame `-EBUSY` drops.
- [x] **Reduce V4L2 Queue Depths & Fix `export_fd` Initialization** (`src/video/v4l2.h`, `src/video/v4l2_codec.c`)
  - Reduce `OUTPUT_MPLANE` buffers from `12` to `4` and `CAPTURE_MPLANE` buffers from `clamp(min_bufs + 8, 12, 20)` to `clamp(min_bufs + 3, 5, V4L2_MAX_CAPTURE_BUFFERS)`.
  - Initialize `v4l2_fd = -1`, `drm_fd = -1`, and all `export_fd = -1` prior to allocation to prevent closing `fd 0` (`stdin`) on partial error paths.

---

## Phase 3: DRM/KMS Presentation, Plane State Restoration & VBlank Optimization
*Goal: Eliminate stale frame presentation on VBlank boundaries, reduce per-flip atomic ioctl overhead, and cleanly restore the Linux console (`fbcon`) on exit.*

- [x] **Drain Decoded Capture Frames Before DRM VBlank Events** (`src/video/v4l2.c`)
  - Process `CAPTURE_MPLANE` (`VIDIOC_DQBUF`) before `drmHandleEvent()` in `v4l2_event_thread` so newly decoded frames are staged in time for a simultaneous VBlank wakeup.
  - Skip empty/error buffers (`bytesused == 0`, `V4L2_BUF_FLAG_ERROR`, `V4L2_BUF_FLAG_LAST`) and when multiple decoded frames dequeue in one burst, immediately requeue intermediate frames and present only the freshest frame.
- [x] **Primary Plane Selection, Console Restoration & Fast Atomic Commits** (`src/video/v4l2.h`, `src/video/v4l2_drm.h`, `src/video/v4l2_drm.c`, `src/video/v4l2_codec.c`, `src/platform.c`, `src/main.c`)
  - Prefer `DRM_PLANE_TYPE_PRIMARY` supporting `DRM_FORMAT_NV12`, save initial plane/CRTC properties (`FB_ID`, `CRTC_ID`, geometry) in `v4l2_drm_init()`, and restore them on exit via `v4l2_drm_restore_plane_sync()` so `fbcon` recovers cleanly.
  - Only call `v4l2_drm_disable_plane_sync()` during `v4l2_codec_handle_drc()` when `ctx->capture_streaming` is already active (saving 16.67 ms on initial stream startup).
  - Pre-allocate `drmModeAtomicReqPtr` using `drmModeAtomicSetCursor(req, 0)` and track `plane_needs_full_props` so steady-state page flips only update `FB_ID`.
  - Support optional `COLOR_ENCODING` (`BT.709` / `BT.601`) and `COLOR_RANGE` (`Limited` / `Full`) plane properties, unblank `/sys/class/graphics/fb0/blank` on start/stop, and list `v4l2` in `-platform` CLI help.

---

## Phase 4: Real-Time Determinism (`mlockall` + Thread Priorities) & Test Suite
*Goal: Prevent page-fault jitter on 1 GB RAM and verify all backend state machines with automated tests.*

- [ ] **Lock Process Memory & Elevate Hot-Path Threads** (`src/video/v4l2.c`)
  - Call `mlockall(MCL_CURRENT | MCL_FUTURE | MCL_ONFAULT)` during `v4l2_setup()` to prevent page faults without pre-faulting full 8 MB thread stacks on the 1 GB RPi 3B.
- [ ] **Integrate & Expand Unit Test Suite** (`CMakeLists.txt`, `test/test_v4l2_backend.c`)
  - Wire `test_v4l2_backend` into `CMakeLists.txt` (`enable_testing()`) and add regression tests for `gs_sps_fix()` VUI patching, `export_fd` safety, and multi-frame capture burst draining.
