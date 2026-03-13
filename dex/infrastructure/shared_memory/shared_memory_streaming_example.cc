// Core IPC mechanism headers
#include <chrono>
#include <iostream>
#include <thread>

#include "spdlog/sinks/stdout_sinks.h"
#include "spdlog/spdlog.h"

#include "dex/infrastructure/shared_memory/shared_memory_streaming.h"

namespace {

constexpr size_t kResolution = 1920ul * 1080ul;
constexpr size_t kPixelDataSize = kResolution * 3ul;
constexpr size_t kNameLength = 64;

struct FrameBuffer {
  std::array<std::byte, kPixelDataSize> pixel_data;
  std::array<uint16_t, kResolution> depth_data;
  std::array<char, kNameLength> camera_name;
  uint64_t timestamp;
  uint64_t frame_id;
};

using LockFreeSharedDoubleBuffer =
    dex::shared_memory::SharedMemory<FrameBuffer, 2, dex::shared_memory::LockFreeSharedMemoryBuffer>;

// entrypoint for producer
template <typename Producer>
void RunProducer(Producer&& producer) {
  // Pre-set camera name in both buffers before starting the loop
  // TODO(unknown): do we need a concept for buffer type?
  std::forward<Producer>(producer).Run([](FrameBuffer& buffer, const uint counter) {
    constexpr auto kProducerSleepMs = 5;
    std::this_thread::sleep_for(std::chrono::milliseconds(kProducerSleepMs));
    const auto timestamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    buffer.timestamp = timestamp;
    buffer.frame_id = counter;
  });
}

// entrypoint for consumer
template <typename Consumer>
void RunConsumer(Consumer&& consumer, float timeout_seconds = -1.0f) {
  const timespec timeout = {.tv_sec = static_cast<time_t>(std::floor(timeout_seconds)),
                            .tv_nsec = static_cast<int64_t>((timeout_seconds - std::floor(timeout_seconds)) * 1e9)};
  [[maybe_unused]] const auto consumer_result = std::forward<Consumer>(consumer).Run(
      [](const FrameBuffer& buffer, const uint /*counter*/, const int buffer_id) {
        const auto timestamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        std::cout << buffer.timestamp << "," << buffer.frame_id << "," << buffer_id << "," << timestamp << "\n";
        std::flush(std::cout);
      },
      timeout_seconds < 0 ? nullptr : &timeout);
}

}  // namespace

////////////////////////////////////////////////////////////////////////////////
/**
 * Command-line interface for multi-process IPC
 *
 * Supports four modes of operation:
 * - create: Initialize shared memory
 * - publisher: Run producer loop
 * - consumer: Run consumer loop
 * - destroy: Clean up shared memory
 */
////////////////////////////////////////////////////////////////////////////////
int main(const int argc, const char* argv[]) {  // NOLINT(bugprone-exception-escape)
  if (argc < 3) {
    std::cerr << "Usage: " << argv[0] << " \n"  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
              << "  create <shmName>\n"
              << "  publisher <shmName>\n"
              << "  consumer <shmName> [timeout_seconds]\n"
              << "  destroy <shmName>\n"
              << "Options:\n"
              << "  --debug    Enable debug logging\n";
    return 1;
  }

  // Check for debug flag and remove it from args
  bool debug = false;
  std::vector<std::string_view> args;
  for (int i = 1; i < argc; ++i) {
    if (std::string_view(argv[i]) == "--debug") {  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      debug = true;
    } else {
      args.emplace_back(argv[i]);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    }
  }

  if (args.size() < 2) {
    std::cerr << "Not enough arguments\n";
    return 1;
  }

  if (debug) {
    // Create a logger that outputs to stderr and set it as the default logger.
    auto stderr_logger = spdlog::stderr_logger_mt("console");
    spdlog::set_default_logger(stderr_logger);

    spdlog::set_level(spdlog::level::debug);
    spdlog::set_pattern("[%E.%f] %v");  // %E for epoch seconds, %f for fractional seconds
  }

  const std::string_view mode{args[0]};
  const std::string_view shared_memory_name{args[1]};

  if (mode == "create") {
    const LockFreeSharedDoubleBuffer shared_memory =
        LockFreeSharedDoubleBuffer::Create(shared_memory_name, dex::shared_memory::InitializeBuffer<FrameBuffer>);
    if (!shared_memory.IsValid()) {
      std::cerr << "Failed to create shared memory.\n";
      return 1;
    }
    std::cerr << "Created shared memory. Exiting.\n";
  } else if (mode == "publisher") {
    auto producer = dex::shared_memory::Producer<FrameBuffer>(shared_memory_name);
    const std::string_view camera_name = "camera-1";
    std::cerr << "Running publisher loop.\n";
    const LockFreeSharedDoubleBuffer shared_memory = LockFreeSharedDoubleBuffer::Open(shared_memory_name);
    for (auto& buffer : dex::shared_memory::detail::GetBuffers(shared_memory)) {
      const size_t copy_size = std::min(camera_name.size(), kNameLength - 1);
      std::memcpy(buffer.camera_name.data(), camera_name.data(), copy_size);
      gsl::at(buffer.camera_name, static_cast<gsl::index>(copy_size)) = '\0';  // Ensure null termination
    }

    RunProducer(producer);
    std::cerr << "Exiting publisher.\n";
  } else if (mode == "consumer") {
    float timeout_seconds = -1.0f;  // Default to no timeout
    if (args.size() > 2) {
      timeout_seconds = std::stof(std::string(args[2]));
    }
    std::cerr << "Running consumer loop.\n";
    auto consumer = dex::shared_memory::Consumer<FrameBuffer>(shared_memory_name);
    RunConsumer(consumer, timeout_seconds);
    std::cerr << "Exiting consumer.\n";
  } else if (mode == "destroy") {
    if (!dex::shared_memory::SharedMemory<FrameBuffer, 2, dex::shared_memory::LockFreeSharedMemoryBuffer>::Destroy(
            shared_memory_name)) {
      std::cerr << "Failed to unlink shared memory.\n";
      return 1;
    }
    std::cerr << "Shared memory unlinked (destroyed).\n";
  } else {
    std::cerr << "Unknown mode: " << mode << "\n";
    return 1;
  }
  return 0;
}
