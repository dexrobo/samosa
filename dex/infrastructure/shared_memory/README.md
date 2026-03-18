# Shared Memory Streaming

This directory contains Samosa's lock-free shared-memory snapshot streaming primitives:

* `Producer`: writes the next snapshot into shared memory
* `Consumer`: participates in the producer/consumer publication handshake and reads published snapshots
* `Monitor`: passively samples producer-written state for debugging, diagnostics, and observability

The core design goal is that the producer stays non-blocking and the consumer always moves toward the latest usable snapshot.

## Producer / Consumer Model

The normal dataflow is between exactly one producer and one consumer.

* The producer writes POD snapshots into one of two shared-memory slots.
* The consumer uses `read_index` and `write_index` as the ownership and publication handshake.
* The producer and consumer are the only parties that participate in that handshake.

`Monitor` is intentionally outside that flow.

## Monitor Overview

`Monitor` is a passive, best-effort observer.

* It does not write `read_index`.
* It does not write `write_index`.
* It does not reserve buffers.
* It does not add backpressure to the producer or consumer.
* It samples producer-written monitor metadata, not consumer-published state.
* It may skip samples.
* It returns only snapshots that pass monitor-side validation.

That makes `Monitor` useful when you want visibility into what the producer has most recently written, without perturbing the production producer/consumer path.

## What The Monitor Actually Samples

The monitor does **not** read "the snapshot currently published to the consumer."

Instead, it samples:

* `last_written_buffer`
* monitor sequence metadata
* per-slot monitor write metadata

So the monitor answers:

> "What is the latest producer-written snapshot that the monitor can validate right now?"

not:

> "What snapshot is the consumer currently allowed to read?"

Those are often close, but they are intentionally not the same contract.

## Basic Usage

```cpp
#include "dex/infrastructure/shared_memory/shared_memory_monitor.h"

using dex::shared_memory::Monitor;
using dex::shared_memory::MonitorReadMode;

Monitor<Telemetry> monitor{"/telemetry_stream"};

auto latest = monitor.GetLatestBuffer(0.05, MonitorReadMode::WaitForProducerWriteCompletion);
if (latest) {
  Inspect(latest->get());
}
```

You can also run a loop:

```cpp
monitor.Run(
    [](const Telemetry& snapshot, uint64_t sequence) {
      Inspect(snapshot, sequence);
    },
    0.1,
    0,
    MonitorReadMode::WaitForProducerWriteCompletion);
```

## Monitor Read Modes

`MonitorReadMode` controls how aggressive the monitor should be when the producer may be in the middle of a write.

### `SkipDuringProducerWrite`

Use this when:

* you want the monitor to stay conservative
* you do not want to start from an already-active producer write episode
* dropping samples is fine

Behavior:

* if the producer is already writing when the read begins, the monitor returns `nullopt`
* if post-copy validation detects overlap, the monitor discards the sample
* the monitor may retry within the timeout budget, but it will not knowingly accept an overlapped copy

Recommended for:

* dashboards
* periodic health sampling
* observability paths where "skip instead of risk" is the right bias

### `WaitForProducerWriteCompletion`

Use this when:

* you want the strongest validated monitor snapshot
* you can tolerate waiting up to a timeout budget
* you want the reported sequence to correspond to the accepted snapshot

Behavior:

* if the producer is already writing, the monitor waits for monitor metadata to change
* waiting is bounded by `timeout_sec`
* only validated snapshots are returned
* if the timeout expires or validation never succeeds, the read returns `nullopt`

Recommended for:

* debug tooling
* manual inspection utilities
* correctness-oriented diagnostics where latency is less important than avoiding suspicious samples

### `ReadDuringProducerWrite`

Use this when:

* you want the most aggressive best-effort sampling
* you prefer "try now" over waiting
* skipping some samples is acceptable

Behavior:

* the monitor may attempt a copy even while a producer write is in flight
* it still performs post-copy validation
* detected overlap is discarded rather than knowingly returned
* this mode is still lossy and best-effort

Recommended for:

* high-rate telemetry taps
* lightweight debug views
* sampling loops that should keep trying rather than waiting for quiet periods

## Choosing A Mode

Use `SkipDuringProducerWrite` when you want the least intrusive and most conservative sampling behavior.

Use `WaitForProducerWriteCompletion` when you want the strongest validated monitor result and can spend a timeout budget to get it.

Use `ReadDuringProducerWrite` when you want the monitor to stay aggressive and opportunistic, while still rejecting obviously suspicious copies.

If you are unsure, start with `WaitForProducerWriteCompletion`.

## Guarantees

The monitor guarantees:

* passive observation only
* no writes to producer/consumer ownership state
* no locks in the monitor path
* bounded waiting in `WaitForProducerWriteCompletion`
* validated snapshots only
* sequence numbers returned to callbacks match the accepted snapshot

## Non-Guarantees

The monitor intentionally does **not** guarantee:

* delivery of every produced snapshot
* alignment with consumer-visible publication state
* zero waiting
* zero drops
* zero retries

It is a diagnostics primitive, not a second consumer.

## Why The Design Stays Passive

The monitor uses copy-and-validate logic instead of joining the producer/consumer handshake.

That means:

* no buffer reservation
* no ownership transfer
* no changes to consumer semantics
* no producer blocking
* no producer backpressure from the monitor

This is the key tradeoff:

* monitor snapshots are best-effort and may be skipped
* production dataflow stays fast and isolated

## Notes

* The buffer type stored in shared memory must be POD.
* The streaming path assumes exactly one producer.
* For production dataflow, use `Consumer`.
* For passive inspection, use `Monitor`.
