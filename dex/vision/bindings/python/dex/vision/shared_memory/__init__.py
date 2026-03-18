"""Python bindings for the C++ shared memory infrastructure."""

try:
    from dex.vision.shared_memory.shared_memory_bindings import (
        CameraFrameBuffer,
        Consumer,
        Producer,
        StreamingControl,
        destroy_shared_memory,
        initialize_shared_memory,
    )
except ImportError:
    # Fallback for different runfile layouts
    from shared_memory_bindings import (
        CameraFrameBuffer,
        Consumer,
        Producer,
        StreamingControl,
        destroy_shared_memory,
        initialize_shared_memory,
    )

__all__ = [
    "CameraFrameBuffer",
    "Consumer",
    "Producer",
    "StreamingControl",
    "destroy_shared_memory",
    "initialize_shared_memory",
]
