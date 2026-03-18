"""Consume video frames from shared memory and log diagnostics (and optionally Rerun)."""

import argparse
import logging
import os
import sys
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


def log_to_rerun(frame_buffer: shm.CameraFrameBuffer) -> None:
    """Log a frame to Rerun."""
    # Extract image data
    w = frame_buffer.color_width
    h = frame_buffer.color_height
    size = frame_buffer.color_image_size

    # Get image data as numpy array
    raw_data = np.array(frame_buffer.color_image_bytes[:size], copy=False)
    image = raw_data.reshape((h, w, 3))

    # Log to rerun
    rr.set_time_seconds("camera_time", 1e-9 * frame_buffer.timestamp_nanos)
    rr.log("camera/image", rr.Image(image))


def parse_args() -> argparse.Namespace:
    """Parse command line arguments."""
    parser = argparse.ArgumentParser(description="Consume video frames from shared memory and log diagnostics")
    parser.add_argument("shm_name", type=str, help="Name of shared memory segment")
    parser.add_argument("--no-rerun", action="store_true", help="Disable Rerun visualization")
    return parser.parse_args()


def main() -> None:
    """Run the camera consumer_py application."""
    args = parse_args()
    logging.basicConfig(level=logging.INFO)
    logger = logging.getLogger("camera_consumer_py")

    use_rerun = HAS_RERUN and not args.no_rerun
    if use_rerun:
        try:
            rr.init("shared_memory_consumer_py", spawn=True)
        except Exception:  # noqa: BLE001
            logger.warning("Rerun failed to initialize. Continuing without Rerun.")
            use_rerun = False

    consumer = shm.Consumer(args.shm_name)
    if not consumer.is_valid():
        logger.error("Failed to create consumer")
        return

    logger.info("Monitoring shared memory '%s'...", args.shm_name)
    logger.info("Rerun visualization is %s", "ENABLED" if use_rerun else "DISABLED")

    last_frame_id = -1
    diag_interval = 5.0  # seconds
    diag_start_time = time.time()
    diag_frame_count = 0
    diag_total_latency_ms = 0.0

    while True:
        status, frame_buffer = consumer.read()
        if status != shm.RunResult.Success:
            if status == shm.RunResult.Timeout:
                shm.StreamingControl.instance().reset()
                time.sleep(0.001)
                continue
            logger.info("Consumer stopped with status: %s", status)
            break

        if frame_buffer is None or frame_buffer.frame_id == last_frame_id:
            time.sleep(0.001)
            continue

        # Use monotonic clock to match C++ steady_clock
        now_ns = time.monotonic_ns()
        last_frame_id = frame_buffer.frame_id
        diag_frame_count += 1
        diag_total_latency_ms += (now_ns - frame_buffer.timestamp_nanos) / 1e6

        if use_rerun:
            log_to_rerun(frame_buffer)

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


if __name__ == "__main__":
    main()
