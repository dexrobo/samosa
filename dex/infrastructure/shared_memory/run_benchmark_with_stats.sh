#!/bin/bash

# Check if two arguments are provided
if [ "$#" -ne 2 ]; then
    echo "Usage: $0 <benchmark_bin> <formatter_bin>"
    exit 1
fi

BENCHMARK_BIN="$1"
FORMATTER_BIN="$2"

# Run the benchmark (it will create benchmark_results.csv)
"${BENCHMARK_BIN}"

# Run the formatter on the CSV file
"${FORMATTER_BIN}" "benchmark_results.csv"

# Clean up
rm benchmark_results.csv
