#include <sys/wait.h>

#include <atomic>
#include <chrono>
#include <cmath>  // for sqrt
#include <cstdint>
#include <fstream>
#include <iostream>
#include <random>  // Add this include at the top with other includes
#include <string>
#include <string_view>
#include <thread>

#include "benchmark/benchmark.h"

#include "dex/infrastructure/shared_memory/shared_memory_streaming.h"

namespace {

// Use the same FrameBuffer definition as in shared_mem_ipc.cc
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

// Maximum number of measurements we'll store
constexpr size_t kMaxMeasurements = 10000;

// Add near the top with other constants
constexpr int kNumRepetitions = 100;
constexpr const char* kCsvFileName = "benchmark_results.csv";

// Struct to store benchmark metrics in shared memory
struct MetricsBuffer {
  std::array<int64_t, kMaxMeasurements> latencies{};
  std::array<int64_t, kMaxMeasurements> frame_id_differences{};
  std::array<uint64_t, kMaxMeasurements> producer_timestamps{};
  std::array<uint64_t, kMaxMeasurements> consumer_timestamps{};
  std::atomic<size_t> processed_frames{0};
};

// Add this struct after BenchmarkState definition
struct MeasurementRow {
  uint64_t frame_interval_us;
  uint64_t num_frames;
  uint64_t warmup_frames;
  uint64_t iteration;
  uint64_t repetition;
  uint64_t frame_number;
  double latency_ns;
  double producer_interval_ns;
  double consumer_interval_ns;
  int64_t frame_id_diff;  // if everything works correctly, this should be >= 0, but that can't be assumed in tests.
};

using LockFreeSharedFrameBuffer =
    dex::shared_memory::SharedMemory<FrameBuffer, 2, dex::shared_memory::LockFreeSharedMemoryBuffer>;
using LockFreeSharedMetricsBuffer =
    dex::shared_memory::SharedMemory<MetricsBuffer, 1, dex::shared_memory::LockFreeSharedMemoryBuffer>;

// Helper function to run producer process
void RunProducer(std::string_view shared_memory_name,  // NOLINT(bugprone-easily-swappable-parameters)
                 std::string_view metrics_shared_memory_name,
                 const uint64_t num_frames,  // NOLINT(bugprone-easily-swappable-parameters)
                 const uint64_t frame_interval_us) {
  auto metrics_shared_memory = LockFreeSharedMetricsBuffer::Open(metrics_shared_memory_name);
  if (!metrics_shared_memory.IsValid()) {
    std::cerr << "Failed to open shared memory for metrics\n";
    _exit(1);
  }
  auto* metrics = metrics_shared_memory.Get()->buffers.data();
  dex::shared_memory::Producer<FrameBuffer> producer{shared_memory_name};

  producer.Run([&](FrameBuffer& buffer, const uint counter) {
    if (metrics->processed_frames.load(std::memory_order_acquire) >= static_cast<size_t>(num_frames)) {
      _exit(0);
    }
    // Small sleep to simulate work
    std::this_thread::sleep_for(std::chrono::microseconds(frame_interval_us));

    // Get current timestamp
    const auto now = std::chrono::steady_clock::now();
    const auto timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();

    // Store timestamp and frame_id in buffer
    buffer.timestamp = static_cast<uint64_t>(timestamp);
    buffer.frame_id = counter;

    // Store producer timestamp for interval calculation
    if (counter < kMaxMeasurements) {
      gsl::at(metrics->producer_timestamps, static_cast<gsl::index>(counter)) = timestamp;
    }
  });
}

// Helper function to run consumer process
void RunConsumer(std::string_view shared_memory_name,  // NOLINT(bugprone-easily-swappable-parameters)
                 std::string_view metrics_shared_memory_name, const uint64_t num_frames) {
  auto metrics_shared_memory = LockFreeSharedMetricsBuffer::Open(metrics_shared_memory_name);
  if (!metrics_shared_memory.IsValid()) {
    std::cerr << "Failed to open shared memory for metrics\n";
    _exit(1);
  }

  auto* metrics = metrics_shared_memory.Get()->buffers.data();
  dex::shared_memory::Consumer<FrameBuffer> consumer{shared_memory_name};
  uint64_t last_frame_id = 0;
  bool first_frame = true;
  size_t frame_count = 0;

  [[maybe_unused]] const auto consumer_result = consumer.Run([&](const FrameBuffer& buffer) {
    // Get current timestamp for latency calculation
    const auto now = std::chrono::steady_clock::now();
    const auto timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();

    if (frame_count < kMaxMeasurements) {
      // Store timestamps and calculate latency
      const auto idx = static_cast<gsl::index>(frame_count);
      gsl::at(metrics->consumer_timestamps, idx) = timestamp;
      gsl::at(metrics->latencies, idx) = static_cast<int64_t>(timestamp - buffer.timestamp);

      // Calculate frame_id differences
      if (!first_frame) {
        gsl::at(metrics->frame_id_differences, idx) = static_cast<int64_t>(buffer.frame_id - last_frame_id);
      }
    }

    last_frame_id = buffer.frame_id;
    first_frame = false;
    frame_count++;

    // Use fetch_add with release semantics - simpler and more efficient
    const size_t current_count = metrics->processed_frames.fetch_add(1, std::memory_order_release);

    // Check if we've reached num_frames (note: we check current_count since fetch_add returns the old value)
    if (current_count + 1 >= static_cast<size_t>(num_frames)) {
      _exit(0);
    }
  });
}

// Helper function to cleanup shared memory segments
void CleanupSharedMemory(std::string_view shared_memory_name,  // NOLINT(bugprone-easily-swappable-parameters)
                         std::string_view metrics_shared_memory_name) {
  [[maybe_unused]] const bool result = LockFreeSharedFrameBuffer::Destroy(shared_memory_name);
  [[maybe_unused]] const bool metrics_result = LockFreeSharedMetricsBuffer::Destroy(metrics_shared_memory_name);
}

// Helper function to create and initialize shared memory segments
bool CreateSharedMemory(std::string_view shared_memory_name, std::string_view metrics_shared_memory_name) {
  // Create shared memory for data
  {
    auto shared_memory =
        LockFreeSharedFrameBuffer::Create(shared_memory_name, dex::shared_memory::InitializeBuffer<FrameBuffer>);
    if (!shared_memory.IsValid()) {
      return false;
    }
  }

  // Create shared memory for metrics
  {
    auto metrics_shared_memory = LockFreeSharedMetricsBuffer::Create(metrics_shared_memory_name);
    if (!metrics_shared_memory.IsValid()) {
      CleanupSharedMemory(shared_memory_name, metrics_shared_memory_name);
      return false;
    }
  }

  return true;
}

// Modify BenchmarkSharedMemoryLatency to track iteration and repetition
void BenchmarkSharedMemoryLatency(benchmark::State& state) {
  // Static counter for repetitions that persists between benchmark calls
  static uint64_t repetition_count = 0;

  const uint64_t frame_interval_us = state.range(0);
  const uint64_t num_frames = state.range(1);
  const auto warmup_frames = static_cast<size_t>(state.range(2));

  // Create a random generator for unique shared memory names for each iteration
  std::random_device random_device;
  std::mt19937 random_engine(random_device());
  constexpr uint kMinId = 10000;
  constexpr uint kMaxId = 99999;
  std::uniform_int_distribution<uint> id_distribution(kMinId, kMaxId);

  // Counter for iterations within this repetition
  uint64_t iteration_count = 0;

  for (auto _ : state) {  // NOLINT(readability-identifier-length)
    // Generate random names for each iteration
    const std::string shared_memory_name = "benchmark_shared_memory_" + std::to_string(id_distribution(random_engine));
    const std::string metrics_shared_memory_name =
        "benchmark_metrics_shared_memory_" + std::to_string(id_distribution(random_engine));

    // Create and initialize shared memory segments with the random names
    if (!CreateSharedMemory(shared_memory_name, metrics_shared_memory_name)) {
      state.SkipWithError("Failed to create shared memory segments");
      break;
    }

    // Fork consumer process
    const pid_t consumer_pid = fork();
    if (consumer_pid == -1) {
      CleanupSharedMemory(shared_memory_name, metrics_shared_memory_name);
      state.SkipWithError("Failed to fork consumer process");
      break;
    }
    if (consumer_pid == 0) {  // Consumer child process
      RunConsumer(shared_memory_name, metrics_shared_memory_name, num_frames);
      _exit(0);
    }

    // Fork producer process
    const pid_t producer_pid = fork();
    if (producer_pid == -1) {
      kill(consumer_pid, SIGTERM);
      CleanupSharedMemory(shared_memory_name, metrics_shared_memory_name);
      state.SkipWithError("Failed to fork producer process");
      break;
    }
    if (producer_pid == 0) {  // Producer child process
      RunProducer(shared_memory_name, metrics_shared_memory_name, num_frames, frame_interval_us);
      _exit(0);
    }

    // Wait for both processes to complete
    int consumer_status = 0;
    int producer_status = 0;
    waitpid(consumer_pid, &consumer_status, 0);
    waitpid(producer_pid, &producer_status, 0);

    // Open metrics shared memory to read results
    auto metrics_shared_memory = LockFreeSharedMetricsBuffer::Open(metrics_shared_memory_name);
    if (!metrics_shared_memory.IsValid()) {
      CleanupSharedMemory(shared_memory_name, metrics_shared_memory_name);
      state.SkipWithError("Failed to open metrics shared memory for reading");
      break;
    }

    auto* metrics = metrics_shared_memory.Get()->buffers.data();
    const auto measurement_frames = std::min(static_cast<size_t>(num_frames), kMaxMeasurements);

    if (measurement_frames > warmup_frames + 1) {  // Need at least 2 frames after warmup for intervals
      std::ofstream csv_file(kCsvFileName, std::ios::app);
      for (size_t i = warmup_frames + 1; i < measurement_frames; ++i) {
        const MeasurementRow row = {
            .frame_interval_us = frame_interval_us,
            .num_frames = num_frames,
            .warmup_frames = warmup_frames,
            .iteration = iteration_count,
            .repetition = repetition_count,
            .frame_number = i - warmup_frames - 1,
            .latency_ns = static_cast<double>(gsl::at(metrics->latencies, static_cast<gsl::index>(i))),
            .producer_interval_ns =
                static_cast<double>(gsl::at(metrics->producer_timestamps, static_cast<gsl::index>(i)) -
                                    gsl::at(metrics->producer_timestamps, static_cast<gsl::index>(i - 1))),
            .consumer_interval_ns =
                static_cast<double>(gsl::at(metrics->consumer_timestamps, static_cast<gsl::index>(i)) -
                                    gsl::at(metrics->consumer_timestamps, static_cast<gsl::index>(i - 1))),
            .frame_id_diff = gsl::at(metrics->frame_id_differences, static_cast<gsl::index>(i))};

        csv_file << row.frame_interval_us << "," << row.num_frames << "," << row.warmup_frames << "," << row.iteration
                 << "," << row.repetition << "," << row.frame_number << "," << row.latency_ns << ","
                 << row.producer_interval_ns << "," << row.consumer_interval_ns << "," << row.frame_id_diff << "\n";
      }
    }

    iteration_count++;

    // Clean up shared memory segments before next iteration
    CleanupSharedMemory(shared_memory_name, metrics_shared_memory_name);
  }

  // Increment repetition count after all iterations are done
  repetition_count++;

  // Reset repetition count if we've completed all repetitions
  if (repetition_count >= kNumRepetitions) {
    repetition_count = 0;
  }
}

// Register the benchmark
BENCHMARK(BenchmarkSharedMemoryLatency)
    ->Args({1, 1000, 100})          // NOLINT(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
    ->Args({1000, 100, 10})         // NOLINT(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
    ->Args({10000, 100, 10})        // NOLINT(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
    ->Iterations(1)                 // Run multiple times for each argument
    ->Repetitions(kNumRepetitions)  // Use the same constant here
    ->Unit(benchmark::kMicrosecond)
    ->ComputeStatistics("min",
                        [](const std::vector<double>& values) -> double { return *(std::ranges::min_element(values)); })
    ->ComputeStatistics("max",
                        [](const std::vector<double>& values) -> double { return *(std::ranges::max_element(values)); })

    ->DisplayAggregatesOnly(true)
    ->UseRealTime();

}  // namespace

// Modify the main function
int main(int argc, char** argv) {
  // Truncate the file and write headers
  {
    std::ofstream init_file(kCsvFileName);
    init_file << "frame_interval_us,num_frames,warmup_frames,iteration,repetition,frame_number,"
              << "latency_ns,producer_interval_ns,consumer_interval_ns,frame_id_diff\n";
  }

  benchmark::Initialize(&argc, argv);

  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();

  return 0;
}

// BENCHMARK_MAIN();
