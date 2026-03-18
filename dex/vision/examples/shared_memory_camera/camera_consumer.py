"""Consumer application for shared memory camera streams with optional visualization."""

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
        # Compatibility path for various environment layouts (especially Bazel)
        HAS_RERUN = False
        for p in sys.path:
            rerun_sdk_path = Path(p) / "rerun_sdk"
            if rerun_sdk_path.exists():
                sys.path.append(str(rerun_sdk_path))
                # Add viewer binary to PATH for spawn mode
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
    """Worker thread that offloads visualization logging to avoid blocking the data loop."""

    def __init__(self, jpeg_quality: int | None = None) -> None:
        """Initialize the visualization worker."""
        super().__init__(daemon=True)
        # Queue size 1 ensures we always display the most recent frame
        self.queue: queue.Queue = queue.Queue(maxsize=1)
        self.running = True
        self.jpeg_quality = jpeg_quality

    def run(self) -> None:
        """Process and log frames to the visualizer."""
        while self.running:
            try:
                frame_data = self.queue.get(timeout=0.1)
                if frame_data is None:
                    continue

                image, timestamp_nanos = frame_data

                # Update visualization timeline
                rr.set_time(timeline="camera_time", timestamp=1e-9 * timestamp_nanos)

                img_log = rr.Image(image)
                if self.jpeg_quality:
                    img_log = img_log.compress(jpeg_quality=self.jpeg_quality)

                rr.log("camera/image", img_log)
            except queue.Empty:
                continue
            except Exception:
                logging.getLogger("camera_consumer_py").exception("Visualization thread error")

    def stop(self) -> None:
        """Stop the visualization worker."""
        self.running = False


def parse_args() -> argparse.Namespace:
    """Parse command line arguments."""
    parser = argparse.ArgumentParser(description="Consume and visualize shared memory camera frames.")
    parser.add_argument("shm_name", type=str, help="Shared memory segment name.")
    parser.add_argument("--no-rerun", action="store_true", help="Disable Rerun visualization.")
    parser.add_argument("--serve", action="store_true", help="Host a server for remote Rerun viewers.")
    parser.add_argument("--connect", type=str, help="Connect to an existing Rerun viewer (IP:PORT).")
    parser.add_argument("--compress", type=int, help="JPEG quality (1-100) for Rerun images.", default=None)
    return parser.parse_args()


def setup_visualizer(args: argparse.Namespace, logger: logging.Logger) -> bool:
    """Initialize the Rerun visualizer based on connection flags."""
    if not HAS_RERUN or args.no_rerun:
        return False

    try:
        # Log version to help debug host/container mismatches
        logger.info("Rerun SDK version: %s", rr.__version__)

        rr.init("camera_consumer_py", spawn=False)
        if args.serve:
            # In Rerun 0.23.0, use serve_grpc for native viewers (port 9876)
            if hasattr(rr, "serve_grpc"):
                logger.info("Rerun: Listening for gRPC connections on 0.0.0.0:9876")
                rr.serve_grpc()
            elif hasattr(rr, "serve"):
                logger.info("Rerun: Listening for connections on 0.0.0.0:9876")
                rr.serve()
            else:
                logger.info("Rerun: Serve not found, using task-link mode")
                rr.connect()
        elif args.connect:
            # In Rerun 0.23.0, use connect_grpc specifically
            if hasattr(rr, "connect_grpc"):
                logger.info("Rerun: Streaming to viewer via gRPC at %s", args.connect)
                rr.connect_grpc(args.connect)
            else:
                logger.info("Rerun: Streaming to viewer at %s", args.connect)
                rr.connect(args.connect)
        else:
            logger.info("Rerun: Launching local viewer")
            rr.spawn()
    except Exception as e:  # noqa: BLE001
        logger.warning("Rerun unavailable: %s. Falling back to diagnostic mode.", e)
        return False
    else:
        return True


def dispatch_frame(frame_buffer: shm.CameraFrameBuffer, worker: VisualizationWorker | None) -> None:
    """Extract and hand off frame data to the visualization worker."""
    if not worker:
        return

    # Extract color buffer region
    size = frame_buffer.color_image_size
    # We copy the data here because frame_buffer is reused in the next iteration
    raw_copy = np.array(frame_buffer.color_image_bytes[:size], copy=True)
    image = raw_copy.reshape((frame_buffer.color_height, frame_buffer.color_width, 3))

    with contextlib.suppress(queue.Full):
        worker.queue.put_nowait((image, frame_buffer.timestamp_nanos))


def main() -> None:
    """Run the camera consumer application."""
    args = parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(name)s: %(message)s")
    logger = logging.getLogger("camera_consumer_py")

    # Initialize visualization
    use_rerun = setup_visualizer(args, logger)
    viz_worker = VisualizationWorker(jpeg_quality=args.compress) if use_rerun else None
    if viz_worker:
        viz_worker.start()

    # Connect to shared memory
    consumer = shm.Consumer(args.shm_name)
    if not consumer.is_valid():
        logger.error("Failed to connect to shared memory segment: %s", args.shm_name)
        return

    logger.info("Attached to stream: %s", args.shm_name)
    logger.info("Visualization: %s", "ENABLED" if use_rerun else "DISABLED")

    # Reusable buffer to minimize allocations
    frame_buffer = shm.CameraFrameBuffer()
    last_frame_id = -1

    # Diagnostics
    diag_interval, diag_start_time = 5.0, time.time()
    diag_frame_count, diag_total_latency_ms = 0, 0.0

    try:
        while True:
            # Block until a new frame is available (GIL is released in C++)
            status = consumer.read_into(frame_buffer)

            if status != shm.RunResult.Success:
                if status == shm.RunResult.Timeout:
                    # Connection alive but no data; reset and continue waiting
                    shm.StreamingControl.instance().reset()
                    continue
                logger.info("Stream ended (status: %s)", status)
                break

            # Deduplicate wakes
            if frame_buffer.frame_id == last_frame_id:
                continue

            # Latency measurement using monotonic clocks
            now_ns = time.monotonic_ns()
            last_frame_id = frame_buffer.frame_id
            diag_frame_count += 1
            diag_total_latency_ms += (now_ns - frame_buffer.timestamp_nanos) / 1e6

            dispatch_frame(frame_buffer, viz_worker)

            # Periodic status report
            now = time.time()
            if now - diag_start_time >= diag_interval:
                avg_fps = diag_frame_count / (now - diag_start_time)
                avg_latency = diag_total_latency_ms / diag_frame_count if diag_frame_count > 0 else 0
                logger.info(
                    "Status | FPS: %.1f | Latency: %.2f ms (avg) | Frame: %d",
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
