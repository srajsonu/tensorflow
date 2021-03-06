/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#ifndef TENSORFLOW_COMPILER_XLA_PYTHON_LOCAL_CLIENT_H_
#define TENSORFLOW_COMPILER_XLA_PYTHON_LOCAL_CLIENT_H_

#include <string>
#include <vector>

#include "absl/types/span.h"
#include "include/pybind11/pybind11.h"
#include "tensorflow/compiler/xla/client/executable_build_options.h"
#include "tensorflow/compiler/xla/client/local_client.h"
#include "tensorflow/compiler/xla/client/xla_computation.h"
#include "tensorflow/compiler/xla/python/shared_device_buffer.h"
#include "tensorflow/compiler/xla/python/worker_thread.h"
#include "tensorflow/compiler/xla/service/computation_placer.h"
#include "tensorflow/compiler/xla/service/shaped_buffer.h"
#include "tensorflow/compiler/xla/shape.h"
#include "tensorflow/compiler/xla/status.h"
#include "tensorflow/compiler/xla/statusor.h"

namespace xla {

// Registers a 'fn_capsule' as a CPU custom call target.
// 'fn_capsule' is a void* pointer encapsulated in a PyCapsule object, with name
// "xla._CPU_CUSTOM_CALL_TARGET".
Status RegisterCpuCustomCallTarget(const std::string& fn_name,
                                   pybind11::capsule capsule);

class PyLocalClient {
 public:
  // Initializes a local XLA client for `platform_name`. Returns an error if no
  // such platform exists, or if the platform has no visible devices.
  static StatusOr<std::unique_ptr<PyLocalClient>> Get(
      const std::string& platform_name);

  explicit PyLocalClient(LocalClient* client);

  Status TransferToInfeed(const LiteralSlice& literal, int device_ordinal);
  StatusOr<pybind11::object> TransferFromOutfeed(const Shape& shape,
                                                 int device_ordinal);

  int device_count() const { return client_->device_count(); }
  LocalClient* client() const { return client_; }

  tensorflow::thread::ThreadPool* h2d_transfer_pool() {
    return &h2d_transfer_pool_;
  }
  const std::vector<std::unique_ptr<WorkerThread>>& execute_threads() {
    return execute_threads_;
  }

 private:
  LocalClient* client_;
  tensorflow::thread::ThreadPool h2d_transfer_pool_;
  // We use a single worker thread per device, both for simplicity and because
  // it avoids a deadlock in tensorflow::thread::ThreadPool (b/130761212).
  std::vector<std::unique_ptr<WorkerThread>> execute_threads_;
};

// Holds a reference from Python to one or more device buffers.
class PyLocalBuffer {
 public:
  static StatusOr<PyLocalBuffer> FromPython(const pybind11::object& argument,
                                            PyLocalClient* client,
                                            int device_ordinal);

  // Converts multiple (python object, device ordinal) pairs into
  // PyLocalBuffers in parallel.
  static StatusOr<std::vector<PyLocalBuffer>> FromPythonValues(
      const std::vector<std::pair<pybind11::object, int>>& argument,
      PyLocalClient* client);

  PyLocalBuffer() = default;
  PyLocalBuffer(Shape on_host_shape,
                std::shared_ptr<PySharedDeviceBuffer> device_buffer,
                PyLocalClient* client);
  StatusOr<pybind11::object> ToPython() const;
  const Shape& on_host_shape() const { return on_host_shape_; }
  const PySharedDeviceBuffer* device_buffer() const {
    return device_buffer_.get();
  }

  void Delete() {
    device_buffer_ = nullptr;
    client_ = nullptr;
  }

  // Returns a view of the PyLocalBuffer DAG as a ShapedBuffer. The
  // PyLocalBuffer retains ownership of the device buffers.
  ShapedBuffer AsShapedBuffer() const;

  // Destructures a tuple-valued PyLocalBuffer into its constituent elements.
  StatusOr<std::vector<PyLocalBuffer>> DestructureTuple();

 private:
  Shape on_host_shape_;
  std::shared_ptr<PySharedDeviceBuffer> device_buffer_;
  PyLocalClient* client_ = nullptr;
};

// Represents a compiled computation that can be executed given handles to
// device-allocated literals. Wraps an XLA LocalExecutable.
class PyLocalExecutable {
 public:
  // Compiles a computation to an executable.
  static StatusOr<std::unique_ptr<PyLocalExecutable>> Compile(
      const XlaComputation& computation, std::vector<Shape> argument_layouts,
      const ExecutableBuildOptions* build_options, PyLocalClient* client);

  PyLocalExecutable(std::unique_ptr<LocalExecutable> executable,
                    DeviceAssignment device_assignment, PyLocalClient* client);

  int num_replicas() const {
    return executable_->build_options().num_replicas();
  }

  // Returns the device ordinals to which each replica is assigned.
  std::vector<int> DeviceOrdinals() const;

  const DeviceAssignment& device_assignment() const {
    return device_assignment_;
  }

  StatusOr<PyLocalBuffer> Execute(
      absl::Span<PyLocalBuffer* const> argument_handles);

  // Execute on many replicas. Takes a sequence of argument lists (one argument
  // list per replica) and returns a tuple of results (one result per replica).
  // The number of argument lists must be equal to the replica count.
  StatusOr<std::vector<PyLocalBuffer>> ExecutePerReplica(
      absl::Span<const std::vector<PyLocalBuffer*>> argument_handles);

  void Delete() { executable_ = nullptr; }

 private:
  std::unique_ptr<LocalExecutable> executable_;
  const DeviceAssignment device_assignment_;
  PyLocalClient* const client_;
};

}  // namespace xla

#endif  // TENSORFLOW_COMPILER_XLA_PYTHON_LOCAL_CLIENT_H_
