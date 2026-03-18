# Shared Memory Camera Example

This example demonstrates how to use the `dex/infrastructure/shared_memory` library to stream high-bandwidth camera frames between processes.

## Components

### C++ Applications
1. **Camera Driver (`camera_driver`)**: A mock producer that generates RGB frames at a target frame rate (default 30 FPS).
2. **Camera Consumer (`camera_consumer`)**: A consumer that attaches to the shared memory stream and reports the end-to-end latency for each received frame.

### Python Applications
3. **Video Reader (`video_reader.py`)**: A publisher that reads a video file from disk (using OpenCV) and streams it to shared memory at a specific framerate.
4. **Rerun Monitor (`rerun_monitor.py`)**: A monitor application that reads frames from shared memory and visualizes them in real-time using [rerun.io](https://rerun.io/).

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
Run the driver in one terminal:
```bash
bazel run //dex/vision/examples/shared_memory_camera:camera_driver -- my_camera_stream 30
```

Run the consumer in another terminal:
```bash
bazel run //dex/vision/examples/shared_memory_camera:camera_consumer -- my_camera_stream
```

### Option B: Video to Rerun (Python)
Ensure you have a Rerun viewer running or accessible.

Run the video reader:
```bash
bazel run //dex/vision/examples/shared_memory_camera:video_reader -- path/to/your/video.mp4 my_video_stream
```

Run the rerun monitor:
```bash
bazel run //dex/vision/examples/shared_memory_camera:rerun_monitor -- my_video_stream
```

### Option C: Cross-Language Stream
This demonstrates the core interoperability of Samosa. The shared memory segments and lock-free protocol are identical across languages.

#### 1. C++ Producer -> Python Consumer
Generate synthetic data in C++ and visualize it in Python (Rerun).

* **Start C++ Producer**:
    ```bash
    bazel run //dex/vision/examples/shared_memory_camera:camera_driver -- cross_lang_1 60
    ```
* **Start Python Consumer**:
    ```bash
    bazel run //dex/vision/examples/shared_memory_camera:rerun_monitor -- cross_lang_1
    ```

#### 2. Python Producer -> C++ Consumer
Read a video file in Python and monitor the end-to-end latency in C++.

* **Start Python Producer**:
    ```bash
    bazel run //dex/vision/examples/shared_memory_camera:video_reader -- path/to/video.mp4 cross_lang_2
    ```
* **Start C++ Consumer**:
    ```bash
    bazel run //dex/vision/examples/shared_memory_camera:camera_consumer -- cross_lang_2
    ```

## Python Bindings

The Python applications use the production bindings located in `//dex/vision/bindings/python`. These bindings provide zero-copy access to the raw image buffers using NumPy.
