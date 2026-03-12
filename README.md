# Shared Memory IPC

This library provides a simple interface for creating and using shared memory for inter-process communication (IPC).

## Usage
> [!IMPORTANT] TODO: Fix usage examples to match the actual public API (SharedMemory::Create/Open, etc.).

First, define the data-structure that you want to publish and consume.
```cpp
struct FrameBuffer {
  std::array<std::byte, kPixelDataSize> pixel_data;
  std::array<uint16_t, kResolution> depth_data;
  std::array<char, kNameLength> camera_name;
  unsigned long timestamp;
  unsigned long frame_id;
};
```
Then,

1. Create a shared memory object

   ```cpp
   SharedMemory<FrameBuffer> shared_memory(shared_memory_name, true);
   if (!shared_memory) {
     std::cerr << "Failed to create shared memory.\n";
     return 1;
   }
   ```
   > [!WARNING] The shared memory must be created first before publishing or consuming data.

2. Publish data to the shared memory object

   ```cpp
   auto producer = Producer<FrameBuffer>{shared_memory_name};
   producer.Run([](FrameBuffer& buffer, const uint frame_count) {
     // TODO: Fill buffer with data
   });
   ```
   > Internally, the producer will alternate between buffers to publish data and provide the correct buffer to the callback.
3. Consume data from the shared memory object

   ```cpp
   auto consumer = Consumer<FrameBuffer>{shared_memory_name};
   consumer.Run([](FrameBuffer& buffer) {
     // TODO: Process buffer
   });
   ```
   > [!NOTE] Producer and consumer can be brought up in any order.

4. Destroy the shared memory

   ```cpp
  if (!SharedMemory<FrameBuffer>::Destroy(shared_memory_name)) {
     std::cerr << "Failed to unlink shared memory.\n";
     return 1;
   }
   ```

## Run the tests

```sh
bazel test --config=prod //dex/infrastructure/shared_memory/... --test_output=all
```

## Run the benchmark
```sh
bazel run --config=prod //dex/infrastructure/shared_memory:run_benchmark_with_stats
```

## Run the example
Create the shared memory object
```sh
bazel run //dex/infrastructure/shared_memory:shared_memory_streaming_example create /shared-mem
```
Run the producer
```sh
bazel run //dex/infrastructure/shared_memory:shared_memory_streaming_example publisher /shared-mem -- --debug
```
Run the consumer
```sh
bazel run //dex/infrastructure/shared_memory:shared_memory_streaming_example consumer /shared-mem -- --debug
```
> [!NOTE] To save the non-debug output to a file, do:
> ```sh
> bazel run //dex/infrastructure/shared_memory:shared_memory_ipc_example consumer /shared-mem -- --debug | tee console.log
> ```

Destroy the shared memory object
```sh
bazel run //dex/infrastructure/shared_memory:shared_memory_streaming_example destroy /shared-mem
```
