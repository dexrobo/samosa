# Samosa: Resilient Lock-Free Snapshot Streaming

Samosa is a high-performance C++20 library designed for ultra-low latency data streaming between processes. Unlike traditional message queues, Samosa implements **snapshot semantics**: consumers always receive the latest available data, making it ideal for high-frequency sensor data, telemetry, and video frames where throughput and "current state" are prioritized over historical completeness.

## Key Features

- **Non-Blocking Publisher**: Producers never block on consumers. High-frequency updates are published to shared memory with minimal overhead.
- **Latest-Only Consumption**: Consumers always fetch the most recent snapshot. If multiple updates occurred since the last read, intermediate ones are skipped to prevent "lag" buildup.
- **Independent Lifecycle Resilience**: Producers and consumers can restart at any time. They automatically reconnect to the shared memory segment and resume data transfer reliably.
- **Lock-Free Internals**: Uses atomic operations and Linux futexes for synchronization, avoiding heavy kernel locks.
- **Double-Buffered**: Implements internal double-buffering to ensure the consumer never reads a partially written frame.

## Requirements

- **Operating System**: Linux (Kernel 2.6.22+ required for `futex` support).
- **Compiler**: GCC 12+ or Clang 14+ (C++20 support required).
- **Build System**: [Bazel](https://bazel.build/).

## Usage

### 1. Define your Data Structure
The data structure must be a "Plain Old Data" (POD) type suitable for shared memory (no pointers to heap memory).

```cpp
#include <array>
#include <cstdint>

struct Telemetry {
  uint64_t timestamp_ns;
  std::array<float, 6> pose;
  std::array<uint8_t, 1024> payload;
};
```

### 2. Initialize the Shared Memory Segment
One process (usually a manager or the first producer) creates the segment.

```cpp
#include "dex/infrastructure/shared_memory/shared_memory_streaming.h"

using dex::shared_memory::SharedMemory;
using dex::shared_memory::LockFreeSharedMemoryBuffer;
using dex::shared_memory::InitializeBuffer;

const std::string shm_name = "/telemetry_stream";

// Create the segment
auto shm = SharedMemory<Telemetry, 2, LockFreeSharedMemoryBuffer>::Create(
    shm_name, InitializeBuffer<Telemetry>);
```

### 3. Publish Data
The `Producer::Run` method enters a continuous loop, invoking your callback whenever a buffer is ready for the next frame.

```cpp
#include "dex/infrastructure/shared_memory/shared_memory_streaming.h"

// Producer automatically opens the existing segment by name
dex::shared_memory::Producer<Telemetry> producer{shm_name};

// Run takes a callback with several supported signatures:
// - [](Telemetry& buffer)
// - [](Telemetry& buffer, uint counter)
// - [](Telemetry& buffer, uint counter, int buffer_id)
producer.Run([](Telemetry& buffer, uint counter) {
    buffer.timestamp_ns = GetTimeNs();
    // Fill pose and payload...
    std::cout << "Publishing frame: " << counter << std::endl;
});
```

### 4. Consume Data
The `Consumer::Run` method waits for new snapshots and executes your callback. It automatically handles frame skipping if the producer is faster than the consumer.

```cpp
#include "dex/infrastructure/shared_memory/shared_memory_streaming.h"

dex::shared_memory::Consumer<Telemetry> consumer{shm_name};

// Run blocks until interrupted or a timeout occurs.
// Supported signatures:
// - [](const Telemetry& buffer)
// - [](const Telemetry& buffer, uint counter)
// - [](const Telemetry& buffer, uint counter, int buffer_id)
consumer.Run([](const Telemetry& buffer, uint counter) {
    std::cout << "Received snapshot " << counter 
              << " at: " << buffer.timestamp_ns << std::endl;
});
```

## Performance & Benchmarking

Samosa includes a specialized benchmarking tool to measure end-to-end latency and frame-skip statistics under various loads.

```bash
# Run the benchmark
bazel run //dex/infrastructure/shared_memory:shared_memory_streaming_benchmark

# Post-process results to generate statistics
bazel run //dex/infrastructure/shared_memory:run_benchmark_with_stats
```

## Contributing

We welcome contributions! To ensure high standards of correctness and performance, please follow these steps:

### Development Environment
The easiest way to contribute is using the provided **Dev Container**. It contains all necessary dependencies (Bazel, GCC 12, Python, Node.js) and a pre-configured Linux environment.

### Verification
Before submitting a Pull Request, you **must** run the comprehensive verification suite.

```bash
# Run all formatters, linters, and tests
./tools/check.sh all
```

### PR Guidelines
1. **Tests**: Every fix or feature requires a corresponding test case in `dex/infrastructure/shared_memory/`.
2. **Sanitizers**: Ensure your changes pass under both `asan-dynamic` and `tsan-dynamic` configurations.
3. **Style**: Code must be formatted using `bazel run //tools/format`.

## License
Samosa is released under the MIT License. See [LICENSE](LICENSE) for details.


