#include "dex/infrastructure/video_monitor/config.h"

#include <stdexcept>
#include <string>

#include "gtest/gtest.h"

namespace dex::video_monitor {
namespace {

TEST(ConfigTest, ParsesBasicToml) {
  const std::string toml = R"(
[server]
bind_address = "127.0.0.1"
port = 9090
fragment_ring_size = 60

[[topics]]
shm_name = "/front_camera"
endpoint = "front"
target_fps = 15
bitrate_kbps = 1000
keyframe_interval = 30

[[topics]]
shm_name = "/rear_camera"
target_fps = 30
)";

  auto config = LoadConfigFromString(toml);

  EXPECT_EQ(config.server.bind_address, "127.0.0.1");
  EXPECT_EQ(config.server.port, 9090);
  EXPECT_EQ(config.server.fragment_ring_size, 60u);

  ASSERT_EQ(config.topics.size(), 2u);

  EXPECT_EQ(config.topics[0].shm_name, "/front_camera");
  EXPECT_EQ(config.topics[0].endpoint, "front");
  EXPECT_EQ(config.topics[0].target_fps, 15u);
  EXPECT_EQ(config.topics[0].bitrate_kbps, 1000u);
  EXPECT_EQ(config.topics[0].keyframe_interval, 30u);

  EXPECT_EQ(config.topics[1].shm_name, "/rear_camera");
  EXPECT_EQ(config.topics[1].endpoint, "rear_camera");  // Auto-derived.
  EXPECT_EQ(config.topics[1].target_fps, 30u);
}

TEST(ConfigTest, DefaultsForMissingFields) {
  const std::string toml = R"(
[[topics]]
shm_name = "/cam0"
)";

  auto config = LoadConfigFromString(toml);

  EXPECT_EQ(config.server.bind_address, "0.0.0.0");
  EXPECT_EQ(config.server.port, 8080);

  ASSERT_EQ(config.topics.size(), 1u);
  EXPECT_EQ(config.topics[0].target_fps, 15u);
  EXPECT_EQ(config.topics[0].bitrate_kbps, 1500u);
  EXPECT_EQ(config.topics[0].keyframe_interval, 30u);
  EXPECT_EQ(config.topics[0].max_width, 1280u);
  EXPECT_EQ(config.topics[0].max_height, 720u);
  EXPECT_EQ(config.topics[0].endpoint, "cam0");
}

TEST(ConfigTest, EmptyToml) {
  const std::string toml;
  auto config = LoadConfigFromString(toml);
  EXPECT_TRUE(config.topics.empty());
}

TEST(ConfigTest, CLITopicFlags) {
  const char* argv[] = {"video_monitor", "--topic", "/cam0", "--topic", "/cam1", "--port", "9999"};
  int argc = 7;

  auto config = LoadConfig(argc, argv);

  EXPECT_EQ(config.server.port, 9999);
  ASSERT_EQ(config.topics.size(), 2u);
  EXPECT_EQ(config.topics[0].shm_name, "/cam0");
  EXPECT_EQ(config.topics[0].endpoint, "cam0");
  EXPECT_EQ(config.topics[1].shm_name, "/cam1");
  EXPECT_EQ(config.topics[1].endpoint, "cam1");
}

TEST(ConfigTest, CLIOverridesTomlPort) {
  // Simulate: --config is not actually loaded here (no file), but we test the override logic.
  const char* argv[] = {"video_monitor", "--port", "7777", "--bind", "192.168.1.1"};
  int argc = 5;

  auto config = LoadConfig(argc, argv);

  EXPECT_EQ(config.server.port, 7777);
  EXPECT_EQ(config.server.bind_address, "192.168.1.1");
}

TEST(ConfigTest, EndpointDerivedFromNestedShmName) {
  const std::string toml = R"(
[[topics]]
shm_name = "/cameras/front/color"
)";

  auto config = LoadConfigFromString(toml);
  ASSERT_EQ(config.topics.size(), 1u);
  EXPECT_EQ(config.topics[0].endpoint, "cameras_front_color");
}

TEST(ConfigTest, RejectsUnknownArgument) {
  const char* argv[] = {"video_monitor", "-port", "9876"};
  int argc = 3;
  EXPECT_THROW(LoadConfig(argc, argv), std::runtime_error);
}

TEST(ConfigTest, RejectsUnknownLongFlag) {
  const char* argv[] = {"video_monitor", "--unknown-flag"};
  int argc = 2;
  EXPECT_THROW(LoadConfig(argc, argv), std::runtime_error);
}

TEST(ConfigTest, RejectsMissingValue) {
  const char* argv[] = {"video_monitor", "--port"};
  int argc = 2;
  EXPECT_THROW(LoadConfig(argc, argv), std::runtime_error);
}

TEST(ConfigTest, HelpFlagThrows) {
  const char* argv[] = {"video_monitor", "--help"};
  int argc = 2;
  EXPECT_THROW(LoadConfig(argc, argv), std::runtime_error);
}

}  // namespace
}  // namespace dex::video_monitor
