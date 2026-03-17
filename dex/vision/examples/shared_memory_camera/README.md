# Shared Memory Camera Example

This example demonstrates how to use the `dex/infrastructure/shared_memory` library to stream high-bandwidth camera frames between processes.

## Components

1. **Camera Driver (`camera_driver`)**: A mock producer that generates RGB frames at a target frame rate (default 30 FPS).
2. **Camera Consumer (`camera_consumer`)**: A consumer that attaches to the shared memory stream and reports the end-to-end latency for each received frame.

## Data Structure

The example uses `CameraFrameBuffer` (defined in `dex/drivers/camera/base/types.h`), which is approximately 16.6MB per frame. This includes:
* Metadata (Frame ID, Timestamps, Intrinsics, Extrinsics)
* Raw RGB Color Buffer (1920x1080)
* Raw Depth Buffer (1920x1080)

## Running the Example

First, build the targets:
```bash
bazel build //dex/vision/examples/shared_memory_camera/...
```

Run the driver in one terminal:
```bash
./bazel-bin/dex/vision/examples/shared_memory_camera/camera_driver my_camera_stream 30
```

Run the consumer in another terminal:
```bash
./bazel-bin/dex/vision/examples/shared_memory_camera/camera_consumer my_camera_stream
```
