# AGENTS.md

This file captures repo-specific working rules for agent-driven changes in `samosa`.

## Scope

These instructions apply to the whole repository unless a deeper `AGENTS.md` overrides them for a subdirectory.

## Environment

* Run all `bazel` commands inside the devcontainer, not on the host.
* Run code execution that depends on the repo toolchain or Linux kernel behavior inside the devcontainer.
* **Check if you are already in a devcontainer**: If you are already running inside the container (e.g., `test -f /.dockerenv` is true), run commands directly. Do not use `docker exec` from within the container.
* The expected live container name is `samosa-dev`.
* Preferred pattern (if on host):

```bash
docker exec -it samosa-dev bazel test //path/to:target
```

* For an interactive shell (if on host):

```bash
docker exec -it samosa-dev bash
```

* If the container is stopped (if on host), start it with:

```bash
docker start samosa-dev
```

Refer to [README.md](./README.md) for the canonical devcontainer setup.

## Platform Support

* Supported runtime/build targets are Linux only.
* Supported architectures are `amd64` and `aarch64`.
* Do not add macOS compatibility fixes unless explicitly requested.

## Build And Verification

* Before finishing a code change, run the smallest relevant Bazel test target(s) inside the devcontainer.
* For broader validation, use `bazel run check -- all` (prepending `docker exec -it samosa-dev` only if on host).

* If a change touches concurrency, shared memory, synchronization, or hot paths, prefer targeted tests first and then run broader verification when practical.

## Shared Memory Work

* Preserve the non-blocking producer design.
* Do not introduce locks into the shared-memory streaming path unless explicitly requested.
* Treat producer/consumer public behavior as stable by default.
* Keep monitor-style tooling passive unless the task explicitly says otherwise.

## Editing Rules

* Keep changes minimal and local to the task.
* Add or update tests for behavior changes.
* Do not make durable policy changes in this file unless asked.

## Documentation Style

* Write README content, doc comments, and explanatory comments for a new developer or user who does not have prior context.
* Prefer standalone explanations over change-history framing. Do not describe behavior as "old vs new" unless historical context is explicitly requested.
* Explain the current contract, intended use, guarantees, and non-guarantees directly.
* When describing modes or options, optimize for user intent and decision-making first: what it is for, when to use it, and what tradeoff it makes.
* Prefer clear, concrete language over implementation jargon when writing user-facing docs.
* Comments in code should explain why or clarify non-obvious behavior, not narrate trivial mechanics.
* When a concept is easier to understand visually, small diagrams are welcome, especially in READMEs and developer-facing docs.
