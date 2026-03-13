# Samosa: Resilient Lock-Free Snapshot Streaming

Samosa is a high-performance C++20 library designed for ultra-low latency data streaming between processes. Unlike traditional message queues, Samosa implements **snapshot semantics**: consumers always receive the latest available data, making it ideal for high-frequency sensor data, telemetry, and video frames where throughput and "current state" are prioritized over historical completeness.

## Key Features

- **Non-Blocking Publisher**: Producers never block on consumers. High-frequency updates are published to shared memory with minimal overhead.
- **Latest-Only Consumption**: Consumers always fetch the most recent snapshot. If multiple updates occurred since the last read, intermediate ones are skipped to prevent "lag" buildup.
- **Independent Lifecycle Resilience**: Producers and consumers can restart at any time. They automatically reconnect to the shared memory segment and resume data transfer reliably.
- **Lock-Free Internals**: Uses atomic operations and Linux futexes for synchronization, avoiding heavy kernel locks.
- **Double-Buffered**: Implements internal double-buffering to ensure the consumer never reads a partially written frame.

## Usage

### 1. Define your Data Structure
The data structure must be a "Plain Old Data" (POD) type suitable for shared memory (no pointers to heap memory).

```cpp
#include <array>
#include <cstddef>

struct Telemetry {
  uint64_t timestamp_ns;
  std::array<float, 6> pose;
  std::array<uint8_t, 1024> payload;
};
```

### 2. Initialize the Shared Memory Segment
One process (usually the producer or a manager) must create the segment.

```cpp
#include "dex/infrastructure/shared_memory/shared_memory_streaming.h"

using dex::shared_memory::SharedMemory;
using dex::shared_memory::LockFreeSharedMemoryBuffer;
using dex::shared_memory::InitializeBuffer;

const std::string shm_name = "/telemetry_stream";

// Create the segment (or fail if it exists)
auto shm = SharedMemory<Telemetry, 2, LockFreeSharedMemoryBuffer>::Create(
    shm_name, InitializeBuffer<Telemetry>);
```

### 3. Publish Data
Producers use the `Producer` class to stream snapshots.

```cpp
#include "dex/infrastructure/shared_memory/shared_memory_streaming.h"

dex::shared_memory::Producer<Telemetry> producer{shm_name};

producer.Run([](Telemetry& buffer, uint32_t frame_count, int buffer_id) {
    buffer.timestamp_ns = GetTimeNs();
    // Fill buffer...
});
```

### 4. Consume Data
Consumers use the `Consumer` class to receive the latest snapshot. The `Run` method blocks if no new data is available.

```cpp
#include "dex/infrastructure/shared_memory/shared_memory_streaming.h"

dex::shared_memory::Consumer<Telemetry> consumer{shm_name};

consumer.Run([](const Telemetry& buffer) {
    std::cout << "Received snapshot at: " << buffer.timestamp_ns << std::endl;
});
```

## Contributing

We welcome contributions! To ensure high standards of correctness and performance, please follow these steps:

### Development Environment
The easiest way to contribute is using the provided **Dev Container**. It contains all necessary dependencies (Bazel, GCC 12, Python, Node.js).

### Verification
Before submitting a Pull Request, you **must** run the comprehensive verification suite. This script runs all formatters, linters (Ruff and MyPy), and test configurations (including ASAN, TSAN, and UBSAN).

```bash
# Run everything
./tools/check.sh all

# Or run specific parts
./tools/check.sh format
./tools/check.sh lint
./tools/check.sh test-prod
```

### PR Guidelines
1. **Tests**: Every fix or feature requires a corresponding test case in `dex/infrastructure/shared_memory/`.
2. **Sanitizers**: Ensure your changes pass under both `asan-dynamic` and `tsan-dynamic` configurations.
3. **Style**: Code must be formatted using `bazel run //tools/format`.

## License
Samosa is released under the MIT License. See [LICENSE](LICENSE) for details.
