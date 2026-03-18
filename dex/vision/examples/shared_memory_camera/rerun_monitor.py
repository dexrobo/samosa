"""Consume video frames from shared memory and log to Rerun."""

import argparse
import logging
import time

import numpy as np
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
    while True:
        frame_buffer = consumer.read()
        if frame_buffer is None:
            # If the underlying C++ control was stopped due to timeout, reset it so we can keep waiting
            if not shm.StreamingControl.instance().is_running():
                shm.StreamingControl.instance().reset()
            # Yield to other processes
            time.sleep(0.001)
            continue

        if frame_buffer.frame_id == last_frame_id:
            # Skip if no new frame
            time.sleep(0.001)
            continue

        last_frame_id = frame_buffer.frame_id

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

        if last_frame_id % 30 == 0:
            logger.info("Consumed frame %d", last_frame_id)


if __name__ == "__main__":
    main()
