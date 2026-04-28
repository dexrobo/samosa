#include "dex/infrastructure/video_monitor/http_server.h"

#include <chrono>
#include <sstream>
#include <utility>

#include "httplib.h"
#include "spdlog/spdlog.h"

namespace dex::video_monitor {

// Minimal MSE player page. The browser fetches the fMP4 stream via fetch() and
// appends chunks to a SourceBuffer. This is the standard way to play live fMP4
// in browsers -- <video src="..."> does not support chunked live streams.
// NOLINTBEGIN
constexpr const char* kPlayerPageTemplate = R"html(<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>Video Monitor</title>
<style>
  body { margin:0; background:#1a1a1a; display:flex; flex-direction:column;
         justify-content:center; align-items:center; min-height:100vh; font-family:sans-serif; color:#eee; }
  video { max-width:95vw; max-height:85vh; background:#000; }
  #hud { font-size:12px; color:#888; margin-top:8px; font-family:monospace; }
  .error { color:#f66; padding:2em; font-size:1.2em; }
</style></head><body>
<video id="v" autoplay muted controls></video>
<div id="hud"></div>
<script>
const streamUrl = '%STREAM_URL%';
const video = document.getElementById('v');
const hud = document.getElementById('hud');

if (!('MediaSource' in window)) {
  document.body.innerHTML = '<div class="error">MediaSource API not supported in this browser.</div>';
} else {
  document.addEventListener('visibilitychange', () => {
    if (!document.hidden) location.reload();
  });

  const ms = new MediaSource();
  video.src = URL.createObjectURL(ms);

  ms.addEventListener('sourceopen', async () => {
    const sb = ms.addSourceBuffer('video/mp4; codecs="avc1.42c028"');
    sb.mode = 'sequence';

    const response = await fetch(streamUrl);
    const reader = response.body.getReader();

    const queue = [];
    let appending = false;

    function processQueue() {
      if (appending || queue.length === 0 || ms.readyState !== 'open') return;
      appending = true;
      try {
        sb.appendBuffer(queue.shift());
      } catch (e) {
        console.error('appendBuffer error:', e);
        appending = false;
      }
    }

    sb.addEventListener('updateend', () => {
      appending = false;
      processQueue();
    });

    setInterval(() => {
      if (sb.buffered.length === 0) return;
      const start = sb.buffered.start(0);
      const end = sb.buffered.end(0);
      const behind = end - video.currentTime;
      if (end - start > 8 && !sb.updating) {
        try { sb.remove(0, end - 6); } catch(e) {}
      }
      if (behind > 1.5 || video.currentTime < start) {
        video.currentTime = end - 0.1;
      }
      if (video.paused) video.play().catch(() => {});
      const lag = Math.max(0, behind);
      hud.textContent = 'buffered ' + (end - start).toFixed(1) + 's | lag -' + lag.toFixed(1) + 's';
    }, 300);

    async function pump() {
      while (true) {
        const { done, value } = await reader.read();
        if (done) break;
        queue.push(value);
        processQueue();
      }
    }
    pump().catch(e => console.error('Stream error:', e));
  });
}
</script></body></html>)html";

// Dashboard page: tiled grid of all topics with click-to-maximize.
// %TOPICS_JSON% is replaced with a JSON array of {name, stream} objects.
constexpr const char* kDashboardPageTemplate = R"html(<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>Video Monitor — Dashboard</title>
<style>
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body { background: #111; font-family: sans-serif; color: #eee; overflow: hidden; height: 100vh; }
  #header { display: flex; align-items: center; padding: 6px 16px; background: #1a1a1a;
            border-bottom: 1px solid #333; gap: 16px; }
  #header h1 { font-size: 14px; font-weight: 500; opacity: 0.7; }
  #header a { color: #6af; font-size: 12px; text-decoration: none; }
  #grid { display: grid; gap: 2px; padding: 2px; height: calc(100vh - 36px); }
  .tile { position: relative; background: #000; overflow: hidden; cursor: pointer;
          display: flex; align-items: center; justify-content: center; }
  .tile video { width: 100%; height: 100%; object-fit: contain; }
  .tile .label { position: absolute; top: 6px; left: 8px; font-size: 11px; color: #fff;
                 background: rgba(0,0,0,0.6); padding: 2px 8px; border-radius: 3px;
                 pointer-events: none; z-index: 2; display: flex; align-items: center; gap: 6px; }
  .tile .dot { width: 8px; height: 8px; border-radius: 50%; flex-shrink: 0; }
  .dot.live { background: #4f4; animation: pulse 1s infinite; }
  .dot.starting { background: #fa0; animation: pulse 0.5s infinite; }
  .dot.stale { background: #fa0; }
  .dot.offline { background: #f44; }
  @keyframes pulse { 0%,100% { opacity:1; } 50% { opacity:0.4; } }
  .tile .hud { position: absolute; bottom: 4px; left: 8px; right: 8px; font-size: 10px; color: #888;
               font-family: monospace; pointer-events: none; z-index: 2;
               display: flex; justify-content: space-between; }
  .tile.maximized { position: fixed; top: 0; left: 0; width: 100vw; height: 100vh; z-index: 10; }
  .tile.maximized .label { font-size: 13px; top: 10px; left: 12px; }
</style></head><body>
<div id="header">
  <h1>Video Monitor</h1>
  <a href="/status">status</a>
</div>
<div id="grid"></div>
<script>
const topics = %TOPICS_JSON%;
const grid = document.getElementById('grid');

// Reload on tab refocus -- the fetch stream buffers data while backgrounded
// and draining the backlog takes too long. A reload reconnects fresh from
// the latest IDR.
document.addEventListener('visibilitychange', () => {
  if (!document.hidden) location.reload();
});

// Compute grid layout based on topic count.
const n = topics.length;
const cols = n <= 1 ? 1 : n <= 2 ? 2 : n <= 4 ? 2 : n <= 6 ? 3 : n <= 9 ? 3 : 4;
grid.style.gridTemplateColumns = 'repeat(' + cols + ', 1fr)';

function createPlayer(topic) {
  const tile = document.createElement('div');
  tile.className = 'tile';

  const label = document.createElement('div');
  label.className = 'label';
  const dot = document.createElement('span');
  dot.className = 'dot offline';
  label.appendChild(dot);
  const nameSpan = document.createElement('span');
  nameSpan.textContent = topic.name;
  label.appendChild(nameSpan);
  tile.appendChild(label);

  const video = document.createElement('video');
  video.autoplay = true; video.muted = true; video.playsInline = true;
  tile.appendChild(video);

  const hud = document.createElement('div');
  hud.className = 'hud';
  const hudLeft = document.createElement('span');
  const hudRight = document.createElement('span');
  hud.appendChild(hudLeft);
  hud.appendChild(hudRight);
  tile.appendChild(hud);

  // Click to maximize/restore.
  tile.addEventListener('click', () => {
    if (tile.classList.contains('maximized')) {
      tile.classList.remove('maximized');
    } else {
      document.querySelectorAll('.tile.maximized').forEach(t => t.classList.remove('maximized'));
      tile.classList.add('maximized');
    }
  });

  if (!('MediaSource' in window)) {
    hud.textContent = 'MSE not supported';
    grid.appendChild(tile);
    return;
  }

  const ms = new MediaSource();
  video.src = URL.createObjectURL(ms);

  ms.addEventListener('sourceopen', async () => {
    const sb = ms.addSourceBuffer('video/mp4; codecs="avc1.42c028"');
    sb.mode = 'sequence';

    const response = await fetch(topic.stream);
    const reader = response.body.getReader();

    const queue = [];
    let appending = false;

    function processQueue() {
      if (appending || queue.length === 0 || ms.readyState !== 'open') return;
      appending = true;
      try { sb.appendBuffer(queue.shift()); }
      catch (e) { console.error(topic.name, 'append error:', e); appending = false; }
    }

    sb.addEventListener('updateend', () => {
      appending = false;
      processQueue();
    });

    function seekToLive() {
      if (sb.buffered.length === 0) return;
      const start = sb.buffered.start(0);
      const end = sb.buffered.end(0);
      const behind = end - video.currentTime;
      if (end - start > 8 && !sb.updating) {
        try { sb.remove(0, end - 6); } catch(e) {}
      }
      if (behind > 1.5 || video.currentTime < start) {
        video.currentTime = end - 0.1;
      }
      if (video.paused) video.play().catch(() => {});
      const lag = Math.max(0, behind);
      hudLeft.textContent = 'buffered ' + (end - start).toFixed(1) + 's | lag -' + lag.toFixed(1) + 's';
    }

    setInterval(seekToLive, 300);

    async function pump() {
      while (true) {
        const { done, value } = await reader.read();
        if (done) break;
        queue.push(value);
        processQueue();
      }
    }
    pump().catch(e => console.error(topic.name, 'stream error:', e));
  });

  grid.appendChild(tile);
  return { name: topic.name, dot: dot, hudRight: hudRight, video: video };
}

const players = topics.map(createPlayer);

// Poll /status to update dots, FPS, clients, drops.
setInterval(async () => {
  try {
    const res = await fetch('/status');
    const data = await res.json();
    for (const p of players) {
      const t = data.topics[p.name];
      if (!t) continue;
      // Status dot based on server state + client-side video buffer.
      const hasVideo = p.video.buffered.length > 0;
      const serverLive = t.state === 'streaming' && t.last_frame_ago_ms < 2000;
      let dotClass;
      if (serverLive && hasVideo) dotClass = 'live';           // green pulsing: playing
      else if (serverLive && !hasVideo) dotClass = 'starting'; // yellow pulsing: encoder warming up
      else if (t.state === 'waiting_for_shm') dotClass = 'offline'; // red: no shm
      else dotClass = 'stale';                                 // yellow solid: no data
      p.dot.className = 'dot ' + dotClass;
      // Right HUD: fps, clients, drops.
      const parts = [];
      parts.push(t.measured_fps.toFixed(1) + ' fps');
      parts.push(t.clients_connected + (t.clients_connected === 1 ? ' client' : ' clients'));
      if (t.frames_dropped > 0) parts.push(t.frames_dropped + ' dropped');
      p.hudRight.textContent = parts.join(' | ');
    }
  } catch(e) {}
}, 1000);
</script></body></html>)html";

// NOLINTEND

struct HttpServer::Impl {
  httplib::Server server;
};

// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
// NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
HttpServer::HttpServer(ServerConfig config, std::vector<TopicEndpoint> endpoints)
    : config_(std::move(config)), endpoints_(std::move(endpoints)), impl_(std::make_unique<Impl>()) {
  // GET /view/{topic} -- browser player page using MSE.
  for (const auto& endpoint : endpoints_) {
    const std::string view_path = "/view/" + endpoint.name;
    const std::string stream_path = endpoint.path;
    impl_->server.Get(view_path, [stream_path](const httplib::Request& /*req*/, httplib::Response& res) {
      std::string page = kPlayerPageTemplate;
      // Replace placeholder with the stream URL (relative path works).
      const size_t pos = page.find("%STREAM_URL%");
      if (pos != std::string::npos) {
        page.replace(pos, 12, stream_path);
      }
      res.set_content(page, "text/html");
    });
  }

  // GET / -- tiled dashboard showing all topics.
  impl_->server.Get("/", [this](const httplib::Request& /*req*/, httplib::Response& res) {
    // Build JSON array of topics for the dashboard template.
    std::ostringstream topics_json;
    topics_json << "[";
    for (size_t idx = 0; idx < endpoints_.size(); ++idx) {
      if (idx > 0) {
        topics_json << ",";
      }
      topics_json << R"({"name":")" << endpoints_[idx].name << R"(","stream":")" << endpoints_[idx].path << R"("})";
    }
    topics_json << "]";

    std::string page = kDashboardPageTemplate;
    const size_t pos = page.find("%TOPICS_JSON%");
    if (pos != std::string::npos) {
      page.replace(pos, 13, topics_json.str());
    }
    res.set_content(page, "text/html");
  });

  // GET /topics -- list available streams.
  impl_->server.Get("/topics", [this](const httplib::Request& /*req*/, httplib::Response& res) {
    std::ostringstream json;
    json << R"({"topics":[)";
    for (size_t idx = 0; idx < endpoints_.size(); ++idx) {
      if (idx > 0) {
        json << ",";
      }
      json << R"({"name":")" << endpoints_[idx].name << R"(","path":")" << endpoints_[idx].path << R"("})";
    }
    json << "]}";
    res.set_content(json.str(), "application/json");
  });

  // GET /status -- live pipeline status for all topics.
  impl_->server.Get("/status", [this, start_time = std::chrono::steady_clock::now()](const httplib::Request& /*req*/,
                                                                                     httplib::Response& res) {
    auto now = std::chrono::steady_clock::now();
    auto uptime_sec = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
    auto now_ns =
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count());

    std::ostringstream json;
    json << R"({"uptime_sec":)" << uptime_sec << R"(,"topics":{)";

    for (size_t idx = 0; idx < endpoints_.size(); ++idx) {
      if (idx > 0) {
        json << ",";
      }
      const auto& endpoint = endpoints_[idx];
      json << R"(")" << endpoint.name << R"(":{)";

      if (endpoint.stats != nullptr) {
        auto state = endpoint.stats->GetState();
        auto last_ts = endpoint.stats->last_frame_timestamp_ns.load(std::memory_order_relaxed);
        uint64_t last_frame_ago_ms = 0;
        if (last_ts > 0 && now_ns > last_ts) {
          last_frame_ago_ms = (now_ns - last_ts) / 1000000;
        }

        // Override state to stale if streaming but no frame for >2s.
        if (state == PipelineState::kStreaming && last_frame_ago_ms > 2000) {
          state = PipelineState::kStale;
        }

        json << R"("state":")" << PipelineStateToString(state) << R"(",)";
        json << R"("resolution":")" << endpoint.stats->width.load(std::memory_order_relaxed) << "x"
             << endpoint.stats->height.load(std::memory_order_relaxed) << R"(",)";
        json << R"("target_fps":)" << endpoint.topic_config.target_fps << ",";
        json << R"("measured_fps":)" << (endpoint.stats->measured_fps_x10.load(std::memory_order_relaxed) / 10.0)
             << ",";
        json << R"("bitrate_kbps":)" << endpoint.topic_config.bitrate_kbps << ",";
        json << R"("frames_encoded":)" << endpoint.stats->frames_encoded.load(std::memory_order_relaxed) << ",";
        json << R"("frames_dropped":)" << endpoint.stats->frames_dropped.load(std::memory_order_relaxed) << ",";
        json << R"("clients_connected":)" << endpoint.ring->ClientCount() << ",";
        json << R"("last_frame_ago_ms":)" << last_frame_ago_ms << ",";
        json << R"("encoder_reinits":)" << endpoint.stats->encoder_reinits.load(std::memory_order_relaxed);
      } else {
        json << R"("state":"unknown")";
      }

      json << "}";
    }

    json << "}}";
    res.set_content(json.str(), "application/json");
  });

  // GET /stream/{topic} -- fMP4 video stream per topic.
  for (auto& endpoint : endpoints_) {
    // NOLINTNEXTLINE(readability-function-cognitive-complexity)
    impl_->server.Get(endpoint.path, [ring = endpoint.ring](const httplib::Request& /*req*/, httplib::Response& res) {
      res.set_header("Access-Control-Allow-Origin", "*");
      res.set_header("Cache-Control", "no-cache, no-store");
      res.set_header("Connection", "keep-alive");

      ring->AddClient();

      res.set_chunked_content_provider(
          "video/mp4",
          // NOLINTNEXTLINE(readability-function-cognitive-complexity)
          [ring](size_t /*offset*/, httplib::DataSink& sink) -> bool {
            uint64_t last_seq = 0;

            // Wait for init segment if not available yet (lazy encoding cold start).
            while (true) {
              auto result = ring->ReadFrom(0);
              if (result.init_segment.has_value()) {
                if (!sink.write(reinterpret_cast<const char*>(result.init_segment->data()),
                                result.init_segment->size())) {
                  return false;
                }
                for (const auto& frag : result.fragments) {
                  if (!sink.write(reinterpret_cast<const char*>(frag->data()), frag->size())) {
                    return false;
                  }
                }
                last_seq = result.last_sequence;
                break;
              }
              // No init segment yet -- wait for data to arrive.
              if (!ring->WaitForNew(0, std::chrono::milliseconds(1000))) {
                continue;  // Timeout, retry.
              }
            }

            // Streaming loop: wait for new fragments and send them.
            while (true) {
              if (!ring->WaitForNew(last_seq, std::chrono::milliseconds(1000))) {
                if (ring->HeadSequence() == last_seq) {
                  continue;
                }
              }

              auto new_result = ring->ReadFrom(last_seq);
              for (const auto& frag : new_result.fragments) {
                if (!sink.write(reinterpret_cast<const char*>(frag->data()), frag->size())) {
                  return false;  // Client disconnected.
                }
              }
              last_seq = new_result.last_sequence;
            }
          },
          [ring](bool /*success*/) { ring->RemoveClient(); });
    });
  }
}

// NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

HttpServer::~HttpServer() { Stop(); }

void HttpServer::Start() {
  if (running_) {
    return;
  }
  running_ = true;

  SPDLOG_INFO("HTTP server starting on {}:{}", config_.bind_address, config_.port);

  listener_thread_ = std::thread([this] {
    if (!impl_->server.listen(config_.bind_address, config_.port)) {
      SPDLOG_ERROR("HTTP server failed to listen on {}:{}", config_.bind_address, config_.port);
    }
  });
}

void HttpServer::Stop() {
  if (!running_) {
    return;
  }
  running_ = false;

  impl_->server.stop();
  if (listener_thread_.joinable()) {
    listener_thread_.join();
  }

  SPDLOG_INFO("HTTP server stopped");
}

}  // namespace dex::video_monitor
