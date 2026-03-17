#include <chrono>
#include <iostream>
#include <thread>

#include "gsl/gsl"
#include "spdlog/spdlog.h"

#include "dex/drivers/camera/base/types.h"
#include "dex/infrastructure/shared_memory/shared_memory_streaming.h"

int main(int argc, char* argv[]) {
  try {
    const gsl::span<char* const> args(argv, static_cast<size_t>(argc));

    if (argc < 2) {
      std::cerr << "Usage: " << args[0] << " <shared_memory_name>\n";
      return 1;
    }

    const std::string_view shm_name = args[1];

    SPDLOG_INFO("Starting camera consumer on '{}'", shm_name);

    auto consumer = dex::shared_memory::Consumer<dex::camera::CameraFrameBuffer>(shm_name);
    if (!consumer.IsValid()) {
      SPDLOG_ERROR("Failed to initialize shared memory consumer");
      return 1;
    }

    uint64_t last_frame_id = 0;

    (void)consumer.Run([&](const dex::camera::CameraFrameBuffer& buffer) {
      const auto now = std::chrono::steady_clock::now();
      const auto now_nanos =
          static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count());

      const auto latency_ms = static_cast<double>(now_nanos - buffer.timestamp_nanos) / 1e6;  // NOLINT

      if (buffer.frame_id != last_frame_id) {
        SPDLOG_INFO("Frame ID: {}, Latency: {:.3f} ms, Camera: {}", buffer.frame_id, latency_ms,
                    std::string(buffer.camera_name.data()));
        last_frame_id = buffer.frame_id;
      }
    });
  } catch (const std::exception& e) {
    SPDLOG_ERROR("Exception occurred: {}", e.what());
    return 1;
  } catch (...) {
    SPDLOG_ERROR("Unknown exception occurred.");
    return 1;
  }

  return 0;
}

