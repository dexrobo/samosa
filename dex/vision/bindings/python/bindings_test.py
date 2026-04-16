"""Tests for shared memory camera Python bindings."""

import threading
import time
import unittest

import dex.vision.shared_memory as shm
import numpy as np


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

    def test_monitor_reads_latest_snapshot(self) -> None:
        """Verify that the Python monitor can passively sample the latest producer-written snapshot."""
        shm_name = "test_shm_monitor"

        shm.destroy_shared_memory(shm_name)
        ctrl = shm.StreamingControl.instance()
        ctrl.reset()

        assert shm.initialize_shared_memory(shm_name)

        write_buffer = shm.CameraFrameBuffer()
        write_buffer.frame_id = 456
        write_buffer.color_width = 32
        write_buffer.color_height = 16
        write_buffer.color_image_size = 32 * 16 * 3
        write_buffer.camera_name = "MonitorCamera"
        test_pattern = np.arange(write_buffer.color_image_size, dtype=np.uint8)
        write_buffer.color_image_bytes[: write_buffer.color_image_size] = test_pattern

        producer = shm.Producer(shm_name)
        monitor = shm.Monitor(shm_name)
        assert producer.is_valid()
        assert monitor.is_valid()

        producer.write(write_buffer)

        dst = shm.CameraFrameBuffer()
        found = monitor.read_into(dst, 0.1, shm.MonitorReadMode.WaitForStableSnapshot)
        assert found
        assert dst.frame_id == 456
        assert dst.camera_name == "MonitorCamera"

        dst_pattern = np.array(dst.color_image_bytes[: dst.color_image_size])
        np.testing.assert_array_equal(dst_pattern, test_pattern)

        monitored = monitor.read(0.1, shm.MonitorReadMode.Opportunistic)
        assert monitored is not None
        assert monitored.frame_id == 456

        shm.destroy_shared_memory(shm_name)

    def test_calibration_roundtrip(self) -> None:
        """Verify that per-frame calibration fields survive a Producer -> Monitor roundtrip."""
        shm_name = "test_shm_calibration"

        shm.destroy_shared_memory(shm_name)
        ctrl = shm.StreamingControl.instance()
        ctrl.reset()

        assert shm.initialize_shared_memory(shm_name)

        # Known calibration values
        intrinsics_k = np.array([500.0, 0.0, 320.0, 0.0, 500.0, 240.0, 0.0, 0.0, 1.0])
        intrinsics_d = np.array([0.1, -0.2, 0.001, 0.002, 0.05, 0.0, 0.0, 0.0])
        depth_k = np.array([400.0, 0.0, 320.0, 0.0, 400.0, 240.0, 0.0, 0.0, 1.0])
        depth_d = np.array([-0.05, 0.1, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0])
        cam_to_world = np.array([1.0, 0.0, 0.0, 0.5, 0.0, 1.0, 0.0, -0.3, 0.0, 0.0, 1.0, 1.2])
        depth_to_rgb = np.array([1.0, 0.0, 0.0, 0.015, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0])
        depth_scale = 0.00025
        stereo_baseline = 0.063

        write_buffer = shm.CameraFrameBuffer()
        write_buffer.frame_id = 789
        write_buffer.color_width = 32
        write_buffer.color_height = 16
        write_buffer.color_image_size = 32 * 16 * 3

        # Set calibration fields
        write_buffer.color_intrinsics_k = intrinsics_k
        write_buffer.color_intrinsics_d = intrinsics_d
        write_buffer.depth_intrinsics_k = depth_k
        write_buffer.depth_intrinsics_d = depth_d
        write_buffer.cam_to_world_extrinsics = cam_to_world
        write_buffer.depth_to_rgb_extrinsics = depth_to_rgb
        write_buffer.cam_to_world_extrinsics_set = True
        write_buffer.depth_scale = depth_scale
        write_buffer.stereo_baseline_meters = stereo_baseline

        producer = shm.Producer(shm_name)
        assert producer.is_valid()
        producer.write(write_buffer)

        monitor = shm.Monitor(shm_name)
        assert monitor.is_valid()
        dst = shm.CameraFrameBuffer()
        found = monitor.read_into(dst, 0.1, shm.MonitorReadMode.WaitForStableSnapshot)
        assert found
        assert dst.frame_id == 789

        # Verify all calibration fields survived the roundtrip
        np.testing.assert_array_almost_equal(np.array(dst.color_intrinsics_k), intrinsics_k)
        np.testing.assert_array_almost_equal(np.array(dst.color_intrinsics_d), intrinsics_d)
        np.testing.assert_array_almost_equal(np.array(dst.depth_intrinsics_k), depth_k)
        np.testing.assert_array_almost_equal(np.array(dst.depth_intrinsics_d), depth_d)
        np.testing.assert_array_almost_equal(np.array(dst.cam_to_world_extrinsics), cam_to_world)
        np.testing.assert_array_almost_equal(np.array(dst.depth_to_rgb_extrinsics), depth_to_rgb)
        assert dst.cam_to_world_extrinsics_set is True
        assert abs(dst.depth_scale - depth_scale) < 1e-6
        assert abs(dst.stereo_baseline_meters - stereo_baseline) < 1e-9

        shm.destroy_shared_memory(shm_name)

    def test_extrinsics_set_defaults_false(self) -> None:
        """A fresh CameraFrameBuffer should have cam_to_world_extrinsics_set=False."""
        buffer = shm.CameraFrameBuffer()
        assert buffer.cam_to_world_extrinsics_set is False
        assert buffer.depth_scale == 0.0
        assert buffer.stereo_baseline_meters == 0.0

    def test_format_fields_default_empty(self) -> None:
        """A fresh CameraFrameBuffer should have empty format strings."""
        buffer = shm.CameraFrameBuffer()
        assert buffer.color_format == ""
        assert buffer.depth_format == ""
        assert buffer.color_stereo_right_format == ""

    def test_format_fields_roundtrip(self) -> None:
        """Format strings should survive set/get and Producer -> Monitor roundtrip."""
        shm_name = "test_shm_format"
        shm.destroy_shared_memory(shm_name)
        ctrl = shm.StreamingControl.instance()
        ctrl.reset()
        assert shm.initialize_shared_memory(shm_name)

        write_buffer = shm.CameraFrameBuffer()
        write_buffer.frame_id = 99
        write_buffer.color_width = 32
        write_buffer.color_height = 16
        write_buffer.color_image_size = 32 * 16 * 3
        write_buffer.color_format = "jpeg"
        write_buffer.depth_format = "uint16"
        write_buffer.color_stereo_right_format = "raw"

        # Verify local set/get
        assert write_buffer.color_format == "jpeg"
        assert write_buffer.depth_format == "uint16"
        assert write_buffer.color_stereo_right_format == "raw"

        # Verify roundtrip through shared memory
        producer = shm.Producer(shm_name)
        assert producer.is_valid()
        producer.write(write_buffer)

        monitor = shm.Monitor(shm_name)
        assert monitor.is_valid()
        dst = shm.CameraFrameBuffer()
        found = monitor.read_into(dst, 0.1, shm.MonitorReadMode.WaitForStableSnapshot)
        assert found
        assert dst.color_format == "jpeg"
        assert dst.depth_format == "uint16"
        assert dst.color_stereo_right_format == "raw"

        shm.destroy_shared_memory(shm_name)


if __name__ == "__main__":
    unittest.main()
