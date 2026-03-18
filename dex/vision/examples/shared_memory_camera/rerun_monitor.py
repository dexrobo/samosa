"""Consume video frames from shared memory and log to Rerun."""

import argparse
import logging
import os
import sys
import time
from pathlib import Path

import numpy as np

try:
    import rerun as rr
except ImportError:
    try:
        from rerun_sdk import rerun as rr
    except ImportError:
        # Path hack for some Bazel layouts
        for p in sys.path:
            rerun_sdk_path = Path(p) / "rerun_sdk"
            if rerun_sdk_path.exists():
                sys.path.append(str(rerun_sdk_path))
                # Also add rerun_cli to PATH for spawn=True
                rerun_cli_path = rerun_sdk_path / "rerun_cli"
                if rerun_cli_path.exists():
                    os.environ["PATH"] += os.pathsep + str(rerun_cli_path)
                break
        import rerun as rr

import dex.vision.shared_memory as shm


def main() -> None:
    """Run the rerun monitor application."""
    parser = argparse.ArgumentParser(description="Consume video frames from shared memory and log to Rerun")
    parser.add_argument("shm_name", type=str, help="Name of shared memory segment")
    args = parser.parse_args()

    logging.basicConfig(level=logging.INFO)
    logger = logging.getLogger("rerun_monitor")

    rr.init("shared_memory_monitor", spawn=True)

    consumer = shm.Consumer(args.shm_name)
    if not consumer.is_valid():
        logger.error("Failed to create consumer")
        return

    logger.info("Monitoring shared memory '%s'...", args.shm_name)

    last_frame_id = -1
    diag_interval = 5.0  # seconds
    diag_start_time = time.time()
    diag_frame_count = 0
    diag_total_latency_ms = 0.0

    while True:
        status, frame_buffer = consumer.read()
        if status != shm.RunResult.Success:
            if status == shm.RunResult.Timeout:
                # If the underlying C++ control was stopped due to timeout, reset it so we can keep waiting
                shm.StreamingControl.instance().reset()
                time.sleep(0.001)
                continue

            logger.info("Consumer stopped with status: %s", status)
            break

        if frame_buffer is None:
            # Should not happen if status is Success
            time.sleep(0.001)
            continue

        if frame_buffer.frame_id == last_frame_id:
            # Skip if no new frame
            time.sleep(0.001)
            continue

        now_ns = time.time_ns()
        last_frame_id = frame_buffer.frame_id
        diag_frame_count += 1
        diag_total_latency_ms += (now_ns - frame_buffer.timestamp_nanos) / 1e6

        # Extract image data
        w = frame_buffer.color_width
        h = frame_buffer.color_height
        size = frame_buffer.color_image_size

        # Get image data as numpy array
        # Note: color_image_bytes returns a nb.ndarray view of the whole buffer
        # We need to slice it to the actual image size and reshape it.
        raw_data = np.array(frame_buffer.color_image_bytes[:size], copy=False)
        image = raw_data.reshape((h, w, 3))

        # Log to rerun
        rr.set_time_nanos("camera_time", frame_buffer.timestamp_nanos)
        rr.log("camera/image", rr.Image(image))

        # Periodic diagnostics
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
            # Reset counters
            diag_start_time = now
            diag_frame_count = 0
            diag_total_latency_ms = 0.0


if __name__ == "__main__":
    main()
