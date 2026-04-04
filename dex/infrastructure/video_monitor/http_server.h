#ifndef DEX_INFRASTRUCTURE_VIDEO_MONITOR_HTTP_SERVER_H
#define DEX_INFRASTRUCTURE_VIDEO_MONITOR_HTTP_SERVER_H

#include <string>
#include <thread>
#include <vector>

#include "dex/infrastructure/video_monitor/config.h"
#include "dex/infrastructure/video_monitor/fragment_ring.h"
#include "dex/infrastructure/video_monitor/pipeline_stats.h"

namespace dex::video_monitor {

class HttpServer {
 public:
  struct TopicEndpoint {
    std::string name;
    std::string path;            // e.g., "/stream/front_camera"
    FragmentRing* ring;          // Non-owning.
    const PipelineStats* stats;  // Non-owning. For /status endpoint.
    TopicConfig topic_config;    // For reporting target_fps, bitrate, etc.
  };

  HttpServer(const ServerConfig& config, std::vector<TopicEndpoint> endpoints);
  ~HttpServer();

  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;
  HttpServer(HttpServer&&) = delete;
  HttpServer& operator=(HttpServer&&) = delete;

  void Start();
  void Stop();

 private:
  ServerConfig config_;
  std::vector<TopicEndpoint> endpoints_;
  std::thread listener_thread_;
  bool running_{false};

  // Implementation is in the .cc to avoid httplib.h in the header.
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace dex::video_monitor

#endif  // DEX_INFRASTRUCTURE_VIDEO_MONITOR_HTTP_SERVER_H
