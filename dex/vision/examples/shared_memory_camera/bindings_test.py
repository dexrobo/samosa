"""Tests for shared memory camera Python bindings."""

import time
import unittest

import numpy as np

from dex.vision.examples.shared_memory_camera import shared_memory_camera_bindings as shm


class TestSharedMemoryCameraBindings(unittest.TestCase):
    """Test suite for verifying the shared memory camera bindings."""

    def test_buffer_integrity(self) -> None:
        """Verify that CameraFrameBuffer fields and image data are preserved."""
        # Create a buffer and populate it
        buffer = shm.CameraFrameBuffer()
        buffer.color_width = 640
        buffer.color_height = 480
        buffer.color_image_size = 640 * 480 * 3
        buffer.frame_id = 42
        buffer.timestamp_nanos = 123456789
        buffer.camera_name = "TestCamera"

        # Create some random image data
        rng = np.random.default_rng()
        image_data = rng.integers(0, 255, (buffer.color_image_size,), dtype=np.uint8)
        buffer.color_image_bytes[: buffer.color_image_size] = image_data

        # Verify basic fields
        assert buffer.color_width == 640
        assert buffer.color_height == 480
        assert buffer.frame_id == 42
        assert buffer.timestamp_nanos == 123456789
        assert buffer.camera_name == "TestCamera"

        # Verify image data
        retrieved_data = np.array(buffer.color_image_bytes[: buffer.color_image_size], copy=True)
        np.testing.assert_array_equal(retrieved_data, image_data)

    def test_producer_consumer_roundtrip(self) -> None:
        """Verify that a frame can be written by a producer and read by a consumer."""
        shm_name = "test_shm_roundtrip"

        # Initialize streaming control
        ctrl = shm.StreamingControl.instance()
        ctrl.reset()

        # Initialize shared memory
        assert shm.initialize_shared_memory(shm_name)

        producer = shm.Producer(shm_name)
        consumer = shm.Consumer(shm_name)

        assert producer.is_valid()
        assert consumer.is_valid()

        # Create and write a frame
        write_buffer = shm.CameraFrameBuffer()
        write_buffer.frame_id = 123
        write_buffer.color_width = 100
        write_buffer.color_height = 100
        write_buffer.color_image_size = 100 * 100 * 3

        test_pattern = np.arange(write_buffer.color_image_size, dtype=np.uint8)
        write_buffer.color_image_bytes[: write_buffer.color_image_size] = test_pattern

        # Write the frame
        producer.write(write_buffer)

        # Read the frame back
        # Use frame_id=0 for Consumer.read to wait for ANY new frame initially
        # (or 123 to be specific if we know the count)
        read_buffer = None
        for _ in range(10):
            read_buffer = consumer.read(0)
            if read_buffer is not None:
                break
            time.sleep(0.01)

        assert read_buffer is not None
        assert read_buffer.frame_id == 123
        assert read_buffer.color_width == 100

        read_pattern = np.array(read_buffer.color_image_bytes[: read_buffer.color_image_size])
        np.testing.assert_array_equal(read_pattern, test_pattern)


if __name__ == "__main__":
    unittest.main()
