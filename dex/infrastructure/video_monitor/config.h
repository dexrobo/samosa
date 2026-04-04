#ifndef DEX_INFRASTRUCTURE_VIDEO_MONITOR_CONFIG_H
#define DEX_INFRASTRUCTURE_VIDEO_MONITOR_CONFIG_H

#include <cstdint>
#include <string>
#include <vector>

namespace dex::video_monitor {

struct TopicConfig {
  std::string shm_name;  // Shared memory segment name (e.g., "/front_camera").
  std::string endpoint;  // HTTP path suffix (e.g., "front_camera").
  uint32_t target_fps{30};
  uint32_t bitrate_kbps{2000};
  uint32_t keyframe_interval{60};
};

struct ServerConfig {
  std::string bind_address{"0.0.0.0"};
  uint16_t port{8080};
  uint32_t fragment_ring_size{120};
};

struct MonitorConfig {
  ServerConfig server;
  std::vector<TopicConfig> topics;
};

/// Load configuration from a TOML file with CLI overrides.
///
/// CLI flags:
///   --config <path>       Path to TOML config file.
///   --port <port>         Override server port.
///   --bind <address>      Override bind address.
///   --topic <shm_name>    Add a topic with default settings (can be repeated).
///
/// If no --config is provided, topics must be specified via --topic flags.
MonitorConfig LoadConfig(int argc, const char* const* argv);

/// Load configuration from a TOML string (for testing).
MonitorConfig LoadConfigFromString(const std::string& toml_content);

}  // namespace dex::video_monitor

#endif  // DEX_INFRASTRUCTURE_VIDEO_MONITOR_CONFIG_H
