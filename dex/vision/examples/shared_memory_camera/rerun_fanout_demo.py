"""Fan out a video stream to a throttled consumer and one or more source-rate monitors.

This example launches:

1. A producer that streams video frames into shared memory at the source video's frame rate.
2. A normal shared-memory consumer that reads the stream and logs to a Rerun gRPC server at 5 Hz.
3. N passive shared-memory monitors that sample the same stream and log at the full source frame rate.
"""

from __future__ import annotations

import argparse
import importlib
import logging
import multiprocessing as mp
import os
import signal
import sys
import time
import uuid
from pathlib import Path
from typing import TYPE_CHECKING, Protocol

if TYPE_CHECKING:
    from collections.abc import Callable
    from types import ModuleType

import cv2
import dex.vision.shared_memory as shm
import numpy as np

DEFAULT_ATTACH_TIMEOUT_SEC = 5.0
DEFAULT_RERUN_CONNECT_URL = "rerun+http://host.docker.internal:9876/proxy"
DEFAULT_SHARED_RECORDING_APP_ID = "shared_memory_fanout_rerun"
DEFAULT_VIDEO_FPS = 30.0
LOGGER = logging.getLogger("rerun_fanout_demo_py")


class ValidEndpoint(Protocol):
    """Minimal interface shared by Consumer and Monitor bindings."""

    def is_valid(self) -> bool:
        """Return whether the endpoint attached successfully."""


def import_rerun() -> ModuleType:
    """Import rerun from a few layouts used in local and Bazel environments."""
    try:
        return importlib.import_module("rerun")
    except ImportError:
        try:
            return importlib.import_module("rerun_sdk.rerun")
        except ImportError:
            for sys_path in sys.path:
                rerun_sdk_path = Path(sys_path) / "rerun_sdk"
                if not rerun_sdk_path.exists():
                    continue
                sys.path.append(str(rerun_sdk_path))
                rerun_cli_path = rerun_sdk_path / "rerun_cli"
                if rerun_cli_path.exists():
                    os.environ["PATH"] += os.pathsep + str(rerun_cli_path)
                return importlib.import_module("rerun")
            raise


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
    return cv2.resize(frame_rgb, (int(width * scale), int(height * scale)))


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
    LOGGER.warning("Video FPS unavailable, falling back to %.1f FPS for Rerun timing", DEFAULT_VIDEO_FPS)
    return DEFAULT_VIDEO_FPS


def initialize_rerun(
    application_id: str,
    args: argparse.Namespace,
    role: str,
    *,
    send_blueprint: bool,
    grpc_port: int | None = None,
) -> ModuleType:
    """Initialize a Rerun recording stream in either per-process serve or shared connect mode."""
    rr = import_rerun()
    if args.rerun_mode == "connect":
        rr.init(DEFAULT_SHARED_RECORDING_APP_ID, recording_id=args.rerun_recording_id, spawn=False)
    else:
        rr.init(application_id, spawn=False)

    if send_blueprint:
        rrb = importlib.import_module("rerun.blueprint")
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

    if args.rerun_mode == "connect":
        rr.connect_grpc(args.rerun_url)
        LOGGER.info(
            "Rerun %s streaming to %s with shared recording_id=%s",
            role,
            args.rerun_url,
            args.rerun_recording_id,
        )
        return rr

    if grpc_port is None:
        grpc_port = args.consumer_grpc_port
    server_uri = rr.serve_grpc(grpc_port=grpc_port, server_memory_limit="256MB")
    LOGGER.info("Rerun gRPC server for %s listening at %s", application_id, server_uri)
    return rr


def log_frame(rr: ModuleType, entity_path: str, frame_buffer: shm.CameraFrameBuffer) -> None:
    """Log one frame into Rerun."""
    image = frame_to_rgb_image(frame_buffer)
    rr.set_time("video_time", duration=np.timedelta64(frame_buffer.timestamp_nanos, "ns"))
    rr.log(entity_path, rr.Image(image))


def wait_for_valid_endpoint(
    factory: Callable[[str], ValidEndpoint], shm_name: str, role: str, timeout_sec: float = DEFAULT_ATTACH_TIMEOUT_SEC
) -> ValidEndpoint:
    """Retry attaching to shared memory until it becomes valid or the timeout expires."""
    deadline = time.monotonic() + timeout_sec
    while time.monotonic() < deadline:
        endpoint = factory(shm_name)
        if endpoint.is_valid():
            return endpoint
        time.sleep(0.05)

    message = f"{role} could not attach to shared memory segment {shm_name!r} within {timeout_sec:.1f}s"
    raise RuntimeError(message)


def producer_main(args: argparse.Namespace, stop_event: mp.synchronize.Event) -> None:
    """Read a video file and publish it to shared memory at the source frame rate."""
    configure_logging()

    if not shm.initialize_shared_memory(args.shm_name):
        LOGGER.error("Failed to initialize shared memory %s", args.shm_name)
        return

    producer = shm.Producer(args.shm_name)
    if not producer.is_valid():
        LOGGER.error("Failed to create producer for %s", args.shm_name)
        return

    cap = cv2.VideoCapture(args.video_path)
    if not cap.isOpened():
        LOGGER.error("Could not open video %s", args.video_path)
        return

    video_fps = get_video_fps(cap)
    frame_period_sec = 1.0 / video_fps
    buffer = shm.CameraFrameBuffer()
    frame_id = 0
    LOGGER.info(
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
            video_time_nanos = round((frame_id * 1_000_000_000) / video_fps)
            populate_buffer(buffer, frame_rgb, frame_id, video_time_nanos, "RerunFanoutProducer")
            producer.write(buffer)

            if frame_id % 120 == 0:
                LOGGER.info("Produced frame %d", frame_id)
            frame_id += 1

            elapsed = time.monotonic() - frame_start
            remaining = frame_period_sec - elapsed
            if remaining > 0:
                time.sleep(remaining)
    finally:
        cap.release()
        LOGGER.info("Producer stopped at frame %d", frame_id)


def consumer_rerun_main(args: argparse.Namespace, stop_event: mp.synchronize.Event) -> None:
    """Read the stream through the real consumer path and publish to Rerun at 5 Hz."""
    configure_logging()
    rr = initialize_rerun(
        "shared_memory_consumer_rerun",
        args,
        "consumer",
        send_blueprint=True,
        grpc_port=args.consumer_grpc_port,
    )

    try:
        consumer = wait_for_valid_endpoint(shm.Consumer, args.shm_name, "consumer")
    except RuntimeError:
        LOGGER.exception("Consumer could not attach to shared memory")
        return

    frame_buffer = shm.CameraFrameBuffer()
    last_frame_id = -1
    next_emit_time = time.monotonic()
    emit_period_sec = 1.0 / args.consumer_hz

    LOGGER.info("Consumer attached to %s and emitting at %.2f Hz", args.shm_name, args.consumer_hz)

    while not stop_event.is_set():
        status = consumer.read_into(frame_buffer)
        if status == shm.RunResult.Timeout:
            shm.StreamingControl.instance().reset()
            continue
        if status != shm.RunResult.Success:
            LOGGER.info("Consumer exiting with status %s", status)
            return
        if frame_buffer.frame_id == last_frame_id:
            continue

        last_frame_id = frame_buffer.frame_id
        now = time.monotonic()
        if now < next_emit_time:
            continue

        log_frame(rr, "consumer/image", frame_buffer)
        next_emit_time = now + emit_period_sec


def monitor_rerun_main(args: argparse.Namespace, stop_event: mp.synchronize.Event, monitor_index: int) -> None:
    """Passively monitor the producer-written stream and publish to a monitor-specific Rerun output."""
    application_id = f"shared_memory_monitor_rerun_{monitor_index}"
    entity_path = f"monitor_{monitor_index}/image"
    grpc_port = args.monitor_grpc_port + monitor_index
    configure_logging()
    rr = initialize_rerun(
        application_id,
        args,
        "monitor",
        send_blueprint=args.rerun_mode == "serve",
        grpc_port=grpc_port,
    )

    try:
        monitor = wait_for_valid_endpoint(shm.Monitor, args.shm_name, "monitor")
    except RuntimeError:
        LOGGER.exception("Monitor %d could not attach to shared memory", monitor_index)
        return

    frame_buffer = shm.CameraFrameBuffer()
    last_frame_id = -1
    LOGGER.info(
        "Monitor %d attached to %s at source rate%s",
        monitor_index,
        args.shm_name,
        f" via port {grpc_port}" if args.rerun_mode == "serve" else "",
    )

    while not stop_event.is_set():
        if not monitor.read_into(frame_buffer, timeout_sec=args.monitor_timeout_sec):
            continue
        if frame_buffer.frame_id == last_frame_id:
            continue

        last_frame_id = frame_buffer.frame_id
        log_frame(rr, entity_path, frame_buffer)


def stop_children(stop_event: mp.synchronize.Event) -> None:
    """Request a clean stop across child processes."""
    stop_event.set()
    shm.StreamingControl.instance().stop()


def parse_args() -> argparse.Namespace:
    """Parse command line arguments."""
    parser = argparse.ArgumentParser(description="Fan out shared-memory video to consumer and monitor Rerun outputs.")
    parser.add_argument("video_path", help="Path to a video file readable by OpenCV")
    parser.add_argument("--shm-name", default="rerun_fanout_demo", help="Shared-memory segment name")
    parser.add_argument(
        "--rerun-mode",
        choices=("serve", "connect"),
        default="serve",
        help="Serve two local Rerun gRPC endpoints, or connect both streams into one existing Rerun recording.",
    )
    parser.add_argument("--consumer-grpc-port", type=int, default=9876, help="Rerun gRPC port for consumer output")
    parser.add_argument(
        "--monitor-grpc-port",
        type=int,
        default=9877,
        help="Base Rerun gRPC port for monitor outputs in serve mode. Monitor i uses port base+i.",
    )
    parser.add_argument(
        "--num-monitors",
        type=int,
        default=1,
        help="Number of passive monitor processes to launch.",
    )
    parser.add_argument(
        "--rerun-url",
        default=DEFAULT_RERUN_CONNECT_URL,
        help="Rerun gRPC endpoint to connect to when --rerun-mode=connect",
    )
    parser.add_argument(
        "--rerun-recording-id",
        help="Explicit shared Rerun recording id for connect mode. Defaults to a generated UUID.",
    )
    parser.add_argument("--consumer-hz", type=float, default=5.0, help="Consumer-side Rerun logging rate")
    parser.add_argument(
        "--monitor-timeout-sec",
        type=float,
        default=0.05,
        help="Wait budget for each passive monitor read attempt",
    )
    parser.add_argument("--loop", action="store_true", help="Loop the source video forever")
    args = parser.parse_args()
    if args.num_monitors < 0:
        parser.error("--num-monitors must be non-negative")
    if args.rerun_mode == "connect" and not args.rerun_url:
        parser.error("--rerun-url is required when --rerun-mode=connect")
    return args


def main() -> None:
    """Launch the producer, consumer, and monitor processes."""
    args = parse_args()
    configure_logging()
    if args.rerun_mode == "connect" and not args.rerun_recording_id:
        args.rerun_recording_id = f"shared-memory-fanout-{uuid.uuid4()}"

    mp.set_start_method("spawn", force=True)
    stop_event = mp.Event()

    def handle_signal(signum: int, _frame: object) -> None:
        LOGGER.info("Received signal %s, stopping children", signum)
        stop_children(stop_event)

    signal.signal(signal.SIGINT, handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)

    workers = [
        mp.Process(target=producer_main, name="producer", args=(args, stop_event)),
        mp.Process(target=consumer_rerun_main, name="consumer-rerun", args=(args, stop_event)),
    ]
    for monitor_index in range(args.num_monitors):
        workers.append(
            mp.Process(
                target=monitor_rerun_main,
                name=f"monitor-rerun-{monitor_index}",
                args=(args, stop_event, monitor_index),
            )
        )

    for worker in workers:
        worker.start()

    if args.rerun_mode == "serve":
        monitor_ports = [args.monitor_grpc_port + monitor_index for monitor_index in range(args.num_monitors)]
        LOGGER.info("Connect consumer viewer to rerun+http://127.0.0.1:%d/proxy", args.consumer_grpc_port)
        if monitor_ports:
            LOGGER.info(
                "Connect monitor viewers to: %s",
                ", ".join(f"rerun+http://127.0.0.1:{port}/proxy" for port in monitor_ports),
            )
        else:
            LOGGER.info("No monitor viewers requested (--num-monitors=0)")
    else:
        LOGGER.info(
            "Consumer and %d monitor(s) will stream into %s with shared recording_id=%s",
            args.num_monitors,
            args.rerun_url,
            args.rerun_recording_id,
        )

    try:
        while True:
            time.sleep(0.2)
            exited_worker = next((worker for worker in workers if worker.exitcode is not None), None)
            if exited_worker is None:
                continue

            LOGGER.info("Worker %s exited with code %s, stopping the rest", exited_worker.name, exited_worker.exitcode)
            break
    except KeyboardInterrupt:
        LOGGER.info("Keyboard interrupt received, stopping children")
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
