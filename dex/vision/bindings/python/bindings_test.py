"""Tests for shared memory camera Python bindings."""

import time
import unittest

import numpy as np

import dex.vision.shared_memory as shm


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

        # Cleanup any existing shm
        shm.destroy_shared_memory(shm_name)

        # Initialize streaming control
        ctrl = shm.StreamingControl.instance()
        ctrl.reset()

        # Initialize shared memory
        assert shm.initialize_shared_memory(shm_name)

        # Create a frame to write
        write_buffer = shm.CameraFrameBuffer()
        write_buffer.frame_id = 123
        write_buffer.color_width = 100
        write_buffer.color_height = 100
        write_buffer.color_image_size = 100 * 100 * 3
        test_pattern = np.arange(write_buffer.color_image_size, dtype=np.uint8)
        write_buffer.color_image_bytes[: write_buffer.color_image_size] = test_pattern

        read_results = []

        def run_consumer() -> None:
            consumer = shm.Consumer(shm_name)
            assert consumer.is_valid()
            # Wait for any frame
            status, res = consumer.read()
            if status == shm.RunResult.Success and res is not None:
                read_results.append(res)

        def run_producer() -> None:
            producer = shm.Producer(shm_name)
            assert producer.is_valid()
            # The protocol often needs a few cycles to sync up if started from scratch
            for _ in range(5):
                producer.write(write_buffer)
                time.sleep(0.05)

        import threading

        t_cons = threading.Thread(target=run_consumer)
        t_prod = threading.Thread(target=run_producer)

        t_cons.start()
        time.sleep(0.1)  # Give consumer time to enter wait state
        t_prod.start()

        t_cons.join(timeout=2.0)
        t_prod.join(timeout=2.0)

        assert not t_cons.is_alive(), "Consumer thread hung"
        assert len(read_results) > 0, "Consumer did not receive any frame"

        read_buffer = read_results[0]
        assert read_buffer.frame_id == 123
        assert read_buffer.color_width == 100

        read_pattern = np.array(read_buffer.color_image_bytes[: read_buffer.color_image_size])
        np.testing.assert_array_equal(read_pattern, test_pattern)

        # Final cleanup
        shm.destroy_shared_memory(shm_name)


if __name__ == "__main__":
    unittest.main()
