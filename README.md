# moonlight-embedded-rpi3-v4l2

> Modern hardware-accelerated V4L2 M2M + DRM/KMS backend for Moonlight Embedded on Raspberry Pi (specifically optimized for Raspberry Pi 3B / VideoCore IV on modern Raspberry Pi OS Bullseye & Bookworm).

---

## Overview

Modern Raspberry Pi OS releases (Bullseye, Bookworm, and newer) have deprecated the proprietary MMAL and OpenMAX multimedia APIs in favor of standard Linux kernel APIs: **V4L2 M2M** (`bcm2835-codec`) for video decoding and **DRM/KMS** (`vc4-kms-v3d`) for display presentation.

This fork implements a native, zero-copy, low-latency **V4L2 M2M + Atomic DRM/KMS** backend (`-platform v4l2`) for `moonlight-embedded`. It brings back full hardware-accelerated GameStream/Sunshine streaming to the Raspberry Pi 3B without relying on deprecated legacy firmware drivers, userland libraries, or heavy desktop window managers.

### Key Features

* **Hardware Video Decoding**: Fully utilizes the BCM2837 VideoCore IV hardware H.264 decoder via standard Linux V4L2 Memory-to-Memory (`/dev/video10` - `bcm2835-codec`).
* **Zero-Copy DRM/KMS Atomic Presentation**: Directly imports decoded NV12 frames via DMABUF (`VIDIOC_EXPBUF` -> DRM GEM handles) and presents them to the HDMI display using Atomic KMS (`/dev/dri/card0` / `card1`), completely bypassing X11/Wayland for minimal overhead and zero-copy performance.
* **Low-Latency DPB & Bitstream Optimization**: Applies SPS bitstream patch (`max_dec_frame_buffering = 1`, `num_reorder_frames = 0`) to prevent hardware pipeline frame reordering delays.
* **Single-Slot Staging & Frame Dropping**: Prevents display latency buildup by dropping stale queued frames in favor of the freshest available frame upon VBlank.
* **Automatic Macroblock Viewport Cropping**: Correctly handles 1088-pixel coded macroblock alignment, cleanly cropping to visible 1080p without visual artifacting or distortion.
* **Architecture Agnostic**: Tested and compatible with both 32-bit (`armhf`) and 64-bit (`aarch64`) modern Raspberry Pi OS installations.

---

## Building & Installation

### 1. Prerequisites (Raspberry Pi OS)

Ensure you have the required development headers installed on your Raspberry Pi:

```bash
sudo apt update
sudo apt install -y cmake build-essential libdrm-dev libv4l-dev libevdev-dev \
                    libudev-dev libavcodec-dev libavutil-dev libasound2-dev \
                    libopus-dev libssl-dev libcurl4-openssl-dev
```

### 2. Build

```bash
git clone https://github.com/<your-repo>/moonlight-embedded.git
cd moonlight-embedded
mkdir build && cd build
cmake ..
make -j4
sudo make install
```

---

## Usage

Pair with your Sunshine / GameStream host:

```bash
moonlight pair <HOST_IP>
```

Start streaming using the **`v4l2`** platform:

* **Recommended for 60 FPS (Fast-paced gaming & lowest input lag):**
  ```bash
  moonlight stream <HOST_IP> -app "Desktop" -platform v4l2 -720 -60fps -bitrate 10000 -mapping /path/to/gamecontrollerdb.txt
  ```

* **Recommended for 1080p (Productivity / Desktop):**
  ```bash
  moonlight stream <HOST_IP> -app "Desktop" -platform v4l2 -1080 -30fps -bitrate 10000 -mapping /path/to/gamecontrollerdb.txt
  ```

---

## Original Moonlight Embedded Documentation

[![Build](https://img.shields.io/github/actions/workflow/status/moonlight-stream/moonlight-embedded/build.yml?branch=master)](https://github.com/moonlight-stream/moonlight-embedded/actions/workflows/build.yml?query=branch%3Amaster) [Nightly Build Downloads](https://nightly.link/moonlight-stream/moonlight-embedded/workflows/build/master)

Moonlight Embedded is an open source client for [Sunshine](https://github.com/LizardByte/Sunshine) and NVIDIA GameStream for embedded Linux systems, like Raspberry Pi, CuBox-i and ODROID. Moonlight allows you to stream your full collection of games and applications from your PC to other devices to play them remotely.

Moonlight also has [PC](https://github.com/moonlight-stream/moonlight-qt), [Android](https://github.com/moonlight-stream/moonlight-android), and [iOS](https://github.com/moonlight-stream/moonlight-ios) clients.

### Documentation

More information about installing and running Moonlight Embedded is available on the [wiki](https://github.com/moonlight-stream/moonlight-embedded/wiki).

### Bugs

Please check the wiki and old bug reports before submitting a new bug report.

Bugs can be reported to the [issue tracker](https://github.com/moonlight-stream/moonlight-embedded/issues).

### See also

[Moonlight-common-c](https://github.com/moonlight-stream/moonlight-common-c) is the shared codebase between different Moonlight implementations

### Contribute

1. Fork us
2. Write code
3. Send Pull Requests
