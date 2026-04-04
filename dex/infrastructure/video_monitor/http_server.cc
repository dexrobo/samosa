#include "dex/infrastructure/video_monitor/http_server.h"

#include <chrono>
#include <sstream>

#include "httplib.h"
#include "spdlog/spdlog.h"

namespace dex::video_monitor {

// Minimal MSE player page. The browser fetches the fMP4 stream via fetch() and
// appends chunks to a SourceBuffer. This is the standard way to play live fMP4
// in browsers — <video src="..."> does not support chunked live streams.
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
  const ms = new MediaSource();
  video.src = URL.createObjectURL(ms);

  ms.addEventListener('sourceopen', async () => {
    const sb = ms.addSourceBuffer('video/mp4; codecs="avc1.42c028"');
    // sequence mode: browser auto-generates timestamps starting from 0 for each
    // new connection, ignoring tfdt. This ensures late joiners and page refreshes
    // always start cleanly at t=0.
    sb.mode = 'sequence';

    const response = await fetch(streamUrl);
    const reader = response.body.getReader();

    // Append queue — serializes appendBuffer calls to avoid InvalidStateError.
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

    // Live edge tracker — keeps playback pinned to the latest data.
    setInterval(() => {
      if (sb.buffered.length === 0) return;
      const start = sb.buffered.start(0);
      const end = sb.buffered.end(0);
      const behind = end - video.currentTime;

      // Evict old data to cap memory (~6s retained).
      if (end - start > 8 && !sb.updating) {
        try { sb.remove(0, end - 6); } catch(e) {}
      }

      // Jump to live edge if fallen behind.
      if (behind > 1.5 || video.currentTime < start) {
        video.currentTime = end - 0.1;
      }

      // Auto-resume if paused from buffer underrun.
      if (video.paused) video.play().catch(() => {});

      hud.textContent = 'buf: ' + (end - start).toFixed(1) + 's | behind: ' + behind.toFixed(1) + 's';
    }, 300);

    // Read stream chunks and enqueue for append.
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

// NOLINTEND

struct HttpServer::Impl {
  httplib::Server server;
};

HttpServer::HttpServer(const ServerConfig& config, std::vector<TopicEndpoint> endpoints)
    : config_(config), endpoints_(std::move(endpoints)), impl_(std::make_unique<Impl>()) {
  // GET /view/{topic} — browser player page using MSE.
  for (const auto& ep : endpoints_) {
    std::string view_path = "/view/" + ep.name;
    std::string stream_path = ep.path;
    impl_->server.Get(view_path, [stream_path](const httplib::Request& /*req*/, httplib::Response& res) {
      std::string page = kPlayerPageTemplate;
      // Replace placeholder with the stream URL (relative path works).
      size_t pos = page.find("%STREAM_URL%");
      if (pos != std::string::npos) {
        page.replace(pos, 12, stream_path);
      }
      res.set_content(page, "text/html");
    });
  }

  // GET / — index page listing all viewer links.
  impl_->server.Get("/", [this](const httplib::Request& /*req*/, httplib::Response& res) {
    std::ostringstream html;
    html << "<!DOCTYPE html><html><head><title>Video Monitor</title>"
         << "<style>body{font-family:sans-serif;margin:2em;background:#1a1a1a;color:#eee;}"
         << "a{color:#6af;font-size:1.2em;}</style></head><body>"
         << "<h1>Video Monitor</h1><ul>";
    for (const auto& ep : endpoints_) {
      html << "<li><a href=\"/view/" << ep.name << "\">" << ep.name << "</a>"
           << " (<a href=\"" << ep.path << "\">raw stream</a>)</li>";
    }
    html << "</ul><p><a href=\"/status\">Pipeline Status</a></p></body></html>";
    res.set_content(html.str(), "text/html");
  });

  // GET /topics — list available streams.
  impl_->server.Get("/topics", [this](const httplib::Request& /*req*/, httplib::Response& res) {
    std::ostringstream json;
    json << R"({"topics":[)";
    for (size_t i = 0; i < endpoints_.size(); ++i) {
      if (i > 0) json << ",";
      json << R"({"name":")" << endpoints_[i].name << R"(","path":")" << endpoints_[i].path << R"("})";
    }
    json << "]}";
    res.set_content(json.str(), "application/json");
  });

  // GET /status — live pipeline status for all topics.
  impl_->server.Get("/status", [this, start_time = std::chrono::steady_clock::now()](const httplib::Request& /*req*/,
                                                                                     httplib::Response& res) {
    auto now = std::chrono::steady_clock::now();
    auto uptime_sec = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
    auto now_ns =
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count());

    std::ostringstream json;
    json << R"({"uptime_sec":)" << uptime_sec << R"(,"topics":{)";

    for (size_t i = 0; i < endpoints_.size(); ++i) {
      if (i > 0) json << ",";
      const auto& ep = endpoints_[i];
      json << R"(")" << ep.name << R"(":{)";

      if (ep.stats != nullptr) {
        auto state = ep.stats->GetState();
        auto last_ts = ep.stats->last_frame_timestamp_ns.load(std::memory_order_relaxed);
        uint64_t last_frame_ago_ms = 0;
        if (last_ts > 0 && now_ns > last_ts) {
          last_frame_ago_ms = (now_ns - last_ts) / 1000000;
        }

        json << R"("state":")" << PipelineStateToString(state) << R"(",)";
        json << R"("resolution":")" << ep.stats->width.load(std::memory_order_relaxed) << "x"
             << ep.stats->height.load(std::memory_order_relaxed) << R"(",)";
        json << R"("target_fps":)" << ep.topic_config.target_fps << ",";
        json << R"("measured_fps":)" << (ep.stats->measured_fps_x10.load(std::memory_order_relaxed) / 10.0) << ",";
        json << R"("bitrate_kbps":)" << ep.topic_config.bitrate_kbps << ",";
        json << R"("frames_encoded":)" << ep.stats->frames_encoded.load(std::memory_order_relaxed) << ",";
        json << R"("frames_dropped":)" << ep.stats->frames_dropped.load(std::memory_order_relaxed) << ",";
        json << R"("clients_connected":)" << ep.stats->clients_connected.load(std::memory_order_relaxed) << ",";
        json << R"("last_frame_ago_ms":)" << last_frame_ago_ms << ",";
        json << R"("encoder_reinits":)" << ep.stats->encoder_reinits.load(std::memory_order_relaxed);
      } else {
        json << R"("state":"unknown")";
      }

      json << "}";
    }

    json << "}}";
    res.set_content(json.str(), "application/json");
  });

  // GET /stream/{topic} — fMP4 video stream per topic.
  for (auto& ep : endpoints_) {
    impl_->server.Get(ep.path, [ring = ep.ring](const httplib::Request& /*req*/, httplib::Response& res) {
      res.set_header("Access-Control-Allow-Origin", "*");
      res.set_header("Cache-Control", "no-cache, no-store");
      res.set_header("Connection", "keep-alive");

      res.set_chunked_content_provider(
          "video/mp4",
          [ring](size_t /*offset*/, httplib::DataSink& sink) -> bool {
            uint64_t last_seq = 0;

            // First read: get init segment + fragments from latest IDR.
            auto result = ring->ReadFrom(0);
            if (result.init_segment.has_value()) {
              if (!sink.write(reinterpret_cast<const char*>(result.init_segment->data()),
                              result.init_segment->size())) {
                return false;  // Client disconnected.
              }
            }
            for (const auto& frag : result.fragments) {
              if (!sink.write(reinterpret_cast<const char*>(frag->data()), frag->size())) {
                return false;
              }
            }
            last_seq = result.last_sequence;

            // Streaming loop: wait for new fragments and send them.
            while (true) {
              if (!ring->WaitForNew(last_seq, std::chrono::milliseconds(1000))) {
                // Timeout — send empty to keep connection alive, or check for shutdown.
                // The ring's WaitForNew returns false on shutdown (NotifyAll).
                // We can check by seeing if the head hasn't moved.
                if (ring->HeadSequence() == last_seq) {
                  continue;  // No new data, just a timeout. Keep waiting.
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
          [](bool /*success*/) {
            // Connection closed callback — nothing to clean up.
          });
    });
  }
}

HttpServer::~HttpServer() { Stop(); }

void HttpServer::Start() {
  if (running_) return;
  running_ = true;

  SPDLOG_INFO("HTTP server starting on {}:{}", config_.bind_address, config_.port);

  listener_thread_ = std::thread([this] {
    if (!impl_->server.listen(config_.bind_address, config_.port)) {
      SPDLOG_ERROR("HTTP server failed to listen on {}:{}", config_.bind_address, config_.port);
    }
  });
}

void HttpServer::Stop() {
  if (!running_) return;
  running_ = false;

  impl_->server.stop();
  if (listener_thread_.joinable()) {
    listener_thread_.join();
  }

  SPDLOG_INFO("HTTP server stopped");
}

}  // namespace dex::video_monitor
