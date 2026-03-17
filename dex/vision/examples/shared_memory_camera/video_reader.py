"""Read video from disk and publish to shared memory."""

import argparse
import logging
import time

import cv2
import numpy as np

from dex.vision.examples.shared_memory_camera import shared_memory_camera_bindings as shm

# Constants matching C++ dex::camera
MAX_WIDTH = 1920
MAX_HEIGHT = 1080


def main() -> None:
    """Run the video reader application."""
    parser = argparse.ArgumentParser(description="Read video from disk and publish to shared memory")
    parser.add_argument("video_path", type=str, help="Path to video file")
    parser.add_argument("shm_name", type=str, help="Name of shared memory segment")
    parser.add_argument("--fps", type=float, default=None, help="Target FPS (default is video FPS)")
    args = parser.parse_args()

    logging.basicConfig(level=logging.INFO)
    logger = logging.getLogger("video_reader")

    cap = cv2.VideoCapture(args.video_path)
    if not cap.isOpened():
        logger.error("Error: Could not open video %s", args.video_path)
        return

    video_fps = cap.get(cv2.CAP_PROP_FPS)
    target_fps = args.fps if args.fps else video_fps
    frame_duration = 1.0 / target_fps

    logger.info("Initializing shared memory '%s'", args.shm_name)
    if not shm.initialize_shared_memory(args.shm_name):
        logger.error("Failed to initialize shared memory")
        return

    producer = shm.Producer(args.shm_name)
    if not producer.is_valid():
        logger.error("Failed to create producer")
        return

    frame_id = 0
    while cap.isOpened():
        start_time = time.time()

        ret, frame = cap.read()
        if not ret:
            # Loop video
            cap.set(cv2.CAP_PROP_POS_FRAMES, 0)
            continue

        # Convert to RGB (OpenCV uses BGR)
        frame_rgb = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)

        # Resize if necessary to fit buffer constraints
        h, w, _ = frame_rgb.shape
        if w > MAX_WIDTH or h > MAX_HEIGHT:
            scale = min(MAX_WIDTH / w, MAX_HEIGHT / h)
            frame_rgb = cv2.resize(frame_rgb, (int(w * scale), int(h * scale)))
            h, w, _ = frame_rgb.shape

        # Create and populate CameraFrameBuffer
        buffer = shm.CameraFrameBuffer()
        buffer.color_width = w
        buffer.color_height = h
        buffer.color_image_size = w * h * 3
        buffer.frame_id = frame_id
        buffer.timestamp_nanos = int(time.time() * 1e9)
        buffer.camera_name = "VideoReader"

        # Map frame data to buffer.color_image_bytes
        flat_frame = frame_rgb.flatten()
        buffer.color_image_bytes[: len(flat_frame)] = flat_frame.view(np.uint8)

        producer.write(buffer)

        frame_id += 1
        if frame_id % 30 == 0:
            logger.info("Published frame %d", frame_id)

        elapsed = time.time() - start_time
        if elapsed < frame_duration:
            time.sleep(frame_duration - elapsed)

    cap.release()


if __name__ == "__main__":
    main()
