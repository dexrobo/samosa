# Design — Video Monitor Binary

## Technology Decisions

| Decision | Choice | License | Rationale |
|---|---|---|---|
| Language | C++ | N/A | Direct `#include` of shared memory types. No FFI, no layout sync. |
| Encoder | OpenH264 | BSD-2-Clause | Permissive. x264 is GPL-2.0 (ruled out). Cisco-backed. Baseline through High profile. |
| HTTP server | cpp-httplib | MIT | Header-only, zero deps, chunked transfer encoding. |
| Config format | toml++ | MIT | Header-only, zero deps. Project already uses TOML conventions. |
| fMP4 muxer | Custom | N/A | No permissive C++ fMP4 library exists. ISO BMFF box structure is simple (~500-700 lines). |
| Time sync | Shared monotonic clock | N/A | `steady_clock` epoch across pipelines. Optional `timestamp_nanos` for wall-clock. |

## File Structure

```text
dex/infrastructure/video_monitor/
    BUILD.bazel
    PLAN.md
    docs/
        requirements.md
        architecture.md
        design.md
    main.cc                             # Entry point, config, orchestration
    config.h / config.cc                # TOML parsing + CLI overrides + validation
    pipeline.h / pipeline.cc            # TopicPipeline: monitor -> downsample -> encode -> mux -> broadcast
    pipeline_stats.h                    # PipelineStats atomic counters
    encoder.h / encoder.cc              # OpenH264 wrapper
    color_convert.h / color_convert.cc  # RGB24/BGR24 -> I420 (NEON asm / NASM asm via select())
    downsample.h / downsample.cc        # Bilinear downsampling to max_width/max_height
    fmp4_muxer.h / fmp4_muxer.cc       # Minimal ISO BMFF fMP4 muxer
    fragment_ring.h / fragment_ring.cc  # Broadcast ring buffer + client tracking
    http_server.h / http_server.cc      # HTTP endpoint setup + dashboard + status
    time_base.h                         # Shared monotonic clock singleton
```

## Key Interfaces

### Config (`config.h`)

```cpp
struct TopicConfig {
    std::string shm_name;           // Shared memory segment name (e.g., "/front_camera")
    std::string endpoint;           // HTTP path suffix (e.g., "front_camera")
    uint32_t target_fps = 15;
    uint32_t bitrate_kbps = 1500;
    uint32_t keyframe_interval = 30; // Frames between IDRs
    uint32_t max_width = 1280;       // Bilinear downsample cap
    uint32_t max_height = 720;       // Bilinear downsample cap
};

struct ServerConfig {
    std::string bind_address = "0.0.0.0";
    uint16_t port = 8080;
    uint32_t fragment_ring_size = 120; // ~4 seconds at 30fps
};

struct MonitorConfig {
    ServerConfig server;
    std::vector<TopicConfig> topics;
};

MonitorConfig LoadConfig(const std::string& config_path, int argc, char** argv);
```

### Encoder (`encoder.h`)

```cpp
class H264Encoder {
public:
    struct Params {
        uint32_t width, height, fps, bitrate_kbps, keyframe_interval;
    };

    explicit H264Encoder(const Params& params);
    ~H264Encoder();

    H264Encoder(const H264Encoder&) = delete;
    H264Encoder& operator=(const H264Encoder&) = delete;
    H264Encoder(H264Encoder&&) noexcept;
    H264Encoder& operator=(H264Encoder&&) noexcept;

    struct EncodedFrame {
        std::vector<uint8_t> nals;  // Annex B NAL units
        bool is_idr;
        uint64_t pts;
    };

    std::optional<EncodedFrame> Encode(const uint8_t* yuv_i420, uint64_t timestamp_us);
};
```

### FMP4Muxer (`fmp4_muxer.h`)

```cpp
class FMP4Muxer {
public:
    struct TrackParams {
        uint32_t width, height;
        uint32_t timescale = 90000;     // Standard H.264 timescale
        std::vector<uint8_t> sps, pps;  // From first IDR
    };

    explicit FMP4Muxer(const TrackParams& params);

    std::vector<uint8_t> GetInitSegment() const;

    std::vector<uint8_t> MuxFragment(
        const std::vector<uint8_t>& annex_b_nals,
        uint64_t decode_time,
        uint32_t duration,
        bool is_idr);
};
```

### FragmentRing (`fragment_ring.h`)

```cpp
struct Fragment {
    std::shared_ptr<const std::vector<uint8_t>> data;
    uint64_t sequence;
    uint64_t timestamp_us;
    bool contains_idr;
};

class FragmentRing {
public:
    explicit FragmentRing(size_t capacity);

    // Producer (single writer)
    void Push(Fragment fragment);
    void SetInitSegment(std::vector<uint8_t> init);  // Invalidates old fragments on reinit

    // Consumer (multiple readers)
    struct ReadResult {
        std::optional<std::vector<uint8_t>> init_segment;
        std::vector<std::shared_ptr<const std::vector<uint8_t>>> fragments;
        uint64_t last_sequence;
    };
    ReadResult ReadFrom(uint64_t after_sequence) const;
    bool WaitForNew(uint64_t after_sequence, std::chrono::milliseconds timeout) const;

    // Client tracking (lazy encoding gate)
    void AddClient();
    void RemoveClient();
    void WaitForClient();  // Blocks pipeline when client_count_ == 0

    // Shutdown
    void NotifyAll();
};
```

### TopicPipeline (`pipeline.h`)

```cpp
class TopicPipeline {
public:
    TopicPipeline(const TopicConfig& config, FragmentRing& ring);
    ~TopicPipeline();

    void Start();
    void Stop();
    bool IsRunning() const;

private:
    void Run();  // Thread entry point
    // Owns: Monitor<CameraFrameBuffer>, CameraFrameBuffer, YUV buffer,
    //       H264Encoder, FMP4Muxer — all created inside Run()
};
```

### HttpServer (`http_server.h`)

```cpp
class HttpServer {
public:
    struct TopicEndpoint {
        std::string path;
        FragmentRing* ring;
    };

    HttpServer(const ServerConfig& config, std::vector<TopicEndpoint> endpoints);
    void Start();
    void Stop();
};
```

### PipelineStats (`pipeline_stats.h`)

```cpp
struct PipelineStats {
    std::atomic<uint64_t> frames_encoded{0};
    std::atomic<uint64_t> frames_dropped{0};
    std::atomic<uint64_t> last_frame_timestamp_ns{0};
    std::atomic<uint32_t> width{0};
    std::atomic<uint32_t> height{0};
    std::atomic<uint32_t> encoder_reinits{0};
    std::atomic<uint32_t> measured_fps_x10{0};  // e.g. 148 = 14.8 fps
};
```

Updated atomically by each pipeline thread. Read lock-free by the `/status` HTTP handler.

## Color Conversion

RGB24/BGR24 to I420 (YUV 4:2:0 planar) using BT.601 coefficients:

```text
Y  =  0.299 * R + 0.587 * G + 0.114 * B
Cb = -0.169 * R - 0.331 * G + 0.500 * B + 128
Cr =  0.500 * R - 0.419 * G - 0.081 * B + 128
```

Processes 2 rows at a time (chroma is subsampled 2x2 in I420). Format auto-detected
from `CameraFrameBuffer::color_format` field — swap R/B coefficients for BGR24.

Platform-specific SIMD via Bazel `select()`:
* **ARM64**: NEON assembly intrinsics
* **x86_64**: NASM assembly

## Configuration

### TOML File

```toml
[server]
bind_address = "0.0.0.0"
port = 8080
fragment_ring_size = 120

[[topics]]
shm_name = "/front_camera"
endpoint = "front_camera"
target_fps = 15
bitrate_kbps = 1500
keyframe_interval = 30
max_width = 1280
max_height = 720

[[topics]]
shm_name = "/rear_camera"
endpoint = "rear_camera"
target_fps = 15
bitrate_kbps = 1000
keyframe_interval = 30
max_width = 640
max_height = 480
```

### CLI Overrides

```bash
video_monitor --config /etc/video_monitor.toml --port 9090 --topic /cam0
video_monitor --help
```

CLI flags override config file values. `--topic` adds a topic with default settings
when no config file is provided. Unknown flags are rejected with an error message.
`--help` prints usage and exits. Duplicate topics are deduplicated.

## Error Handling

| Scenario | Behavior |
|---|---|
| Shared memory not available | Pipeline retries with exponential backoff (1s, 2s, 4s, ... 30s max) |
| Encoder init failure | Fatal for that pipeline. Logged via spdlog. Other pipelines continue. |
| Frame read timeout | Skip iteration, continue loop. Expected when no new frame available. |
| Encoder drops frame | `Encode()` returns nullopt. Skip. Rate control decision. |
| HTTP client disconnect | Write error caught in handler. Handler returns. No effect on pipeline or other clients. |
| Invalid color format | Pipeline logs error, skips frame. Retries on next frame (format may change). |

## Existing Code Reused

| File | What |
|---|---|
| `dex/infrastructure/shared_memory/shared_memory_monitor.h` | `Monitor<CameraFrameBuffer>` for passive reads |
| `dex/infrastructure/shared_memory/streaming_control.h` | `StreamingControl` singleton for signal handling |
| `dex/drivers/camera/base/types.h` | `CameraFrameBuffer` POD struct |

## Risks and Mitigations

| Risk | Mitigation | Status |
|---|---|---|
| OpenH264 Bazel build complexity (NASM for x86 asm) | ARM64 NEON asm + x86_64 NASM asm via platform `select()`. | Resolved |
| fMP4 spec non-compliance | Validate all output with ffprobe. Byte-level box structure assertions in tests. | Ongoing |
| `color_format` changes mid-stream | Detect format per-frame. Re-init encoder if dimensions or format change. `SetInitSegment()` invalidates stale fragments. | Resolved |
| Encoding too slow for use case | Bilinear downsampling to configurable `max_width`/`max_height` (default 1280x720). Lazy encoding skips work when 0 clients. | Resolved |
