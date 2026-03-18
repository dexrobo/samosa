"""Benchmark for shared memory camera Python bindings using multiprocessing."""

import argparse
import logging
import multiprocessing
import multiprocessing.queues
import statistics
import threading
import time
from typing import NamedTuple

import psutil

import dex.vision.shared_memory as shm

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")
logger = logging.getLogger("benchmark")


class BenchmarkStats(NamedTuple):
    """Statistics from a benchmark run."""

    fps: float
    avg_latency: float
    min_latency: float
    median_latency: float
    max_latency: float
    p95_latency: float
    count: int
    peak_rss_mb: float


class MemorySummary(NamedTuple):
    """Summary of memory usage over time."""

    initial_rss_mb: float
    final_rss_mb: float
    peak_rss_mb: float
    climb_mb: float


def run_producer(shm_name: str, frequency: float, num_frames: int, done_event: multiprocessing.Event) -> None:
    """Run a producer at a fixed frequency in a separate process."""
    # Reset streaming control in this process
    shm.StreamingControl.instance().reset()

    producer = shm.Producer(shm_name)
    if not producer.is_valid():
        logger.error("[Producer] Failed to connect")
        done_event.set()
        return

    frame = shm.CameraFrameBuffer()
    frame.color_width = 1920
    frame.color_height = 1080
    frame.color_image_size = 1920 * 1080 * 3

    period = 1.0 / frequency
    logger.info("[Producer] Starting at %.1fHz", frequency)

    for i in range(num_frames):
        start = time.perf_counter()
        frame.frame_id = i
        frame.timestamp_nanos = time.time_ns()

        producer.write(frame)

        elapsed = time.perf_counter() - start
        if elapsed < period:
            time.sleep(period - elapsed)

    logger.info("[Producer] Finished main frames, pulsing to wake consumer")
    # Send a few more pulses to ensure consumer wakes up if it was waiting
    for i in range(5):
        frame.frame_id = num_frames + i
        producer.write(frame)
        time.sleep(0.1)

    done_event.set()
    logger.info("[Producer] Finished")


def run_consumer(
    shm_name: str,
    num_frames: int,
    warmup_frames: int,
    result_queue: multiprocessing.Queue,
    done_event: multiprocessing.Event,
) -> None:
    """Run a consumer and measure latency/throughput in a separate process."""
    # Reset streaming control in this process
    shm.StreamingControl.instance().reset()

    consumer = shm.Consumer(shm_name)
    if not consumer.is_valid():
        logger.error("[Consumer] Failed to connect")
        return

    latencies_ms = []
    timestamps = []

    logger.info("[Consumer] Running until Producer finishes or %d received...", num_frames)

    frame = shm.CameraFrameBuffer()
    received_count = 0
    while not (done_event.is_set() and received_count >= num_frames):
        # 1s timeout in C++
        if consumer.read_into(frame):
            now_ns = time.time_ns()
            latency_ms = (now_ns - frame.timestamp_nanos) / 1e6
            latencies_ms.append(latency_ms)
            timestamps.append(now_ns / 1e9)
            received_count += 1

            if received_count % 100 == 0:
                logger.info("[Consumer] Received %d frames...", received_count)
        elif not shm.StreamingControl.instance().is_running():
            logger.warning("[Consumer] Streaming control stopped.")
            break

        if done_event.is_set() and received_count > 0:
            # Check if any more frames are coming, otherwise break
            time.sleep(0.1)
            if not consumer.read_into(frame):
                break

    # Calculate peak memory usage for this process
    process = psutil.Process()
    peak_rss_mb = process.memory_info().rss / (1024 * 1024)

    # Filter out warmup frames
    if len(latencies_ms) > warmup_frames + 1:
        valid_latencies = latencies_ms[warmup_frames:]
        valid_timestamps = timestamps[warmup_frames:]

        total_time = valid_timestamps[-1] - valid_timestamps[0]
        fps = (len(valid_timestamps) - 1) / total_time
        avg_latency = statistics.mean(valid_latencies)
        min_latency = min(valid_latencies)
        median_latency = statistics.median(valid_latencies)
        max_latency = max(valid_latencies)
        p95_latency = statistics.quantiles(valid_latencies, n=20)[18]

        result_queue.put(
            BenchmarkStats(
                fps=fps,
                avg_latency=avg_latency,
                min_latency=min_latency,
                median_latency=median_latency,
                max_latency=max_latency,
                p95_latency=p95_latency,
                count=len(valid_latencies),
                peak_rss_mb=peak_rss_mb,
            )
        )
    else:
        result_queue.put(None)


def track_memory(pid_list: list[int], stop_event: threading.Event, interval: float = 0.1) -> MemorySummary:
    """Track memory usage across a set of PIDs over time."""
    initial_rss_mb = 0.0
    final_rss_mb = 0.0
    peak_rss_mb = 0.0

    try:
        processes = [psutil.Process(pid) for pid in pid_list]

        # Initial sample
        for proc in processes:
            try:
                initial_rss_mb += proc.memory_info().rss
            except (psutil.NoSuchProcess, psutil.AccessDenied):
                continue
        initial_rss_mb /= 1024 * 1024
        peak_rss_mb = initial_rss_mb

        while not stop_event.is_set():
            current_total_rss = 0.0
            for proc in processes:
                try:
                    current_total_rss += proc.memory_info().rss
                except (psutil.NoSuchProcess, psutil.AccessDenied):
                    continue
            current_mb = current_total_rss / (1024 * 1024)
            peak_rss_mb = max(peak_rss_mb, current_mb)
            final_rss_mb = current_mb
            time.sleep(interval)

    except Exception:
        logger.exception("Error tracking memory")

    return MemorySummary(
        initial_rss_mb=initial_rss_mb,
        final_rss_mb=final_rss_mb,
        peak_rss_mb=peak_rss_mb,
        climb_mb=final_rss_mb - initial_rss_mb,
    )


def print_stats(
    stats: BenchmarkStats, warmup: int, mem_summary: MemorySummary, stable_threshold: float, leak_threshold: float
) -> None:
    """Print the benchmark results."""
    logger.info("Benchmark Results (after %d warmup frames):", warmup)
    logger.info("  Throughput:     %.2f FPS", stats.fps)
    logger.info("  Min Latency:    %.3f ms", stats.min_latency)
    logger.info("  Avg Latency:    %.3f ms", stats.avg_latency)
    logger.info("  Median Latency: %.3f ms", stats.median_latency)
    logger.info("  Max Latency:    %.3f ms", stats.max_latency)
    logger.info("  P95 Latency:    %.3f ms", stats.p95_latency)
    logger.info("  Total Samples:  %d", stats.count)
    logger.info("Memory Stability Analysis:")
    logger.info("  Initial RSS:    %.2f MB", mem_summary.initial_rss_mb)
    logger.info("  Final RSS:      %.2f MB", mem_summary.final_rss_mb)
    logger.info("  Peak RSS:       %.2f MB", mem_summary.peak_rss_mb)
    logger.info("  Memory Climb:   %+.2f MB", mem_summary.climb_mb)

    if abs(mem_summary.climb_mb) < stable_threshold:
        logger.info("  Status:         Memory is STABLE (<%gMB climb)", stable_threshold)
    elif mem_summary.climb_mb > leak_threshold:
        logger.warning("  Status:         Memory is CLIMBING (>%gMB climb) - Potential Leak?", leak_threshold)
    elif mem_summary.climb_mb < 0:
        logger.info("  Status:         Memory DECREASED (%.2f MB) - Stabilization", mem_summary.climb_mb)
    else:
        logger.info("  Status:         Memory usage increased slightly (%.2f MB)", mem_summary.climb_mb)


def main() -> None:
    """Run the benchmark."""
    parser = argparse.ArgumentParser(description="Benchmark shared memory camera bindings")
    parser.add_argument("--shm_name", type=str, default="shm_benchmark", help="Shared memory segment name")
    parser.add_argument("--frequency", type=float, default=120.0, help="Producer frequency in Hz")
    parser.add_argument("--frames", type=int, default=2000, help="Number of frames to benchmark")
    parser.add_argument("--warmup", type=int, default=200, help="Number of warmup frames to discard")
    parser.add_argument("--stable_threshold", type=float, default=1.0, help="Threshold (MB) to consider memory stable")
    parser.add_argument("--leak_threshold", type=float, default=5.0, help="Threshold (MB) to consider memory leaking")
    args = parser.parse_args()

    shm.destroy_shared_memory(args.shm_name)
    if not shm.initialize_shared_memory(args.shm_name):
        logger.error("Failed to initialize shared memory")
        return

    result_queue: multiprocessing.Queue = multiprocessing.Queue()
    done_event = multiprocessing.Event()
    p_prod = multiprocessing.Process(
        target=run_producer, args=(args.shm_name, args.frequency, args.frames, done_event), daemon=True
    )
    p_cons = multiprocessing.Process(
        target=run_consumer, args=(args.shm_name, args.frames, args.warmup, result_queue, done_event), daemon=True
    )

    logger.info("Starting Benchmark: %.1fHz, %d frames", args.frequency, args.frames)
    p_prod.start()
    time.sleep(0.5)
    p_cons.start()

    memory_stop_event = threading.Event()
    # Container to hold the summary object returned by the thread
    mem_summary_container: list[MemorySummary] = []

    def memory_thread_func() -> None:
        pids = []
        if p_prod.pid:
            pids.append(p_prod.pid)
        if p_cons.pid:
            pids.append(p_cons.pid)
        summary = track_memory(pids, memory_stop_event)
        mem_summary_container.append(summary)

    mem_thread = threading.Thread(target=memory_thread_func)
    mem_thread.start()

    p_prod.join(timeout=60)
    p_cons.join(timeout=60)
    memory_stop_event.set()
    mem_thread.join()

    if p_prod.is_alive():
        p_prod.terminate()
    if p_cons.is_alive():
        p_cons.terminate()

    try:
        stats = result_queue.get(timeout=1)
        if stats and mem_summary_container:
            print_stats(stats, args.warmup, mem_summary_container[0], args.stable_threshold, args.leak_threshold)
        else:
            logger.error("Benchmark failed: Insufficient frames received.")
    except multiprocessing.queues.Empty:
        logger.exception("Benchmark failed: Result queue empty.")

    shm.destroy_shared_memory(args.shm_name)


if __name__ == "__main__":
    main()
