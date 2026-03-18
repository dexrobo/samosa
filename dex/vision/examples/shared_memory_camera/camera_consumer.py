"""Consume video frames from shared memory and log diagnostics (and optionally Rerun)."""

import argparse
import contextlib
import logging
import os
import queue
import sys
import threading
import time
from pathlib import Path

import numpy as np

try:
    import rerun as rr

    HAS_RERUN = True
except ImportError:
    try:
        from rerun_sdk import rerun as rr

        HAS_RERUN = True
    except ImportError:
        # Path hack for some Bazel layouts
        HAS_RERUN = False
        for p in sys.path:
            rerun_sdk_path = Path(p) / "rerun_sdk"
            if rerun_sdk_path.exists():
                sys.path.append(str(rerun_sdk_path))
                # Also add rerun_cli to PATH for spawn=True
                rerun_cli_path = rerun_sdk_path / "rerun_cli"
                if rerun_cli_path.exists():
                    os.environ["PATH"] += os.pathsep + str(rerun_cli_path)
                try:
                    import rerun as rr

                    HAS_RERUN = True
                except ImportError:
                    pass
                break

import dex.vision.shared_memory as shm


class VisualizationWorker(threading.Thread):
    """Handles logging to Rerun in a separate thread to keep the consumer loop fast."""

    def __init__(self, jpeg_quality: int | None = None) -> None:
        """Initialize the visualization worker."""
        super().__init__(daemon=True)
        # Size 1 ensures we always prioritize the most recent data
        self.queue: queue.Queue = queue.Queue(maxsize=1)
        self.running = True
        self.jpeg_quality = jpeg_quality

    def run(self) -> None:
        """Process visualization requests."""
        while self.running:
            try:
                frame_data = self.queue.get(timeout=0.1)
                if frame_data is None:
                    continue

                # Unpack: image is a copy of the buffer data
                image, timestamp_nanos = frame_data

                rr.set_time_seconds("camera_time", 1e-9 * timestamp_nanos)
                img_log = rr.Image(image)
                if self.jpeg_quality:
                    img_log = img_log.compress(jpeg_quality=self.jpeg_quality)

                rr.log("camera/image", img_log)
            except queue.Empty:
                continue
            except Exception:
                logging.getLogger("camera_consumer_py").exception("Visualization thread encountered an error")

    def stop(self) -> None:
        """Stop the worker."""
        self.running = False


def parse_args() -> argparse.Namespace:
    """Parse command line arguments."""
    parser = argparse.ArgumentParser(description="Consumer application for shared memory camera streams.")
    parser.add_argument("shm_name", type=str, help="Name of the shared memory segment to attach to.")
    parser.add_argument("--no-rerun", action="store_true", help="Disable Rerun visualization.")
    parser.add_argument("--serve", action="store_true", help="Host a Rerun server for remote viewers.")
    parser.add_argument("--connect", type=str, help="Connect to an existing Rerun viewer (e.g. 127.0.0.1:9876).")
    parser.add_argument("--compress", type=int, help="JPEG quality (1-100) for Rerun images.", default=None)
    return parser.parse_args()


def init_visualizer(args: argparse.Namespace, logger: logging.Logger) -> bool:
    """Initialize the Rerun visualizer based on the requested mode."""
    if not HAS_RERUN or args.no_rerun:
        return False

    try:
        rr.init("shared_memory_consumer_py")
        if args.serve:
            logger.info("Rerun: Hosting server on 0.0.0.0:9876")
            rr.serve()
        elif args.connect:
            logger.info("Rerun: Connecting to %s", args.connect)
            rr.connect(args.connect)
        else:
            logger.info("Rerun: Spawning local viewer")
            rr.spawn()
    except Exception as e:  # noqa: BLE001
        logger.warning("Rerun visualization disabled: %s", e)
        return False
    else:
        return True


def dispatch_to_visualizer(frame_buffer: shm.CameraFrameBuffer, worker: VisualizationWorker | None) -> None:
    """Prepare and send frame data to the visualization worker."""
    if not worker:
        return

    # Extract the active region of the color buffer
    size = frame_buffer.color_image_size
    # We must copy the data before dispatching to the thread, as the next
    # iteration of the consumer loop will overwrite frame_buffer.
    raw_copy = np.array(frame_buffer.color_image_bytes[:size], copy=True)
    image = raw_copy.reshape((frame_buffer.color_height, frame_buffer.color_width, 3))

    with contextlib.suppress(queue.Full):
        worker.queue.put_nowait((image, frame_buffer.timestamp_nanos))


def main() -> None:
    """Run the camera consumer application."""
    args = parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(name)s: %(message)s")
    logger = logging.getLogger("camera_consumer_py")

    # Setup visualization
    use_rerun = init_visualizer(args, logger)
    viz_worker = VisualizationWorker(jpeg_quality=args.compress) if use_rerun else None
    if viz_worker:
        viz_worker.start()

    # Initialize shared memory consumer
    consumer = shm.Consumer(args.shm_name)
    if not consumer.is_valid():
        logger.error("Could not connect to shared memory segment: %s", args.shm_name)
        return

    logger.info("Attached to stream: %s", args.shm_name)
    logger.info("Visualization: %s", "ENABLED" if use_rerun else "DISABLED")

    # Pre-allocate buffer to avoid GC pressure
    frame_buffer = shm.CameraFrameBuffer()
    last_frame_id = -1

    # Diagnostic counters
    diag_interval = 5.0
    diag_start_time = time.time()
    diag_frame_count = 0
    diag_total_latency_ms = 0.0

    try:
        while True:
            # Block until a new frame is available (releasing Python GIL)
            status = consumer.read_into(frame_buffer)

            if status != shm.RunResult.Success:
                if status == shm.RunResult.Timeout:
                    # Producer might be idle or restarting; reset and wait again
                    shm.StreamingControl.instance().reset()
                    continue
                logger.info("Stream stopped (status: %s)", status)
                break

            # Deduplicate (futex might wake multiple times for the same index)
            if frame_buffer.frame_id == last_frame_id:
                continue

            # Calculate metrics using monotonic clock
            now_ns = time.monotonic_ns()
            last_frame_id = frame_buffer.frame_id
            diag_frame_count += 1
            diag_total_latency_ms += (now_ns - frame_buffer.timestamp_nanos) / 1e6

            # Offload heavy visualization work
            dispatch_to_visualizer(frame_buffer, viz_worker)

            # Log periodic status
            now = time.time()
            if now - diag_start_time >= diag_interval:
                avg_fps = diag_frame_count / (now - diag_start_time)
                avg_latency = diag_total_latency_ms / diag_frame_count if diag_frame_count > 0 else 0
                logger.info(
                    "FPS: %.1f | Latency: %.2f ms (avg) | Last Frame: %d",
                    avg_fps,
                    avg_latency,
                    last_frame_id,
                )
                diag_start_time, diag_frame_count, diag_total_latency_ms = now, 0, 0.0

    except KeyboardInterrupt:
        logger.info("Stopping...")
    finally:
        if viz_worker:
            viz_worker.stop()
        logger.info("Stopped.")


if __name__ == "__main__":
    main()
