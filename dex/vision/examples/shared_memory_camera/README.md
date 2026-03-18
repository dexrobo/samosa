# Shared Memory Camera Example

This example demonstrates how to use the `dex/infrastructure/shared_memory` library to stream high-bandwidth camera frames between processes. It showcases interoperability between C++ and Python components.

## Components

### Producers (Sources)
1. **Camera Producer CC (`camera_producer_cc`)**: A mock C++ producer that generates synthetic RGB frames at a target frame rate.
2. **Video Producer PY (`video_producer_py`)**: A Python producer that reads a video file from disk (OpenCV) and streams it to shared memory.

### Consumers (Sinks)
3. **Camera Consumer CC (`camera_consumer_cc`)**: A C++ consumer that attaches to the shared memory stream and reports end-to-end latency.
4. **Camera Consumer PY (`camera_consumer_py`)**: A Python consumer that reads frames, logs performance diagnostics, and optionally visualizes them via Rerun.

## Data Structure

The example uses `CameraFrameBuffer` (defined in `dex/drivers/camera/base/types.h`), which is approximately 16.6MB per frame. This includes:
* **Metadata**: Frame ID, Timestamps, Serial Number, Camera Name, Calibration.
* **Color Buffer**: 1920x1080 RGB24 (~6.2MB).
* **Depth Buffer**: 1920x1080 Depth16 (~4.1MB).
* **Stereo Right Buffer**: 1920x1080 RGB24 (~6.2MB).

## Running the Example

First, build all targets:
```bash
bazel build //dex/vision/examples/shared_memory_camera/...
```

### Option A: Pure C++ Stream
Run the producer in one terminal:
```bash
bazel run //dex/vision/examples/shared_memory_camera:camera_producer_cc -- my_camera_stream 30
```

Run the consumer in another terminal:
```bash
bazel run //dex/vision/examples/shared_memory_camera:camera_consumer_cc -- my_camera_stream
```

### Option B: Pure Python Stream
Start streaming a video file:
```bash
bazel run //dex/vision/examples/shared_memory_camera:video_producer_py -- path/to/your/video.mp4 my_video_stream
```

Watch the stream (and optionally visualize in Rerun):
```bash
bazel run //dex/vision/examples/shared_memory_camera:camera_consumer_py -- my_video_stream
```

### Option C: Cross-Language Stream
This demonstrates that the shared memory protocol is identical across languages.

#### 1. C++ Producer -> Python Consumer
Generate synthetic data in C++ and visualize it in Python.

* **Start C++ Producer**:
    ```bash
    bazel run //dex/vision/examples/shared_memory_camera:camera_producer_cc -- cross_lang_1 60
    ```
* **Start Python Consumer**:
    ```bash
    bazel run //dex/vision/examples/shared_memory_camera:camera_consumer_py -- cross_lang_1
    ```

#### 2. Python Producer -> C++ Consumer
Read a video file in Python and monitor latency in C++.

* **Start Python Producer**:
    ```bash
    bazel run //dex/vision/examples/shared_memory_camera:video_producer_py -- path/to/video.mp4 cross_lang_2
    ```
* **Start C++ Consumer**:
    ```bash
    bazel run //dex/vision/examples/shared_memory_camera:camera_consumer_cc -- cross_lang_2
    ```

## Python Bindings

The Python applications use the production bindings located in `//dex/vision/bindings/python`. These bindings provide zero-copy access to the raw image buffers using NumPy.
