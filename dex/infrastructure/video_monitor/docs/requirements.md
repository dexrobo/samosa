# Requirements — Video Monitor Binary

## Functional Requirements

### FR-1: Shared Memory Topic Monitoring

The binary reads one or more shared memory camera topics using the existing
`Monitor<CameraFrameBuffer>` class. Reads are passive and non-blocking — the
monitor never interferes with the producer-consumer protocol.

### FR-2: H.264 Video Encoding

Raw RGB camera frames from shared memory are encoded to H.264 using OpenH264
(BSD-2-Clause). Each topic's frames are encoded independently on a dedicated
pipeline thread.

### FR-3: fMP4 HTTP Streaming

Encoded video is served as fragmented MP4 (ISO BMFF) over HTTP with chunked
transfer encoding. Each topic maps to an HTTP endpoint:

* `GET /stream/{topic_name}` — fMP4 video stream
* `GET /topics` — JSON list of available streams

### FR-4: Multiple Simultaneous Clients

Each topic's stream is encoded once and fanned out to N connected clients.
Adding or removing clients does not affect encoding or other clients.

### FR-5: Multiple Topics

The binary supports monitoring multiple shared memory topics concurrently.
Each topic runs its own independent encoding pipeline.

### FR-6: Time Synchronization

Video streams from different topics share a common monotonic time base,
enabling synchronized playback on the client side. Jitter between streams
is acceptable.

### FR-7: Topic Discovery

A JSON endpoint (`GET /topics`) lists all configured streams with their
names and paths, enabling dynamic client/dashboard discovery.

## Non-Functional Requirements

### NFR-1: Standalone Static Binary

The binary has zero runtime dependencies. It is statically linked and
redistributable as a single file.

### NFR-2: Cross-Platform Targets

Two target platforms:

* `linux/amd64` (x86_64)
* `linux/arm64` (aarch64)

### NFR-3: Production Co-Residency

The binary is designed to run alongside production workloads. It must
minimize CPU, memory, and I/O overhead:

* One CPU core per 1080p30 topic (OpenH264 Baseline, software encoding)
* ~20MB memory per topic (frame buffer + YUV buffer + encoder state)
* No GPU usage (pure software encoding)

### NFR-4: Browser-Native Playback

Streams are playable directly in modern browsers (Chrome, Firefox, Safari,
Edge) via `<video>` tags or the Media Source Extensions (MSE) API. No
plugins, no signaling, no additional infrastructure required.

## Constraints

### C-1: Permissive Licenses Only

All third-party dependencies must use permissive licenses: MIT, BSD,
Apache 2.0, or equivalent. GPL and LGPL are prohibited.

### C-2: Existing Toolchain

Must build with the existing project toolchain: C++20, GCC 12, LLVM 21,
Bazel. No additional build systems or compilers.

### C-3: No Additional Runtime Dependencies

Everything is statically linked into the binary. No shared libraries,
no container runtime, no language runtimes.

## Future Scope (Not This PR)

* **RTSP server**: Reuse encoded NAL units with an RTP muxer for VLC/GStreamer clients.
* **Browser dashboard**: Static HTML served from the binary with multi-panel video grid.
* **x265/HEVC**: Optional codec for lower bandwidth at higher CPU cost.
* **Depth stream encoding**: Encode depth frames (currently out of scope).
* **Audio**: No audio sources in current shared memory infrastructure.
