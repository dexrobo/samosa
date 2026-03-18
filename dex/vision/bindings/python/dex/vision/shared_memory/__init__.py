"""Python bindings for the C++ shared memory infrastructure."""

try:
    from dex.vision.shared_memory.shared_memory_bindings import (
        MAX_HEIGHT,
        MAX_WIDTH,
        CameraFrameBuffer,
        Consumer,
        Producer,
        RunResult,
        StreamingControl,
        destroy_shared_memory,
        initialize_shared_memory,
    )
except ImportError:
    # Fallback for different runfile layouts
    from shared_memory_bindings import (
        MAX_HEIGHT,
        MAX_WIDTH,
        CameraFrameBuffer,
        Consumer,
        Producer,
        RunResult,
        StreamingControl,
        destroy_shared_memory,
        initialize_shared_memory,
    )

__all__ = [
    "MAX_HEIGHT",
    "MAX_WIDTH",
    "CameraFrameBuffer",
    "Consumer",
    "Producer",
    "RunResult",
    "StreamingControl",
    "destroy_shared_memory",
    "initialize_shared_memory",
]
