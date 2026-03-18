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


class RerunThread(threading.Thread):
    """Separate thread for Rerun logging to avoid blocking the consumer loop."""

    def __init__(self, jpeg_quality: int | None = None) -> None:
        """Initialize the Rerun logging thread."""
        super().__init__(daemon=True)
        self.queue: queue.Queue = queue.Queue(maxsize=2)  # Keep queue small to prioritize latest data
        self.running = True
        self.jpeg_quality = jpeg_quality

    def run(self) -> None:
        """Continuously log frames from the queue to Rerun."""
        while self.running:
            try:
                frame_data = self.queue.get(timeout=0.1)
                if frame_data is None:
                    continue

                # Unpack frame data
                image, timestamp_nanos = frame_data

                # Log to rerun
                rr.set_time_seconds("camera_time", 1e-9 * timestamp_nanos)
                img_log = rr.Image(image)
                if self.jpeg_quality:
                    img_log = img_log.compress(jpeg_quality=self.jpeg_quality)

                rr.log("camera/image", img_log)
            except queue.Empty:
                continue
            except Exception:
                logging.getLogger("camera_consumer_py").exception("Rerun thread error")

    def stop(self) -> None:
        """Stop the Rerun logging thread."""
        self.running = False


def parse_args() -> argparse.Namespace:
    """Parse command line arguments."""
    parser = argparse.ArgumentParser(description="Consume video frames from shared memory and log diagnostics")
    parser.add_argument("shm_name", type=str, help="Name of shared memory segment")
    parser.add_argument("--no-rerun", action="store_true", help="Disable Rerun visualization")
    parser.add_argument("--serve", action="store_true", help="Host a Rerun server for external viewers to connect")
    parser.add_argument("--connect", type=str, help="Connect to an existing Rerun viewer (e.g. 127.0.0.1:9876)")
    parser.add_argument("--compress", type=int, help="JPEG quality (1-100) for Rerun images", default=None)
    return parser.parse_args()


def init_rerun(args: argparse.Namespace, logger: logging.Logger) -> bool:
    """Initialize Rerun based on requested mode."""
    if not HAS_RERUN or args.no_rerun:
        return False

    try:
        rr.init("shared_memory_consumer_py")
        if args.serve:
            logger.info("Rerun: Serving on 0.0.0.0:9876...")
            rr.serve()
        elif args.connect:
            logger.info("Rerun: Connecting to %s...", args.connect)
            rr.connect(args.connect)
        else:
            logger.info("Rerun: Spawning local viewer...")
            rr.spawn()
    except Exception as e:  # noqa: BLE001
        logger.warning("Failed to initialize Rerun: %s. Continuing in diagnostic mode.", e)
        return False
    else:
        return True


def process_frame(frame_buffer: shm.CameraFrameBuffer, rerun_worker: RerunThread | None) -> None:
    """Process a single frame and optionally send it to the Rerun worker."""
    if not rerun_worker:
        return

    # Extract image data
    w = frame_buffer.color_width
    h = frame_buffer.color_height
    size = frame_buffer.color_image_size

    # Get image data as numpy array (zero-copy view)
    raw_data = np.array(frame_buffer.color_image_bytes[:size], copy=False)
    image = raw_data.reshape((h, w, 3)).copy()  # Must copy for thread safety

    # Non-blocking put to avoid slowing down consumer
    with contextlib.suppress(queue.Full):
        rerun_worker.queue.put_nowait((image, frame_buffer.timestamp_nanos))


def main() -> None:
    """Run the camera consumer_py application."""
    args = parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(name)s: %(message)s")
    logger = logging.getLogger("camera_consumer_py")

    use_rerun = init_rerun(args, logger)
    rerun_worker = RerunThread(jpeg_quality=args.compress) if use_rerun else None
    if rerun_worker:
        rerun_worker.start()

    consumer = shm.Consumer(args.shm_name)
    if not consumer.is_valid():
        logger.error("Failed to create consumer")
        return

    logger.info("Monitoring shared memory '%s'...", args.shm_name)
    logger.info("Rerun visualization is %s", "ENABLED" if use_rerun else "DISABLED")

    frame_buffer = shm.CameraFrameBuffer()
    last_frame_id = -1
    diag_interval, diag_start_time = 5.0, time.time()
    diag_frame_count, diag_total_latency_ms = 0, 0.0

    try:
        while True:
            status = consumer.read_into(frame_buffer)
            if status != shm.RunResult.Success:
                if status == shm.RunResult.Timeout:
                    shm.StreamingControl.instance().reset()
                    time.sleep(0.001)
                    continue
                logger.info("Consumer stopped with status: %s", status)
                break

            if frame_buffer.frame_id == last_frame_id:
                time.sleep(0.001)
                continue

            now_ns = time.monotonic_ns()
            last_frame_id = frame_buffer.frame_id
            diag_frame_count += 1
            diag_total_latency_ms += (now_ns - frame_buffer.timestamp_nanos) / 1e6

            process_frame(frame_buffer, rerun_worker)

            now = time.time()
            if now - diag_start_time >= diag_interval:
                avg_fps = diag_frame_count / (now - diag_start_time)
                avg_latency = diag_total_latency_ms / diag_frame_count if diag_frame_count > 0 else 0
                logger.info(
                    "[Diagnostics] FPS: %.1f, Latency: %.2f ms (avg), Last ID: %d",
                    avg_fps,
                    avg_latency,
                    last_frame_id,
                )
                diag_start_time, diag_frame_count, diag_total_latency_ms = now, 0, 0.0
    except KeyboardInterrupt:
        logger.info("Shutdown requested via ^C")
    finally:
        if rerun_worker:
            rerun_worker.stop()
        logger.info("Cleaner exit successful.")


if __name__ == "__main__":
    main()
