#!/usr/bin/env python3
"""Format benchmark statistics from detailed per-frame CSV measurements."""

import argparse
import csv
import logging
import math
import sys
from collections import defaultdict
from pathlib import Path
from typing import Any, cast

# Configure logging
logging.basicConfig(level=logging.INFO, format="%(levelname)s: %(message)s")
logger = logging.getLogger(__name__)


def calculate_stats(values: list[float]) -> dict[str, float] | None:
    """
    Calculate basic statistics for a list of values.

    Args:
        values: List of numeric values to analyze.

    Returns:
        Dictionary containing mean, stddev, cv, min, max, median, and percentiles,
        or None if the input list is empty.
    ```
    """
    if not values:
        return None

    n = len(values)
    mean = sum(values) / n
    variance = sum((x - mean) ** 2 for x in values) / n
    stddev = math.sqrt(variance)
    cv = (stddev / mean * 100) if mean != 0 else float("nan")

    sorted_values = sorted(values)

    def get_quantile(q: float) -> float:
        idx = min(int(n * q), n - 1)
        return sorted_values[idx]

    return {
        "mean": mean,
        "stddev": stddev,
        "cv (%)": cv,
        "min": min(values),
        "max": max(values),
        "median": get_quantile(0.5),
        "99th": get_quantile(0.99),
        "99.9th": get_quantile(0.999),
        "99.99th": get_quantile(0.9999),
    }


def format_benchmark_stats(input_file: str) -> None:
    """
    Format benchmark statistics from detailed per-frame CSV measurements.

    Args:
        input_file: Path to the CSV file containing per-frame measurements.
    """
    try:
        data: list[dict[str, float]] = []
        with Path(input_file).open() as f:
            reader = csv.DictReader(f)
            for raw_row in reader:
                # raw_row is dict[str | None, str | None]
                converted_row: dict[str, float] = {}
                for k, v in raw_row.items():
                    if k is not None and v is not None:
                        converted_row[str(k)] = float(v)
                data.append(converted_row)

        if not data:
            logger.warning("No data found in input file: %s", input_file)
            return

        # Group by frame_interval and num_frames
        groups: dict[tuple[int, int], list[dict[str, float]]] = defaultdict(list)
        for row in data:
            key = (int(row["frame_interval"]), int(row["num_frames"]))
            groups[key].append(row)

        for (frame_interval, num_frames), group_data in groups.items():
            total_frames = len(group_data)
            print("\nBenchmark parameters:")
            print(f"  Frame generation interval: {frame_interval:,} us")
            print(f"  Frames per run: {num_frames:,}")
            print(f"  Total frames processed: {total_frames:,}")

            # Calculate frame skip statistics
            skipped = sum(1 for row in group_data if row["frame_id_diff"] > 1)
            non_skipped = total_frames - skipped
            print("\nFrame continuity:")
            print(f"  Non-skipped frames: {non_skipped:,}")
            print(f"  Skipped frames: {skipped:,}")

            metrics = {
                "latency (ns)": [row["latency_ns"] for row in group_data],
                "producer_interval (ns)": [row["producer_interval_ns"] for row in group_data],
                "consumer_interval (ns)": [row["consumer_interval_ns"] for row in group_data],
                "frame_id_diff (frames)": [row["frame_id_diff"] for row in group_data],
            }

            print("-" * 80)
            headers = ["metric", "mean", "stddev", "cv (%)", "min", "max", "median", "99th"]
            fmt_str = (
                f"{headers[0]:<25} {headers[1]:>10} {headers[2]:>10} {headers[3]:>10} "
                f"{headers[4]:>10} {headers[5]:>10} {headers[6]:>10} {headers[7]:>10}"
            )
            print(fmt_str)

            for metric, values in metrics.items():
                s = calculate_stats(values)
                if s:
                    res_str = (
                        f"{metric:<25} {s['mean']:>10.1f} {s['stddev']:>10.1f} {s['cv (%)']:>10.1f} "
                        f"{s['min']:>10.1f} {s['max']:>10.1f} {s['median']:>10.1f} {s['99th']:>10.1f}"
                    )
                    print(res_str)
            print("-" * 80)

    except Exception as e:
        logger.exception("Error processing benchmark data: %s", e)
        sys.exit(1)


def main() -> None:
    """Entry point for the benchmark statistics formatter."""
    parser = argparse.ArgumentParser(description="Format detailed benchmark statistics from per-frame measurements")
    parser.add_argument("input_file", help="Input CSV file with per-frame measurements")
    args = parser.parse_args()

    format_benchmark_stats(args.input_file)


if __name__ == "__main__":
    main()
