#include "dex/infrastructure/video_monitor/config.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "spdlog/spdlog.h"
#include "toml.hpp"

// NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)

namespace dex::video_monitor {
namespace {

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
MonitorConfig ParseToml(const toml::table& tbl) {
  MonitorConfig config;

  if (const auto* server = tbl["server"].as_table()) {
    if (auto val = (*server)["bind_address"].value<std::string>()) {
      config.server.bind_address = *val;
    }
    if (auto val = (*server)["port"].value<int64_t>()) {
      config.server.port = static_cast<uint16_t>(*val);
    }
    if (auto val = (*server)["fragment_ring_size"].value<int64_t>()) {
      config.server.fragment_ring_size = static_cast<uint32_t>(*val);
    }
  }

  if (const auto* topics = tbl["topics"].as_array()) {
    for (const auto& elem : *topics) {
      if (const auto* topic_tbl = elem.as_table()) {
        TopicConfig topic;
        if (auto val = (*topic_tbl)["shm_name"].value<std::string>()) {
          topic.shm_name = *val;
        }
        if (auto val = (*topic_tbl)["endpoint"].value<std::string>()) {
          topic.endpoint = *val;
        }
        if (auto val = (*topic_tbl)["target_fps"].value<int64_t>()) {
          topic.target_fps = static_cast<uint32_t>(*val);
        }
        if (auto val = (*topic_tbl)["bitrate_kbps"].value<int64_t>()) {
          topic.bitrate_kbps = static_cast<uint32_t>(*val);
        }
        if (auto val = (*topic_tbl)["keyframe_interval"].value<int64_t>()) {
          topic.keyframe_interval = static_cast<uint32_t>(*val);
        }
        if (auto val = (*topic_tbl)["max_width"].value<int64_t>()) {
          topic.max_width = static_cast<uint32_t>(*val);
        }
        if (auto val = (*topic_tbl)["max_height"].value<int64_t>()) {
          topic.max_height = static_cast<uint32_t>(*val);
        }

        // Default endpoint from shm_name: strip leading '/' and replace '/' with '_'.
        if (topic.endpoint.empty() && !topic.shm_name.empty()) {
          topic.endpoint = topic.shm_name;
          if (!topic.endpoint.empty() && topic.endpoint[0] == '/') {
            topic.endpoint.erase(0, 1);
          }
          for (auto& chr : topic.endpoint) {
            if (chr == '/') {
              chr = '_';
            }
          }
        }

        config.topics.push_back(std::move(topic));
      }
    }
  }

  return config;
}

// Derive an endpoint name from a shared memory name.
std::string EndpointFromShmName(const std::string& shm_name) {
  std::string endpoint = shm_name;
  if (!endpoint.empty() && endpoint[0] == '/') {
    endpoint.erase(0, 1);
  }
  for (auto& chr : endpoint) {
    if (chr == '/') {
      chr = '_';
    }
  }
  return endpoint;
}

}  // namespace

MonitorConfig LoadConfigFromString(const std::string& toml_content) {
  auto result = toml::parse(toml_content);
  return ParseToml(result);
}

namespace {

// Require that a flag has a following value argument.
void RequireValue(const std::string& flag, int idx, int argc) {
  if (idx + 1 >= argc) {
    throw std::runtime_error("Flag '" + flag + "' requires a value");
  }
}

}  // namespace

MonitorConfig LoadConfig(int argc, const char* const* argv) {
  MonitorConfig config;
  std::string config_path;
  std::vector<std::string> cli_topics;

  // First pass: validate all arguments and collect values.
  for (int idx = 1; idx < argc; ++idx) {
    const std::string arg = argv[idx];

    if (arg == "--config" || arg == "-c") {
      RequireValue(arg, idx, argc);
      config_path = argv[++idx];
    } else if (arg == "--port") {
      RequireValue(arg, idx, argc);
      config.server.port = static_cast<uint16_t>(std::stoi(argv[++idx]));
    } else if (arg == "--bind") {
      RequireValue(arg, idx, argc);
      config.server.bind_address = argv[++idx];
    } else if (arg == "--topic") {
      RequireValue(arg, idx, argc);
      cli_topics.emplace_back(argv[++idx]);
    } else if (arg == "--help" || arg == "-h") {
      throw std::runtime_error(
          "Usage: video_monitor [OPTIONS]\n"
          "\n"
          "Options:\n"
          "  --config, -c <path>   Path to TOML config file\n"
          "  --port <port>         Server port (default: 8080)\n"
          "  --bind <address>      Bind address (default: 0.0.0.0)\n"
          "  --topic <shm_name>    Add a topic (repeatable)\n"
          "  --help, -h            Show this help message");
    } else {
      throw std::runtime_error("Unknown argument: '" + arg +
                               "'\n"
                               "Run with --help for usage information.");
    }
  }

  // Load TOML config if provided.
  if (!config_path.empty()) {
    SPDLOG_INFO("Loading config from: {}", config_path);
    auto result = toml::parse_file(config_path);
    config = ParseToml(result);

    // Re-apply CLI overrides after TOML loading (CLI takes precedence).
    for (int idx = 1; idx < argc; ++idx) {
      const std::string arg = argv[idx];
      if (arg == "--port") {
        config.server.port = static_cast<uint16_t>(std::stoi(argv[++idx]));
      } else if (arg == "--bind") {
        config.server.bind_address = argv[++idx];
      } else {
        ++idx;  // Skip value for other flags (already validated above).
      }
    }
  }

  // Add CLI topics.
  for (const auto& shm_name : cli_topics) {
    TopicConfig topic;
    topic.shm_name = shm_name;
    topic.endpoint = EndpointFromShmName(shm_name);
    config.topics.push_back(std::move(topic));
  }

  return config;
}

}  // namespace dex::video_monitor

// NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
