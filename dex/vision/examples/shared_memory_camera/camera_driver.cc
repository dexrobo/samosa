#include <chrono>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#include "gsl/gsl"
#include "spdlog/spdlog.h"

#include "dex/drivers/camera/base/types.h"
#include "dex/infrastructure/shared_memory/shared_memory_streaming.h"

namespace {

void PopulateMockFrame(dex::camera::CameraFrameBuffer& buffer, uint64_t frame_id, std::string_view camera_name) {
  buffer.frame_id = frame_id;
  const auto now = std::chrono::steady_clock::now();
  buffer.timestamp_nanos =
      static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count());

  dex::camera::StringToArray(std::string(camera_name), buffer.camera_name);
  dex::camera::StringToArray("SN-123456789", buffer.serial_number);

  buffer.color_width = dex::camera::kMaxWidth;
  buffer.color_height = dex::camera::kMaxHeight;
  buffer.color_format = {'R', 'G', 'B', '2', '4', '\0', '\0', '\0'};
  buffer.color_image_size = buffer.color_width * buffer.color_height * 3;

  // Fill with a dummy pattern (e.g., alternating bytes)
  const auto pattern = static_cast<uint8_t>(frame_id % 256);  // NOLINT(readability-magic-numbers)
  std::memset(buffer.color_image_bytes.data(), pattern, buffer.color_image_size);
}

}  // namespace

constexpr float kDefaultFps = 30.0f;

int main(int argc, char* argv[]) {
  try {
    const gsl::span<char* const> args(argv, static_cast<size_t>(argc));

    if (argc < 2) {
      std::cerr << "Usage: " << args[0] << " <shared_memory_name> [target_fps]\n";
      return 1;
    }

    const std::string_view shm_name = args[1];
    float target_fps = kDefaultFps;
    if (argc > 2) {
      target_fps = std::stof(std::string(args[2]));
    }

    const auto frame_duration = std::chrono::duration<double>(1.0f / target_fps);

    SPDLOG_INFO("Starting mock camera driver on '{}' at {} FPS", shm_name, target_fps);

    using ShmBuffer = dex::shared_memory::SharedMemory<dex::camera::CameraFrameBuffer, 2,
                                                       dex::shared_memory::LockFreeSharedMemoryBuffer>;

    // Create the shared memory segment (or open existing)
    // NOTE: We do not call Destroy() on exit to allow persistence.
    auto shared_memory =
        ShmBuffer::Create(shm_name, dex::shared_memory::InitializeBuffer<dex::camera::CameraFrameBuffer>);
    if (!shared_memory.IsValid()) {
      SPDLOG_ERROR("Failed to initialize shared memory segment '{}'", shm_name);
      return 1;
    }

    auto producer = dex::shared_memory::Producer<dex::camera::CameraFrameBuffer>(shm_name);
    if (!producer.IsValid()) {
      SPDLOG_ERROR("Failed to initialize shared memory producer");
      return 1;
    }

    uint64_t frame_count = 0;
    const std::string camera_name = "Mock-Camera-0";

    producer.Run([&](dex::camera::CameraFrameBuffer& buffer, uint32_t /*counter*/) {
      const auto start = std::chrono::steady_clock::now();

      PopulateMockFrame(buffer, frame_count, camera_name);
      frame_count++;

      const auto end = std::chrono::steady_clock::now();
      const auto elapsed = end - start;

      if (elapsed < frame_duration) {
        std::this_thread::sleep_for(frame_duration - elapsed);
      }

      if (frame_count % static_cast<uint64_t>(target_fps) == 0) {
        SPDLOG_INFO("Published {} frames", frame_count);
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

