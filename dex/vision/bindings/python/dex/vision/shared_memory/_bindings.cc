#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/pair.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>

#include "dex/drivers/camera/base/types.h"
#include "dex/infrastructure/shared_memory/shared_memory_monitor.h"
#include "dex/infrastructure/shared_memory/shared_memory_streaming.h"

namespace nb = nanobind;

namespace {

constexpr double kDefaultMonitorTimeoutSec = dex::shared_memory::detail::kDefaultMonitorTimeoutSec;

}

NB_MODULE(shared_memory_bindings, module_handle) {
  module_handle.attr("MAX_WIDTH") = dex::camera::kMaxWidth;
  module_handle.attr("MAX_HEIGHT") = dex::camera::kMaxHeight;

  nb::class_<dex::shared_memory::StreamingControl>(module_handle, "StreamingControl")
      .def_static("instance", &dex::shared_memory::StreamingControl::Instance, nb::rv_policy::reference)
      .def("is_running", &dex::shared_memory::StreamingControl::IsRunning)
      .def("stop", &dex::shared_memory::StreamingControl::Stop)
      .def("reset", &dex::shared_memory::StreamingControl::Reset)
      .def("reconfigure_and_reset", [](dex::shared_memory::StreamingControl& control, bool handle_signals) {
        control.ReconfigureAndReset({.handle_signals = handle_signals});
      });

  nb::enum_<dex::shared_memory::RunResult>(module_handle, "RunResult")
      .value("Success", dex::shared_memory::RunResult::Success)
      .value("Stopped", dex::shared_memory::RunResult::Stopped)
      .value("Timeout", dex::shared_memory::RunResult::Timeout)
      .value("Error", dex::shared_memory::RunResult::Error);

  nb::enum_<dex::shared_memory::MonitorReadMode>(module_handle, "MonitorReadMode")
      .value("SkipIfBusy", dex::shared_memory::MonitorReadMode::SkipIfBusy)
      .value("WaitForStableSnapshot", dex::shared_memory::MonitorReadMode::WaitForStableSnapshot)
      .value("Opportunistic", dex::shared_memory::MonitorReadMode::Opportunistic);

  nb::class_<dex::camera::CameraFrameBuffer>(module_handle, "CameraFrameBuffer")
      .def(nb::init<>())
      .def_rw("color_width", &dex::camera::CameraFrameBuffer::color_width)
      .def_rw("color_height", &dex::camera::CameraFrameBuffer::color_height)
      .def_rw("color_image_size", &dex::camera::CameraFrameBuffer::color_image_size)
      .def_rw("depth_width", &dex::camera::CameraFrameBuffer::depth_width)
      .def_rw("depth_height", &dex::camera::CameraFrameBuffer::depth_height)
      .def_rw("depth_image_size", &dex::camera::CameraFrameBuffer::depth_image_size)
      .def_rw("color_stereo_right_width", &dex::camera::CameraFrameBuffer::color_stereo_right_width)
      .def_rw("color_stereo_right_height", &dex::camera::CameraFrameBuffer::color_stereo_right_height)
      .def_rw("color_stereo_right_image_size", &dex::camera::CameraFrameBuffer::color_stereo_right_image_size)
      .def_rw("frame_id", &dex::camera::CameraFrameBuffer::frame_id)
      .def_rw("timestamp_nanos", &dex::camera::CameraFrameBuffer::timestamp_nanos)
      .def_prop_rw(
          "camera_name", [](dex::camera::CameraFrameBuffer& buffer) { return std::string(buffer.camera_name.data()); },
          [](dex::camera::CameraFrameBuffer& buffer, const std::string& name) {
            dex::camera::StringToArray(name, buffer.camera_name);
          })
      .def_prop_rw(
          "serial_number",
          [](dex::camera::CameraFrameBuffer& buffer) { return std::string(buffer.serial_number.data()); },
          [](dex::camera::CameraFrameBuffer& buffer, const std::string& serial_number) {
            dex::camera::StringToArray(serial_number, buffer.serial_number);
          })
      .def_prop_rw(
          "color_image_bytes",
          [](dex::camera::CameraFrameBuffer& buffer) {
            std::array<size_t, 1> shape = {buffer.color_image_bytes.size()};
            // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
            return nb::ndarray<nb::numpy, uint8_t>(reinterpret_cast<uint8_t*>(buffer.color_image_bytes.data()), 1,
                                                   shape.data());
            // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
          },
          [](dex::camera::CameraFrameBuffer& buffer, const nb::ndarray<uint8_t, nb::c_contig>& data) {
            std::memcpy(buffer.color_image_bytes.data(), data.data(),
                        std::min(buffer.color_image_bytes.size(), data.size()));
          })
      .def_prop_rw(
          "depth_image_bytes",
          [](dex::camera::CameraFrameBuffer& buffer) {
            std::array<size_t, 1> shape = {buffer.depth_image_bytes.size()};
            // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
            return nb::ndarray<nb::numpy, uint8_t>(reinterpret_cast<uint8_t*>(buffer.depth_image_bytes.data()), 1,
                                                   shape.data());
            // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
          },
          [](dex::camera::CameraFrameBuffer& buffer, const nb::ndarray<uint8_t, nb::c_contig>& data) {
            std::memcpy(buffer.depth_image_bytes.data(), data.data(),
                        std::min(buffer.depth_image_bytes.size(), data.size()));
          })
      .def_prop_rw(
          "color_stereo_right_image_bytes",
          [](dex::camera::CameraFrameBuffer& buffer) {
            std::array<size_t, 1> shape = {buffer.color_stereo_right_image_bytes.size()};
            // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
            return nb::ndarray<nb::numpy, uint8_t>(
                reinterpret_cast<uint8_t*>(buffer.color_stereo_right_image_bytes.data()), 1, shape.data());
            // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
          },
          [](dex::camera::CameraFrameBuffer& buffer, const nb::ndarray<uint8_t, nb::c_contig>& data) {
            std::memcpy(buffer.color_stereo_right_image_bytes.data(), data.data(),
                        std::min(buffer.color_stereo_right_image_bytes.size(), data.size()));
          })
      // ===== Camera Calibration Fields =====
      .def_prop_rw(
          "color_intrinsics_k",
          [](dex::camera::CameraFrameBuffer& buffer) {
            std::array<size_t, 1> shape = {buffer.color_intrinsics_k.size()};
            return nb::ndarray<nb::numpy, double>(buffer.color_intrinsics_k.data(), 1, shape.data());
          },
          [](dex::camera::CameraFrameBuffer& buffer, const nb::ndarray<double, nb::c_contig>& data) {
            std::memcpy(buffer.color_intrinsics_k.data(), data.data(),
                        std::min(buffer.color_intrinsics_k.size() * sizeof(double), data.size() * sizeof(double)));
          })
      .def_prop_rw(
          "color_intrinsics_d",
          [](dex::camera::CameraFrameBuffer& buffer) {
            std::array<size_t, 1> shape = {buffer.color_intrinsics_d.size()};
            return nb::ndarray<nb::numpy, double>(buffer.color_intrinsics_d.data(), 1, shape.data());
          },
          [](dex::camera::CameraFrameBuffer& buffer, const nb::ndarray<double, nb::c_contig>& data) {
            std::memcpy(buffer.color_intrinsics_d.data(), data.data(),
                        std::min(buffer.color_intrinsics_d.size() * sizeof(double), data.size() * sizeof(double)));
          })
      .def_prop_rw(
          "depth_intrinsics_k",
          [](dex::camera::CameraFrameBuffer& buffer) {
            std::array<size_t, 1> shape = {buffer.depth_intrinsics_k.size()};
            return nb::ndarray<nb::numpy, double>(buffer.depth_intrinsics_k.data(), 1, shape.data());
          },
          [](dex::camera::CameraFrameBuffer& buffer, const nb::ndarray<double, nb::c_contig>& data) {
            std::memcpy(buffer.depth_intrinsics_k.data(), data.data(),
                        std::min(buffer.depth_intrinsics_k.size() * sizeof(double), data.size() * sizeof(double)));
          })
      .def_prop_rw(
          "depth_intrinsics_d",
          [](dex::camera::CameraFrameBuffer& buffer) {
            std::array<size_t, 1> shape = {buffer.depth_intrinsics_d.size()};
            return nb::ndarray<nb::numpy, double>(buffer.depth_intrinsics_d.data(), 1, shape.data());
          },
          [](dex::camera::CameraFrameBuffer& buffer, const nb::ndarray<double, nb::c_contig>& data) {
            std::memcpy(buffer.depth_intrinsics_d.data(), data.data(),
                        std::min(buffer.depth_intrinsics_d.size() * sizeof(double), data.size() * sizeof(double)));
          })
      .def_prop_rw(
          "cam_to_world_extrinsics",
          [](dex::camera::CameraFrameBuffer& buffer) {
            std::array<size_t, 1> shape = {buffer.cam_to_world_extrinsics.size()};
            return nb::ndarray<nb::numpy, double>(buffer.cam_to_world_extrinsics.data(), 1, shape.data());
          },
          [](dex::camera::CameraFrameBuffer& buffer, const nb::ndarray<double, nb::c_contig>& data) {
            std::memcpy(buffer.cam_to_world_extrinsics.data(), data.data(),
                        std::min(buffer.cam_to_world_extrinsics.size() * sizeof(double), data.size() * sizeof(double)));
          })
      .def_prop_rw(
          "depth_to_rgb_extrinsics",
          [](dex::camera::CameraFrameBuffer& buffer) {
            std::array<size_t, 1> shape = {buffer.depth_to_rgb_extrinsics.size()};
            return nb::ndarray<nb::numpy, double>(buffer.depth_to_rgb_extrinsics.data(), 1, shape.data());
          },
          [](dex::camera::CameraFrameBuffer& buffer, const nb::ndarray<double, nb::c_contig>& data) {
            std::memcpy(buffer.depth_to_rgb_extrinsics.data(), data.data(),
                        std::min(buffer.depth_to_rgb_extrinsics.size() * sizeof(double), data.size() * sizeof(double)));
          })
      .def_rw("cam_to_world_extrinsics_set", &dex::camera::CameraFrameBuffer::cam_to_world_extrinsics_set)
      .def_rw("depth_scale", &dex::camera::CameraFrameBuffer::depth_scale)
      .def_rw("stereo_baseline_meters", &dex::camera::CameraFrameBuffer::stereo_baseline_meters);

  using CameraBuffer = dex::camera::CameraFrameBuffer;
  using ShmBuffer = dex::shared_memory::SharedMemory<CameraBuffer, 2, dex::shared_memory::LockFreeSharedMemoryBuffer>;

  module_handle.def("initialize_shared_memory", [](const std::string& name) {
    auto shm = ShmBuffer::Create(name, dex::shared_memory::InitializeBuffer<CameraBuffer>);
    return shm.IsValid();
  });

  module_handle.def("destroy_shared_memory", [](const std::string& name) { return ShmBuffer::Destroy(name); });

  nb::class_<dex::shared_memory::Producer<CameraBuffer>>(module_handle, "Producer")
      .def(nb::init<const std::string&>())
      .def("is_valid", &dex::shared_memory::Producer<CameraBuffer>::IsValid)
      .def(
          "write",
          [](dex::shared_memory::Producer<CameraBuffer>& producer, const CameraBuffer& src) {
            // Use ProduceSingle instead of Run to avoid the infinite loop
            producer.ProduceSingle([&](CameraBuffer& dst, uint32_t) { std::memcpy(&dst, &src, sizeof(CameraBuffer)); });
          },
          nb::call_guard<nb::gil_scoped_release>());

  nb::class_<dex::shared_memory::Consumer<CameraBuffer>>(module_handle, "Consumer")
      .def(nb::init<const std::string&>())
      .def("is_valid", &dex::shared_memory::Consumer<CameraBuffer>::IsValid)
      .def(
          "read",
          [](dex::shared_memory::Consumer<CameraBuffer>& consumer)
              -> std::pair<dex::shared_memory::RunResult, CameraBuffer*> {
            // CameraBuffer is ~20MB, must heap allocate to avoid stack overflow.
            // Using take_ownership policy so nanobind manages the lifecycle.
            auto result = std::make_unique<CameraBuffer>();
            bool found = false;
            const timespec timeout = {.tv_sec = 1, .tv_nsec = 0};
            auto res = consumer.ConsumeSingle(
                [&](const CameraBuffer& src) {
                  std::memcpy(result.get(), &src, sizeof(CameraBuffer));
                  found = true;
                },
                &timeout);
            if (res == dex::shared_memory::RunResult::Success && found) {
              return {res, result.release()};
            }
            return {res, nullptr};
          },
          nb::rv_policy::take_ownership, nb::call_guard<nb::gil_scoped_release>())
      .def(
          "read_into",
          [](dex::shared_memory::Consumer<CameraBuffer>& consumer, CameraBuffer& dst) -> dex::shared_memory::RunResult {
            bool found = false;
            const timespec timeout = {.tv_sec = 1, .tv_nsec = 0};
            auto res = consumer.ConsumeSingle(
                [&](const CameraBuffer& src) {
                  std::memcpy(&dst, &src, sizeof(CameraBuffer));
                  found = true;
                },
                &timeout);
            if (res == dex::shared_memory::RunResult::Success && !found) {
              return dex::shared_memory::RunResult::Stopped;
            }
            return res;
          },
          nb::call_guard<nb::gil_scoped_release>(), nb::arg("dst"));

  nb::class_<dex::shared_memory::Monitor<CameraBuffer>>(module_handle, "Monitor")
      .def(nb::init<const std::string&>())
      .def("is_valid", &dex::shared_memory::Monitor<CameraBuffer>::IsValid)
      .def(
          "read_into",
          [](dex::shared_memory::Monitor<CameraBuffer>& monitor, CameraBuffer& dst, const double timeout_sec,
             const dex::shared_memory::MonitorReadMode read_mode) {
            return monitor.ReadInto(dst, timeout_sec, read_mode);
          },
          "Copy the latest validated snapshot into caller-owned storage. Preferred for repeated reads because it "
          "reuses the destination buffer and avoids per-read allocations.",
          nb::call_guard<nb::gil_scoped_release>(), nb::arg("dst"), nb::arg("timeout_sec") = kDefaultMonitorTimeoutSec,
          nb::arg("read_mode") = dex::shared_memory::MonitorReadMode::WaitForStableSnapshot)
      .def(
          "read",
          [](dex::shared_memory::Monitor<CameraBuffer>& monitor, const double timeout_sec,
             const dex::shared_memory::MonitorReadMode read_mode) -> CameraBuffer* {
            auto result = std::make_unique<CameraBuffer>();
            if (!monitor.ReadInto(*result, timeout_sec, read_mode)) {
              return nullptr;
            }
            return result.release();
          },
          "Convenience wrapper around read_into() that allocates a fresh CameraFrameBuffer for each successful read. "
          "Prefer read_into() in loops and long-running code.",
          nb::rv_policy::take_ownership, nb::call_guard<nb::gil_scoped_release>(),
          nb::arg("timeout_sec") = kDefaultMonitorTimeoutSec,
          nb::arg("read_mode") = dex::shared_memory::MonitorReadMode::WaitForStableSnapshot);
}

