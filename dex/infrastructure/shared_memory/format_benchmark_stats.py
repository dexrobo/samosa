#!/usr/bin/env python3
"""Format benchmark statistics from detailed per-frame CSV measurements."""

import argparse
import sys
import csv
import math
from collections import defaultdict


def calculate_stats(values):
    if not values:
        return None

    n = len(values)
    mean = sum(values) / n
    variance = sum((x - mean) ** 2 for x in values) / n
    stddev = math.sqrt(variance)
    cv = (stddev / mean * 100) if mean != 0 else float("nan")

    sorted_values = sorted(values)

    def get_quantile(q):
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
    """Format benchmark statistics from detailed per-frame CSV measurements."""
    try:
        data = []
        with open(input_file, "r") as f:
            reader = csv.DictReader(f)
            for row in reader:
                # Convert numeric fields
                processed_row = {k: float(v) for k, v in row.items()}
                data.append(processed_row)

        if not data:
            print("No data found in input file.")
            return

        # Group by frame_interval and num_frames
        groups = defaultdict(list)
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
            headers = ["metric", "mean", "stddev", "cv (%)", "min", "max", "median", "99th", "99.9th"]
            print(
                f"{headers[0]:<25} {headers[1]:>10} {headers[2]:>10} {headers[3]:>10} {headers[4]:>10} {headers[5]:>10} {headers[6]:>10} {headers[7]:>10}"
            )

            for metric, values in metrics.items():
                s = calculate_stats(values)
                if s:
                    print(
                        f"{metric:<25} {s['mean']:>10.1f} {s['stddev']:>10.1f} {s['cv (%)']:>10.1f} {s['min']:>10.1f} {s['max']:>10.1f} {s['median']:>10.1f} {s['99th']:>10.1f}"
                    )
            print("-" * 80)

    except Exception as e:
        print(f"Error processing benchmark data: {e}", file=sys.stderr)
        sys.exit(1)


def main() -> None:
    parser = argparse.ArgumentParser(description="Format detailed benchmark statistics from per-frame measurements")
    parser.add_argument("input_file", help="Input CSV file with per-frame measurements")
    args = parser.parse_args()

    format_benchmark_stats(args.input_file)


if __name__ == "__main__":
    main()
