# Architecture — Video Monitor Binary

## System Overview

```text
SharedMem Topic A ──> Pipeline Thread A ──> FragmentRing A ──┐
SharedMem Topic B ──> Pipeline Thread B ──> FragmentRing B ──┤──> HTTP Server Thread
SharedMem Topic C ──> Pipeline Thread C ──> FragmentRing C ──┘     GET /          (tiled dashboard)
                                                                    GET /view/{topic} (single MSE player)
                                                                    GET /stream/{topic} (raw fMP4)
                                                                    GET /topics    (JSON list)
                                                                    GET /status    (per-topic stats)
```

The binary runs three types of threads:

1. **Main thread** — loads config, creates pipelines and HTTP server, waits for shutdown
2. **Pipeline threads** (one per topic) — read shared memory, encode, mux, push to ring
3. **HTTP server thread** (+handler pool) — serves video streams to clients

## Pipeline Architecture

Each topic runs an independent pipeline on a dedicated thread:

```text
FragmentRing::WaitForClient()          // CV blocks pipeline when 0 clients (lazy encoding, 0% idle CPU)
  -> Monitor::Run(callback)            // != sequence check wakes on new frame, InitializeBuffer wakes futex
  -> downsample(bilinear)              // per-topic max_width/max_height (default 1280x720)
  -> color_convert(RGB24 -> I420)      // BT.601 matrix, ARM64 NEON asm / x86_64 NASM asm
  -> OpenH264::Encode(I420) -> NALs   // ~10-15ms for 720p Baseline
  -> FMP4Muxer::Mux(NALs) -> fragment // init segment once, then moof+mdat per frame
  -> FragmentRing::Push(fragment)      // publish for HTTP fan-out
```

### Data Sizes (per topic, at default 1280x720 cap)

| Buffer | Size | Lifetime |
|---|---|---|
| CameraFrameBuffer (read copy) | ~16MB | Thread-local, reused |
| Downsampled RGB buffer | ~2.8MB (1280x720x3) | Thread-local, reused |
| I420 YUV buffer | ~1.4MB (1280x720x1.5) | Thread-local, reused |
| Encoded NALs | ~5-30KB per frame | Transient |
| fMP4 fragment | ~5-30KB per frame | Shared via FragmentRing |

## Thread Ownership Model

**Model: Thread confinement with message passing via FragmentRing.**

| Thread | Count | Owns | Communicates via |
|---|---|---|---|
| Main | 1 | Config, pipeline lifecycle, shutdown | StreamingControl singleton |
| HTTP server | 1 (+handler pool) | httplib::Server, endpoint routing | FragmentRing (reader) |
| Pipeline | 1 per topic | Monitor, CameraFrameBuffer, downsample buf, YUV buf, Encoder, Muxer | FragmentRing (sole writer) |

### Concurrency Rules

* **Thread confinement**: each pipeline thread owns all its mutable resources. No shared
  mutable state between pipelines.
* **Single writer per ring**: the pipeline thread is the sole writer to its FragmentRing.
  HTTP handler threads are readers only.
* **No shared variables between writers**: per AGENTS.md H.1, no flags or variables are
  shared between multiple writer threads.

### Synchronization

* **Shared memory read path**: uses the existing lock-free atomic protocol (futex +
  sequence validation). `InitializeBuffer` wakes futex; `Monitor::Run` uses `!=` sequence
  check for frame change detection.
* **FragmentRing fan-out**: uses `std::mutex` + `std::condition_variable`. This is on the
  HTTP serving path, not the shared memory hot path. Acceptable latency.
* **Lazy encoding gate**: `FragmentRing::WaitForClient()` blocks on `client_cv_` when
  `client_count_ == 0`. Pipeline thread sleeps with 0% CPU until an HTTP client connects.

## Broadcast Pattern (FragmentRing)

The FragmentRing is the fan-out mechanism between the pipeline (sole writer) and HTTP
handlers (multiple readers):

```text
FragmentRing {
  ring_: vector<Fragment>       // Fixed-size circular buffer
  head_sequence_: uint64        // Monotonic write cursor
  latest_idr_sequence_: uint64  // For late-joiner catch-up
  init_segment_: vector<uint8>  // ftyp+moov, sent to each new client
  client_count_: atomic<int>    // Active client tracking
  mutex_ + cv_                  // Reader synchronization
  client_cv_                    // Wakes pipeline when clients connect
}

Fragment {
  data: shared_ptr<const vector<uint8>>  // fMP4 bytes (moof + mdat)
  sequence: uint64                        // Monotonic fragment sequence
  timestamp_us: uint64                    // PTS from shared time base
  contains_idr: bool                      // Starts with a keyframe?
}
```

* Pipeline blocks in `WaitForClient()` when `client_count_ == 0` (lazy encoding, 0% idle CPU)
* `AddClient()` / `RemoveClient()` update atomic counter and signal `client_cv_`
* Pipeline pushes fragments with monotonic sequence numbers
* Each HTTP client maintains its own read cursor (sequence number)
* Late joiners or slow clients skip to the latest IDR fragment
* Init segment is sent once per client connection
* `SetInitSegment()` invalidates old fragments on encoder reinit (resolution/format change)

## HTTP Endpoints

| Endpoint | Method | Description |
|---|---|---|
| `/` | GET | Tiled dashboard — grid of all topics, click-to-maximize, status dots, per-tile HUD |
| `/view/{topic}` | GET | Single-topic MSE player page |
| `/stream/{topic}` | GET | Raw fMP4 stream (`Content-Type: video/mp4`, chunked transfer encoding) |
| `/topics` | GET | JSON list of available topics |
| `/status` | GET | Per-topic stats (JSON) |

### Stream Protocol

1. Client connects: `GET /stream/{topic_name}`
2. Handler waits for init segment before sending (cold start fix — avoids sending data before encoder produces first IDR)
3. Server responds: `Content-Type: video/mp4`, chunked transfer encoding
4. First chunk: init segment (ftyp + moov with SPS/PPS in avcC box)
5. Subsequent chunks: media segments (moof + mdat) as produced by pipeline
6. Client disconnect: write error caught in handler, handler returns cleanly

### Dashboard

The `/` endpoint serves a tiled grid of all topics. Features: click-to-maximize any tile,
status dots per tile (green = streaming, yellow = waiting, red = stale), per-tile HUD
overlay, and automatic page reload on tab refocus.

### Topics Endpoint

The `/topics` endpoint returns JSON:

```json
{
  "topics": [
    {"name": "front_camera", "path": "/stream/front_camera"},
    {"name": "rear_camera", "path": "/stream/rear_camera"}
  ]
}
```

### Status Endpoint

The `/status` endpoint returns per-topic stats:

```json
{
  "topics": {
    "front_camera": {
      "state": "streaming",
      "width": 1280,
      "height": 720,
      "measured_fps_x10": 148,
      "frames_encoded": 5432,
      "frames_dropped": 3,
      "encoder_reinits": 0,
      "last_frame_ago_ms": 67
    }
  }
}
```

Pipeline states: `waiting_for_shm` -> `waiting_for_frames` -> `streaming` -> `stale`
(stale is derived in the `/status` handler when `last_frame_ago_ms > 2000`).

## Time Synchronization

All pipelines share a common monotonic epoch via a `TimeBase` singleton backed by
`std::chrono::steady_clock`. Presentation timestamps (PTS) in fMP4 fragments are
computed relative to this epoch.

Optionally, `CameraFrameBuffer::timestamp_nanos` (from the camera driver) can be
used for wall-clock correlation if available.

## Graceful Shutdown

```text
SIGINT/SIGTERM
  -> StreamingControl::Stop()        // existing singleton, sets atomic flag
  -> Monitor::Run() loops exit       // check IsRunning() per iteration
  -> TopicPipeline::Stop()           // request_stop() + join via std::jthread
  -> HttpServer::Stop()              // httplib::Server::stop(), closes listener
  -> FragmentRing cv_.notify_all()   // unblock waiting HTTP handlers
  -> RAII cleanup
  -> exit 0
```

Uses the existing `StreamingControl` singleton with signal handling — no new
signal infrastructure needed.

## Future Extension: RTSP

```text
Pipeline -> [encoded NAL units] -+-> fMP4 muxer -> HTTP server (browsers)
                                 +-> (future) RTP muxer -> RTSP server (VLC, etc.)
```

The encoding is the expensive operation. Adding RTSP later requires only a new
muxer and server module. The pipeline and FragmentRing architecture remain unchanged.

## Static Binary Strategy

* `linkopts = ["-static", "-lpthread"]` under `prod_config` select
* Cross-compilation via `toolchains_llvm` (LLVM 21) with `--target=aarch64-linux-gnu`
* Expected binary size: 5-15MB
* Validation: `ldd video_monitor` shows `not a dynamic executable`
