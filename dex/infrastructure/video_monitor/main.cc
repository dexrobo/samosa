#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "fmt/core.h"
#include "spdlog/spdlog.h"

#include "dex/infrastructure/shared_memory/streaming_control.h"
#include "dex/infrastructure/video_monitor/config.h"
#include "dex/infrastructure/video_monitor/fragment_ring.h"
#include "dex/infrastructure/video_monitor/http_server.h"
#include "dex/infrastructure/video_monitor/pipeline.h"

int main(int argc, char** argv) {
  // Configure signal handling via existing infrastructure.
  dex::shared_memory::StreamingControl::SetDefaultConfiguration({.handle_signals = true});

  // Load and validate config.
  dex::video_monitor::MonitorConfig config;
  try {
    config = dex::video_monitor::LoadConfig(argc, const_cast<const char* const*>(argv));
  } catch (const std::exception& err) {
    fmt::print(stderr, "{}\n", err.what());
    return 1;
  }

  if (config.topics.empty()) {
    SPDLOG_ERROR("No topics configured. Use --topic <shm_name> or --config <path>");
    return 1;
  }

  SPDLOG_INFO("Video monitor starting with {} topic(s)", config.topics.size());

  // Create fragment rings, pipelines, and HTTP endpoints.
  std::vector<std::unique_ptr<dex::video_monitor::FragmentRing>> rings;
  std::vector<std::unique_ptr<dex::video_monitor::TopicPipeline>> pipelines;
  std::vector<dex::video_monitor::HttpServer::TopicEndpoint> endpoints;

  std::unordered_set<std::string> seen_shm_names;

  for (const auto& topic : config.topics) {
    if (!seen_shm_names.insert(topic.shm_name).second) {
      SPDLOG_WARN("Skipping duplicate topic '{}'", topic.shm_name);
      continue;
    }

    auto ring = std::make_unique<dex::video_monitor::FragmentRing>(config.server.fragment_ring_size);
    auto pipeline = std::make_unique<dex::video_monitor::TopicPipeline>(topic, *ring);
    std::string path = "/stream/" + topic.endpoint;

    endpoints.push_back({
        .name = topic.endpoint,
        .path = path,
        .ring = ring.get(),
        .stats = &pipeline->Stats(),
        .topic_config = topic,
    });

    pipelines.push_back(std::move(pipeline));
    rings.push_back(std::move(ring));

    SPDLOG_INFO("  Topic '{}' -> {}", topic.shm_name, path);
  }

  // Start HTTP server.
  dex::video_monitor::HttpServer server(config.server, std::move(endpoints));
  server.Start();

  // Start all pipelines.
  for (auto& pipeline : pipelines) {
    pipeline->Start();
  }

  // Wait for shutdown signal.
  auto& control = dex::shared_memory::StreamingControl::Instance();
  while (control.IsRunning()) {
    std::this_thread::sleep_for(
        std::chrono::milliseconds(100));  // NOLINT(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
  }

  SPDLOG_INFO("Shutdown signal received");

  // Teardown (reverse order).
  for (auto& pipeline : pipelines) {
    pipeline->Stop();
  }

  // Notify all waiting HTTP handlers.
  for (auto& ring : rings) {
    ring->NotifyAll();
  }

  server.Stop();

  SPDLOG_INFO("Video monitor stopped");
  return 0;
}
