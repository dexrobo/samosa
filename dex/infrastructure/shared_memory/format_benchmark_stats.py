#!/usr/bin/env python3
"""Format benchmark statistics from detailed per-frame CSV measurements."""

import csv
import json
import logging
import math
import os
import sys
from collections import defaultdict
from pathlib import Path

# Configure logging
logging.basicConfig(level=logging.INFO, format="%(levelname)s: %(message)s")
logger = logging.getLogger(__name__)


def calculate_stats(values: list[float]) -> dict[str, float] | None:
    """Calculate basic statistics for a list of values.

    Args:
        values: List of numeric values to analyze.

    Returns:
        Dictionary containing mean, stddev, cv, min, max, median, and percentiles,
        or None if the input list is empty.

    """
    if not values:
        return None

    n = len(values)
    mean = sum(values) / n
    variance = sum((x - mean) ** 2 for x in values) / n
    stddev = math.sqrt(variance)
    cv = (stddev / mean * 100) if mean != 0 else 0

    sorted_values = sorted(values)
    return {
        "mean": mean,
        "stddev": stddev,
        "cv": cv,
        "min": sorted_values[0],
        "max": sorted_values[-1],
        "median": sorted_values[n // 2],
        "99th": sorted_values[int(n * 0.99)],
    }


def format_benchmark_stats(input_file: str, output_json: str | None = None) -> None:
    """Format benchmark statistics from detailed per-frame CSV measurements.

    Args:
        input_file: Path to the CSV file containing per-frame measurements.
        output_json: Optional path to save the summary statistics as a JSON file.

    """
    workspace = os.environ.get("BUILD_WORKSPACE_DIRECTORY")
    file_path = Path(input_file)
    if workspace and not file_path.is_absolute():
        file_path = Path(workspace) / input_file

    try:
        data: list[dict[str, float]] = []
        if not file_path.exists():
            logger.error("Error processing benchmark data: %s (Not found)", file_path)
            return

        with file_path.open() as f:
            reader = csv.DictReader(f)
            for raw_row in reader:
                converted_row: dict[str, float] = {}
                for k, v in raw_row.items():
                    if k is not None and v is not None:
                        converted_row[str(k)] = float(v)
                data.append(converted_row)

        if not data:
            logger.warning("No data found in input file: %s", input_file)
            return

        # Group data by (frame_interval_us, num_frames)
        groups: dict[tuple[int, int], list[dict[str, float]]] = defaultdict(list)
        for row in data:
            key = (int(row["frame_interval_us"]), int(row["num_frames"]))
            groups[key].append(row)

        all_summaries = []

        for (frame_interval_us, num_frames), group_data in groups.items():
            total_frames = len(group_data)
            logger.info("\nBenchmark parameters:")
            logger.info("  Frame generation interval: %s us", f"{frame_interval_us:,}")
            logger.info("  Frames per run: %s", f"{num_frames:,}")
            logger.info("  Total frames processed: %s", f"{total_frames:,}")

            # Calculate frame skip statistics
            skipped = sum(1 for row in group_data if row["frame_id_diff"] > 1)
            non_skipped = total_frames - skipped
            logger.info("\nFrame continuity:")
            logger.info("  Non-skipped frames: %s", f"{non_skipped:,}")
            logger.info("  Skipped frames: %s", f"{skipped:,}")

            metrics_data = {
                "latency (ns)": [row["latency_ns"] for row in group_data],
                "producer_interval (ns)": [row["producer_interval_ns"] for row in group_data],
                "consumer_interval (ns)": [row["consumer_interval_ns"] for row in group_data],
            }

            logger.info("-" * 80)
            headers = ["metric", "mean", "stddev", "cv (%)", "min", "max", "median", "99th"]
            fmt_str = (
                f"{headers[0]:<25} {headers[1]:>10} {headers[2]:>10} {headers[3]:>10} "
                f"{headers[4]:>10} {headers[5]:>10} {headers[6]:>10} {headers[7]:>10}"
            )
            logger.info(fmt_str)

            summary_item = {
                "parameters": {
                    "frame_interval_us": frame_interval_us,
                    "num_frames_per_run": num_frames,
                    "total_frames": total_frames,
                },
                "continuity": {
                    "non_skipped": non_skipped,
                    "skipped": skipped,
                },
                "metrics": {},
            }

            for metric, values in metrics_data.items():
                s = calculate_stats(values)
                if s:
                    res_str = (
                        f"{metric:<25} {s['mean']:>10.1f} {s['stddev']:>10.1f} {s['cv']:>10.1f} "
                        f"{s['min']:>10.1f} {s['max']:>10.1f} {s['median']:>10.1f} {s['99th']:>10.1f}"
                    )
                    logger.info(res_str)
                    summary_item["metrics"][metric] = s

            logger.info("-" * 80)
            all_summaries.append(summary_item)

        if output_json:
            output_path = Path(output_json)
            if workspace and not output_path.is_absolute():
                output_path = Path(workspace) / output_json

            with output_path.open("w") as f:
                json.dump(all_summaries, f, indent=4)
            logger.info("Benchmark summary saved to: %s", output_path)

    except Exception:
        logger.exception("Error processing benchmark data: %s", input_file)
        sys.exit(1)


def main() -> None:
    """Run the benchmark statistics formatter."""
    import argparse

    parser = argparse.ArgumentParser(description="Format benchmark statistics from per-frame measurements")
    parser.add_argument("input_file", help="Input CSV file with per-frame measurements")
    parser.add_argument("--output_json", help="Path to save the summary statistics as a JSON file")
    args = parser.parse_args()

    format_benchmark_stats(args.input_file, args.output_json)


if __name__ == "__main__":
    main()
