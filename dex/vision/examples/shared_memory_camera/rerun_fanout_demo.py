"""Fan out a video stream to both a throttled consumer and a source-rate monitor.

This example launches three Python processes:

1. A producer that streams video frames into shared memory at the source video's frame rate.
2. A normal shared-memory consumer that reads the stream and logs to a Rerun gRPC server at 5 Hz.
3. A passive shared-memory monitor that samples the same stream and logs to a second Rerun gRPC server
   at the full source frame rate.
"""

from __future__ import annotations

import argparse
import logging
import multiprocessing as mp
import os
import signal
import sys
import time
from pathlib import Path
from typing import Any

import cv2
import dex.vision.shared_memory as shm
import numpy as np

DEFAULT_ATTACH_TIMEOUT_SEC = 5.0
DEFAULT_VIDEO_FPS = 30.0


def import_rerun() -> Any:
    """Import rerun from a few layouts used in local and Bazel environments."""
    try:
        import rerun as rr
    except ImportError:
        try:
            from rerun_sdk import rerun as rr
        except ImportError:
            for sys_path in sys.path:
                rerun_sdk_path = Path(sys_path) / "rerun_sdk"
                if not rerun_sdk_path.exists():
                    continue
                sys.path.append(str(rerun_sdk_path))
                rerun_cli_path = rerun_sdk_path / "rerun_cli"
                if rerun_cli_path.exists():
                    os.environ["PATH"] += os.pathsep + str(rerun_cli_path)
                import rerun as rr

                return rr
            raise
        else:
            return rr
    else:
        return rr


def configure_logging() -> None:
    """Configure process-local logging."""
    logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(processName)s] %(levelname)s: %(message)s")


def frame_to_rgb_image(frame_buffer: shm.CameraFrameBuffer) -> np.ndarray:
    """Copy the active color payload out of the shared-memory POD buffer."""
    size = frame_buffer.color_image_size
    raw_copy = np.array(frame_buffer.color_image_bytes[:size], copy=True)
    return raw_copy.reshape((frame_buffer.color_height, frame_buffer.color_width, 3))


def fit_frame_to_buffer(frame_bgr: np.ndarray) -> np.ndarray:
    """Convert a BGR OpenCV frame to RGB and resize if needed for CameraFrameBuffer limits."""
    frame_rgb = cv2.cvtColor(frame_bgr, cv2.COLOR_BGR2RGB)
    height, width, _ = frame_rgb.shape
    if width <= shm.MAX_WIDTH and height <= shm.MAX_HEIGHT:
        return frame_rgb

    scale = min(shm.MAX_WIDTH / width, shm.MAX_HEIGHT / height)
    resized = cv2.resize(frame_rgb, (int(width * scale), int(height * scale)))
    return resized


def populate_buffer(
    buffer: shm.CameraFrameBuffer,
    frame_rgb: np.ndarray,
    frame_id: int,
    video_time_nanos: int,
    camera_name: str,
) -> None:
    """Fill the POD shared-memory buffer from a numpy RGB frame."""
    height, width, _ = frame_rgb.shape
    flat_frame = frame_rgb.reshape(-1)

    buffer.color_width = width
    buffer.color_height = height
    buffer.color_image_size = width * height * 3
    buffer.frame_id = frame_id
    buffer.timestamp_nanos = video_time_nanos
    buffer.camera_name = camera_name
    buffer.color_image_bytes[: flat_frame.size] = flat_frame.view(np.uint8)


def get_video_fps(cap: cv2.VideoCapture) -> float:
    """Get a usable source-video frame rate for timeline reconstruction."""
    video_fps = cap.get(cv2.CAP_PROP_FPS)
    if video_fps > 0:
        return video_fps
    logging.warning("Video FPS unavailable, falling back to %.1f FPS for Rerun timing", DEFAULT_VIDEO_FPS)
    return DEFAULT_VIDEO_FPS


def initialize_rerun(application_id: str, grpc_port: int) -> Any:
    """Initialize a Rerun recording stream and expose it over gRPC."""
    rr = import_rerun()
    import rerun.blueprint as rrb

    rr.init(application_id, spawn=False)
    rr.send_blueprint(
        rrb.Blueprint(
            rrb.TimePanel(
                timeline="video_time",
                playback_speed=1.0,
                play_state="playing",
            ),
            auto_views=True,
            auto_layout=True,
        ),
        make_active=True,
        make_default=True,
    )
    server_uri = rr.serve_grpc(grpc_port=grpc_port, server_memory_limit="256MB")
    logging.info("Rerun gRPC server for %s listening at %s", application_id, server_uri)
    return rr


def log_frame(rr: Any, entity_path: str, frame_buffer: shm.CameraFrameBuffer) -> None:
    """Log one frame into Rerun."""
    image = frame_to_rgb_image(frame_buffer)
    rr.set_time("video_time", duration=np.timedelta64(frame_buffer.timestamp_nanos, "ns"))
    rr.log(entity_path, rr.Image(image))


def wait_for_valid_endpoint(
    factory: Any, shm_name: str, role: str, timeout_sec: float = DEFAULT_ATTACH_TIMEOUT_SEC
) -> Any:
    """Retry attaching to shared memory until it becomes valid or the timeout expires."""
    deadline = time.monotonic() + timeout_sec
    while time.monotonic() < deadline:
        endpoint = factory(shm_name)
        if endpoint.is_valid():
            return endpoint
        time.sleep(0.05)

    raise RuntimeError(f"{role} could not attach to shared memory segment {shm_name!r} within {timeout_sec:.1f}s")


def producer_main(args: argparse.Namespace, stop_event: mp.synchronize.Event) -> None:
    """Read a video file and publish it to shared memory at the source frame rate."""
    configure_logging()

    if not shm.initialize_shared_memory(args.shm_name):
        logging.error("Failed to initialize shared memory %s", args.shm_name)
        return

    producer = shm.Producer(args.shm_name)
    if not producer.is_valid():
        logging.error("Failed to create producer for %s", args.shm_name)
        return

    cap = cv2.VideoCapture(args.video_path)
    if not cap.isOpened():
        logging.error("Could not open video %s", args.video_path)
        return

    video_fps = get_video_fps(cap)
    frame_period_sec = 1.0 / video_fps
    buffer = shm.CameraFrameBuffer()
    frame_id = 0
    logging.info(
        "Producer streaming %s into %s at source rate %.3f FPS",
        args.video_path,
        args.shm_name,
        video_fps,
    )

    try:
        while not stop_event.is_set():
            frame_start = time.monotonic()
            ok, frame_bgr = cap.read()
            if not ok:
                if args.loop:
                    cap.set(cv2.CAP_PROP_POS_FRAMES, 0)
                    continue
                break

            frame_rgb = fit_frame_to_buffer(frame_bgr)
            video_time_nanos = int(round((frame_id * 1_000_000_000) / video_fps))
            populate_buffer(buffer, frame_rgb, frame_id, video_time_nanos, "RerunFanoutProducer")
            producer.write(buffer)

            if frame_id % 120 == 0:
                logging.info("Produced frame %d", frame_id)
            frame_id += 1

            elapsed = time.monotonic() - frame_start
            remaining = frame_period_sec - elapsed
            if remaining > 0:
                time.sleep(remaining)
    finally:
        cap.release()
        logging.info("Producer stopped at frame %d", frame_id)


def consumer_rerun_main(args: argparse.Namespace, stop_event: mp.synchronize.Event) -> None:
    """Read the stream through the real consumer path and publish to Rerun at 5 Hz."""
    configure_logging()
    rr = initialize_rerun("shared_memory_consumer_rerun", args.consumer_grpc_port)

    try:
        consumer = wait_for_valid_endpoint(shm.Consumer, args.shm_name, "consumer")
    except RuntimeError as error:
        logging.error("%s", error)
        return

    frame_buffer = shm.CameraFrameBuffer()
    last_frame_id = -1
    next_emit_time = time.monotonic()
    emit_period_sec = 1.0 / args.consumer_hz

    logging.info("Consumer attached to %s and emitting at %.2f Hz", args.shm_name, args.consumer_hz)

    while not stop_event.is_set():
        status = consumer.read_into(frame_buffer)
        if status == shm.RunResult.Timeout:
            shm.StreamingControl.instance().reset()
            continue
        if status != shm.RunResult.Success:
            logging.info("Consumer exiting with status %s", status)
            return
        if frame_buffer.frame_id == last_frame_id:
            continue

        last_frame_id = frame_buffer.frame_id
        now = time.monotonic()
        if now < next_emit_time:
            continue

        log_frame(rr, "consumer/image", frame_buffer)
        next_emit_time = now + emit_period_sec


def monitor_rerun_main(args: argparse.Namespace, stop_event: mp.synchronize.Event) -> None:
    """Passively monitor the producer-written stream and publish to a separate Rerun server."""
    configure_logging()
    rr = initialize_rerun("shared_memory_monitor_rerun", args.monitor_grpc_port)

    try:
        monitor = wait_for_valid_endpoint(shm.Monitor, args.shm_name, "monitor")
    except RuntimeError as error:
        logging.error("%s", error)
        return

    frame_buffer = shm.CameraFrameBuffer()
    last_frame_id = -1
    logging.info("Monitor attached to %s without throttling", args.shm_name)

    while not stop_event.is_set():
        if not monitor.read_into(frame_buffer, timeout_sec=args.monitor_timeout_sec):
            continue
        if frame_buffer.frame_id == last_frame_id:
            continue

        last_frame_id = frame_buffer.frame_id
        log_frame(rr, "monitor/image", frame_buffer)


def stop_children(stop_event: mp.synchronize.Event) -> None:
    """Request a clean stop across child processes."""
    stop_event.set()
    shm.StreamingControl.instance().stop()


def parse_args() -> argparse.Namespace:
    """Parse command line arguments."""
    parser = argparse.ArgumentParser(description="Fan out shared-memory video to consumer and monitor Rerun servers.")
    parser.add_argument("video_path", help="Path to a video file readable by OpenCV")
    parser.add_argument("--shm-name", default="rerun_fanout_demo", help="Shared-memory segment name")
    parser.add_argument("--consumer-grpc-port", type=int, default=9876, help="Rerun gRPC port for consumer output")
    parser.add_argument("--monitor-grpc-port", type=int, default=9877, help="Rerun gRPC port for monitor output")
    parser.add_argument("--consumer-hz", type=float, default=5.0, help="Consumer-side Rerun logging rate")
    parser.add_argument(
        "--monitor-timeout-sec",
        type=float,
        default=0.05,
        help="Wait budget for each passive monitor read attempt",
    )
    parser.add_argument("--loop", action="store_true", help="Loop the source video forever")
    return parser.parse_args()


def main() -> None:
    """Launch the producer, consumer, and monitor processes."""
    args = parse_args()
    configure_logging()

    mp.set_start_method("spawn", force=True)
    stop_event = mp.Event()

    def handle_signal(signum: int, _frame: object) -> None:
        logging.info("Received signal %s, stopping children", signum)
        stop_children(stop_event)

    signal.signal(signal.SIGINT, handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)

    workers = [
        mp.Process(target=producer_main, name="producer", args=(args, stop_event)),
        mp.Process(target=consumer_rerun_main, name="consumer-rerun", args=(args, stop_event)),
        mp.Process(target=monitor_rerun_main, name="monitor-rerun", args=(args, stop_event)),
    ]

    for worker in workers:
        worker.start()

    logging.info(
        "Connect viewers to rerun+http://127.0.0.1:%d/proxy (consumer) and rerun+http://127.0.0.1:%d/proxy (monitor)",
        args.consumer_grpc_port,
        args.monitor_grpc_port,
    )

    try:
        while True:
            time.sleep(0.2)
            exited_worker = next((worker for worker in workers if worker.exitcode is not None), None)
            if exited_worker is None:
                continue

            logging.info("Worker %s exited with code %s, stopping the rest", exited_worker.name, exited_worker.exitcode)
            break
    except KeyboardInterrupt:
        logging.info("Keyboard interrupt received, stopping children")
    finally:
        stop_children(stop_event)
        for worker in workers:
            worker.join(timeout=5.0)
            if worker.is_alive():
                worker.terminate()
                worker.join(timeout=1.0)
        shm.destroy_shared_memory(args.shm_name)


if __name__ == "__main__":
    main()
