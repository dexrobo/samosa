# Architecture — Video Monitor Binary

## System Overview

```text
SharedMem Topic A ──> Pipeline Thread A ──> FragmentRing A ──┐
SharedMem Topic B ──> Pipeline Thread B ──> FragmentRing B ──┤──> HTTP Server Thread
SharedMem Topic C ──> Pipeline Thread C ──> FragmentRing C ──┘     GET /stream/{topic}
                                                                    GET /topics
```

The binary runs three types of threads:

1. **Main thread** — loads config, creates pipelines and HTTP server, waits for shutdown
2. **Pipeline threads** (one per topic) — read shared memory, encode, mux, push to ring
3. **HTTP server thread** (+handler pool) — serves video streams to clients

## Pipeline Architecture

Each topic runs an independent pipeline on a dedicated thread:

```text
Monitor::ReadInto(frame_buf)           // ~16MB copy from shm (passive, non-blocking)
  -> color_convert(RGB24 -> I420)      // BT.601 matrix, ~2-3ms for 1080p
  -> OpenH264::Encode(I420) -> NALs   // ~10-15ms for 1080p Baseline
  -> FMP4Muxer::Mux(NALs) -> fragment // init segment once, then moof+mdat per frame
  -> FragmentRing::Push(fragment)      // publish for HTTP fan-out
```

### Data Sizes (per topic)

| Buffer | Size | Lifetime |
|---|---|---|
| CameraFrameBuffer (read copy) | ~16MB | Thread-local, reused |
| I420 YUV buffer | ~3MB (1920x1080x1.5) | Thread-local, reused |
| Encoded NALs | ~10-50KB per frame | Transient |
| fMP4 fragment | ~10-50KB per frame | Shared via FragmentRing |

## Thread Ownership Model

**Model: Thread confinement with message passing via FragmentRing.**

| Thread | Count | Owns | Communicates via |
|---|---|---|---|
| Main | 1 | Config, pipeline lifecycle, shutdown | StreamingControl singleton |
| HTTP server | 1 (+handler pool) | httplib::Server, endpoint routing | FragmentRing (reader) |
| Pipeline | 1 per topic | Monitor, CameraFrameBuffer, YUV buf, Encoder, Muxer | FragmentRing (sole writer) |

### Concurrency Rules

* **Thread confinement**: each pipeline thread owns all its mutable resources. No shared
  mutable state between pipelines.
* **Single writer per ring**: the pipeline thread is the sole writer to its FragmentRing.
  HTTP handler threads are readers only.
* **No shared variables between writers**: per AGENTS.md H.1, no flags or variables are
  shared between multiple writer threads.

### Synchronization

* **Shared memory read path**: uses the existing lock-free atomic protocol (futex +
  sequence validation). No additional synchronization needed.
* **FragmentRing fan-out**: uses `std::mutex` + `std::condition_variable`. This is on the
  HTTP serving path, not the shared memory hot path. Acceptable latency.

## Broadcast Pattern (FragmentRing)

The FragmentRing is the fan-out mechanism between the pipeline (sole writer) and HTTP
handlers (multiple readers):

```text
FragmentRing {
  ring_: vector<Fragment>       // Fixed-size circular buffer
  head_sequence_: uint64        // Monotonic write cursor
  latest_idr_sequence_: uint64  // For late-joiner catch-up
  init_segment_: vector<uint8>  // ftyp+moov, sent to each new client
  mutex_ + cv_                  // Reader synchronization
}

Fragment {
  data: shared_ptr<const vector<uint8>>  // fMP4 bytes (moof + mdat)
  sequence: uint64                        // Monotonic fragment sequence
  timestamp_us: uint64                    // PTS from shared time base
  contains_idr: bool                      // Starts with a keyframe?
}
```

* Pipeline pushes fragments with monotonic sequence numbers
* Each HTTP client maintains its own read cursor (sequence number)
* Late joiners or slow clients skip to the latest IDR fragment
* Init segment is sent once per client connection

## HTTP Streaming Protocol

1. Client connects: `GET /stream/{topic_name}`
2. Server responds: `Content-Type: video/mp4`, chunked transfer encoding
3. First chunk: init segment (ftyp + moov with SPS/PPS in avcC box)
4. Subsequent chunks: media segments (moof + mdat) as produced by pipeline
5. Client disconnect: write error caught in handler, handler returns cleanly

The `/topics` endpoint returns JSON:

```json
{
  "topics": [
    {"name": "front_camera", "path": "/stream/front_camera"},
    {"name": "rear_camera", "path": "/stream/rear_camera"}
  ]
}
```

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
