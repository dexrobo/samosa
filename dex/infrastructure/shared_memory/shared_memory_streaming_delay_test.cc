#include <array>
#include <random>
#include <ranges>
#include <variant>

#include "absl/strings/match.h"
#include "fmt/core.h"
#include "spdlog/sinks/ostream_sink.h"

#include "dex/infrastructure/shared_memory/shared_memory_streaming_test_private.h"

using dex::shared_memory::test::ArrayBuffer;
using dex::shared_memory::test::CoverageSafeExit;
using dex::shared_memory::test::LockFreeSharedArrayBuffer;
using dex::shared_memory::test::SetReadIndex;
using dex::shared_memory::test::SetWriteIndex;
using dex::shared_memory::test::SharedMemStreamingTest;
using dex::shared_memory::test::WaitForWriteIndex;

namespace {

// Define the delay configuration structures
struct TestDelays {
  int producer_init_delay_us;
  int producer_frame_delay_us;
  int consumer_pre_init_delay_us;
  int consumer_init_delay_us;
  int consumer_frame_delay_us;
  bool skip_producer_wait;  // New parameter
};

struct RandomDelaySpec {
  int min_delay_us;
  int max_delay_us;
  int count;                // Number of test iterations to generate with this spec
  bool skip_producer_wait;  // Add skip_producer_wait to RandomDelaySpec
};

struct DelayConfig {
  bool is_deterministic = false;
  std::variant<TestDelays, RandomDelaySpec> delays;
};

// Test parameter structure
struct DelayTestParam {
  int iteration = 0;
  DelayConfig config;

  // Helper to generate string representation for test names
  [[nodiscard]] std::string ToString() const {
    if (config.is_deterministic) {
      return fmt::format("Deterministic_Iteration{}", iteration);
    }
    const auto& spec = std::get<RandomDelaySpec>(config.delays);
    return fmt::format("Random_{}to{}us_{}_Iteration{}", spec.min_delay_us, spec.max_delay_us,
                       spec.skip_producer_wait ? "NoWait" : "Wait", iteration);
  }
};

class SharedMemStreamingDelayTest : public SharedMemStreamingTest, public testing::WithParamInterface<DelayTestParam> {
 protected:
  void SetUp() override {  // NOLINT(readability-convert-member-functions-to-static)
    // For parameterized tests, we need to include both test name and parameter name
    std::string test_name = std::string(testing::UnitTest::GetInstance()->current_test_info()->test_suite_name()) +
                            "_" + testing::UnitTest::GetInstance()->current_test_info()->name();
    // Replace forward slashes with underscores
    std::ranges::replace(test_name, '/', '_');
    shared_memory_name_ = "test_shared_memory_" + test_name;

    // Reset StreamingControl state before each test
    dex::shared_memory::StreamingControl::Instance().Reset();
  }

  // Helper to get delays for current iteration
  static TestDelays GetDelaysForIteration(const DelayTestParam& param) {
    if (param.config.is_deterministic) {
      return std::get<TestDelays>(param.config.delays);
    }

    const auto& spec = std::get<RandomDelaySpec>(param.config.delays);
    std::mt19937 random_engine(42 + param.iteration);
    std::uniform_int_distribution<int> delay_distribution(spec.min_delay_us, spec.max_delay_us);

    return {
        delay_distribution(random_engine),  // producer_init
        delay_distribution(random_engine),  // producer_frame
        delay_distribution(random_engine),  // consumer_pre_init
        delay_distribution(random_engine),  // consumer_init
        delay_distribution(random_engine),  // consumer_frame
        spec.skip_producer_wait             // Use skip_producer_wait from spec
    };
  }
};

TEST_P(SharedMemStreamingDelayTest, ProducerConsumerInitialization) {
  const auto delays = GetDelaysForIteration(GetParam());
  SCOPED_TRACE(
      fmt::format("Testing iteration {} with delays: producer_init={}, producer_frame={}, consumer_init={}, "
                  "consumer_frame={}, skip_producer_wait={}",
                  GetParam().iteration, delays.producer_init_delay_us, delays.producer_frame_delay_us,
                  delays.consumer_init_delay_us, delays.consumer_frame_delay_us, delays.skip_producer_wait));

  std::array<int, 2> pipe_fd = {};
  ASSERT_EQ(pipe(pipe_fd.data()), 0);

  RunProducerConsumer<ArrayBuffer>(
      // Producer lambda
      [pipe_fd, delays](std::string_view shared_memory_name) {
        close(pipe_fd[0]);  // Close read end

        std::stringstream log_stream;
        auto stream_sink = std::make_shared<spdlog::sinks::ostream_sink_st>(log_stream);
        auto logger = std::make_shared<spdlog::logger>("test_logger", stream_sink);
        spdlog::set_default_logger(logger);
        spdlog::set_level(spdlog::level::debug);

        auto shared_memory = LockFreeSharedArrayBuffer::Open(shared_memory_name);
        // Set publisher's initial state to:
        // 1. last published buffer is BufferB
        // 2. publish next frame into BufferA.
        SetWriteIndex(shared_memory, dex::shared_memory::detail::BufferState::BufferB);
        // Producer will think that consumer has requested BufferA and so it will publish to BufferA.
        SetReadIndex(shared_memory, dex::shared_memory::detail::BufferState::BufferA);
        ASSERT_TRUE(shared_memory.IsValid());

        // Start producer
        dex::shared_memory::StreamingControl::Instance().ReconfigureAndReset({.handle_signals = true});
        dex::shared_memory::Producer<ArrayBuffer> producer{shared_memory_name};

        // Use specific delay before starting producer
        std::this_thread::sleep_for(std::chrono::microseconds(delays.producer_init_delay_us));
        producer.Run([delays](ArrayBuffer& buffer, uint /*counter*/, int buffer_id) {
          // Use specific delay for publishing a frame
          std::this_thread::sleep_for(std::chrono::microseconds(delays.producer_frame_delay_us));
          std::string message = "test message";
          std::memcpy(buffer.data(), message.data(), message.size() + 1);
        });

        // Flush the log stream AFTER exiting the producer
        spdlog::default_logger()->flush();
        const std::string log_content = log_stream.str();
        const ssize_t bytes_written = write(pipe_fd[1], log_content.c_str(), log_content.size());
        if (bytes_written == -1) {
          CoverageSafeExit(1);
        }
        CoverageSafeExit(0);
      },

      // Consumer lambda
      [delays](std::string_view shared_memory_name) {
        const float timeout_seconds = 1.0f;
        const timespec timeout = {
            .tv_sec = static_cast<time_t>(std::floor(timeout_seconds)),
            .tv_nsec = static_cast<int64_t>((timeout_seconds - std::floor(timeout_seconds)) * 1e9)};

        auto shared_memory = LockFreeSharedArrayBuffer::Open(shared_memory_name);

        // Use specific delay before waiting for producer
        std::this_thread::sleep_for(std::chrono::microseconds(delays.consumer_pre_init_delay_us));

        // Only wait for producer if skip_producer_wait is false
        if (!delays.skip_producer_wait) {
          ASSERT_TRUE(WaitForWriteIndex(shared_memory, dex::shared_memory::detail::BufferState::BufferA));
          ASSERT_EQ(std::strcmp(shared_memory.Get()
                                    ->buffers[dex::shared_memory::detail::ToBufferIndex(
                                        dex::shared_memory::detail::BufferState::BufferA)]
                                    .data(),
                                "test message"),
                    0);
        }

        // Random delay before starting consumer
        std::this_thread::sleep_for(std::chrono::microseconds(delays.consumer_init_delay_us));
        dex::shared_memory::Consumer<ArrayBuffer> consumer{shared_memory_name};
        [[maybe_unused]] const auto consumer_result = consumer.Run(
            [delays](const ArrayBuffer& buffer) {
              // Random delay in consuming a frame
              std::this_thread::sleep_for(std::chrono::microseconds(delays.consumer_frame_delay_us));
              const std::string message(buffer.data());
              if (message == "test message") CoverageSafeExit(0);
            },
            &timeout);
        CoverageSafeExit(1);  // Timeout or error
      });

  // Parent process reads the log content
  close(pipe_fd[1]);  // Close write end
  std::array<char, 4096> buffer = {};
  std::string log_content;
  ssize_t bytes_read = 0;
  while ((bytes_read = read(pipe_fd[0], buffer.data(), buffer.size())) > 0) {
    log_content.append(buffer.data(), bytes_read);
  }
  close(pipe_fd[0]);

  if (!delays.skip_producer_wait) {
    EXPECT_TRUE(absl::StrContains(log_content, "publisher reset")) << fmt::format(
        "Test iteration {} failed: Expected 'publisher reset' message not found in logs", GetParam().iteration);
  }
}

// Generate test parameters
std::vector<DelayTestParam> GenerateTestParams() {
  std::vector<DelayTestParam> params;

  // Add deterministic test cases - each entry represents all delays for one test iteration
  const std::vector deterministic_delays = {
      // {producer_init, producer_frame, consumer_pre_init, consumer_init, consumer_frame, skip_producer_wait}
      std::tuple(0, 1, 0, 1000, 1, false),  // delay consumer Run() till after producer is running
      std::tuple(1000, 1, 0, 0, 1,
                 false),  // delay producer Run() till after check for producer's published frame starts
      std::tuple(0, 1, 1000, 0, 1, false),  // delay checking for BufferA till after producer is running
      std::tuple(1000, 1, 0, 0, 1, true),   // consumer starts first, skips waiting for producer
      std::tuple(0, 1, 0, 0, 1, true),      // both start immediately, consumer doesn't wait
  };

  for (auto i : std::views::iota(size_t{0}, deterministic_delays.size())) {
    params.push_back({.iteration = static_cast<int>(i),
                      .config = {.is_deterministic = true,
                                 .delays = std::apply([](auto... args) { return TestDelays{{args}...}; },
                                                      deterministic_delays[i])}});
  }

  // Add random test cases
  std::vector<RandomDelaySpec> random_specs;

  // Check if running in reduced test mode (CI environment)
  const char* reduced_test_env = std::getenv("REDUCED_TEST_SET");
  const bool use_reduced_tests = reduced_test_env != nullptr;

  if (use_reduced_tests) {
    // Reduced test set for CI environments
    random_specs = {
        {0, 1000, 10, false},    // 10 iterations with delays between 0-1000us (with wait)
        {0, 1000, 10, true},     // 10 iterations with delays between 0-1000us (without wait)
        {1000, 2000, 5, false},  // 5 iterations with delays between 1000-2000us (with wait)
        {1000, 2000, 5, true}    // 5 iterations with delays between 1000-2000us (without wait)
    };
  } else {
    // Full test set for local development
    random_specs = {
        {0, 1000, 50, false},     // 50 iterations with delays between 0-1000us (with wait)
        {0, 1000, 50, true},      // 50 iterations with delays between 0-1000us (without wait)
        {1000, 2000, 50, false},  // 50 iterations with delays between 1000-2000us (with wait)
        {1000, 2000, 50, true},   // 50 iterations with delays between 1000-2000us (without wait)
        {2000, 10000, 5, false},  // 5 iterations with delays between 2000-10000us (with wait)
        {2000, 10000, 5, true}    // 5 iterations with delays between 2000-10000us (without wait)
    };
  }

  for (const auto& spec : random_specs) {
    for (auto i : std::views::iota(size_t{0}, static_cast<size_t>(spec.count))) {
      params.push_back({.iteration = static_cast<int>(i),
                        .config = {.is_deterministic = false,
                                   .delays = RandomDelaySpec{.min_delay_us = spec.min_delay_us,
                                                             .max_delay_us = spec.max_delay_us,
                                                             .count = spec.count,
                                                             .skip_producer_wait = spec.skip_producer_wait}}});
    }
  }

  return params;
}

INSTANTIATE_TEST_SUITE_P(VariableDelays, SharedMemStreamingDelayTest, testing::ValuesIn(GenerateTestParams()),
                         [](const testing::TestParamInfo<DelayTestParam>& info) { return info.param.ToString(); });

}  // namespace
