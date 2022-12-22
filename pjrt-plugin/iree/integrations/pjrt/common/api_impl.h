// Copyright 2022 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_PJRT_PLUGIN_PJRT_COMMON_API_IMPL_H_
#define IREE_PJRT_PLUGIN_PJRT_COMMON_API_IMPL_H_

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "iree/base/status.h"
#include "iree/hal/api.h"
#include "iree/integrations/pjrt/common/compiler.h"
#include "iree/integrations/pjrt/common/platform.h"
#include "iree/modules/hal/module.h"
#include "iree/vm/api.h"
#include "iree/vm/bytecode_module.h"
#include "tensorflow/compiler/xla/pjrt/c/pjrt_c_api.h"
#include "tensorflow/compiler/xla/shape_util.h"

namespace iree::pjrt {

class ClientInstance;
class DeviceInstance;
class ErrorInstance;
class EventInstance;

//===----------------------------------------------------------------------===//
// PJRT_Error wrapper
// PJRT Errors are simple wrappers around an iree_status_t. They are
// infrequently created, so we make some ergonomic concessions (caching
// messages, etc).
//===----------------------------------------------------------------------===//

class ErrorInstance {
 public:
  ErrorInstance(iree_status_t status) : status_(status) {}
  ~ErrorInstance() { iree_status_ignore(status_); }
  static void BindApi(PJRT_Api* api);

  static const ErrorInstance* FromError(const PJRT_Error* error) {
    return reinterpret_cast<const ErrorInstance*>(error);
  }

  iree_status_t status() const { return status_; }
  const std::string& message() const;

 private:
  iree_status_t status_;
  mutable std::string cached_message_;
};

inline PJRT_Error* MakeError(iree_status_t status) {
  if (iree_status_is_ok(status)) {
    return nullptr;
  }
  auto alloced_error = std::make_unique<ErrorInstance>(status);
  return reinterpret_cast<PJRT_Error*>(alloced_error.release());
}

//===----------------------------------------------------------------------===//
// BufferInstance
//===----------------------------------------------------------------------===//

class BufferInstance {
 public:
  BufferInstance(DeviceInstance& device, iree_hal_buffer_view_t* buffer_view)
      : device_(device), buffer_view_(buffer_view) {}
  ~BufferInstance();
  operator PJRT_Buffer*() { return reinterpret_cast<PJRT_Buffer*>(this); }
  static BufferInstance* Unwrap(PJRT_Buffer* buffer) {
    return reinterpret_cast<BufferInstance*>(buffer);
  }
  static void BindApi(PJRT_Api* api);

  iree_hal_buffer_view_t* buffer_view() { return buffer_view_.get(); }
  DeviceInstance& device() { return device_; }
  bool is_deleted() { return false; }
  bool is_on_cpu() {
    // TODO: Plumb through an indication if running on CPU and then implement
    // the hook to get an unsafe pointer (avoids a copy).
    return false;
  }
  iree_status_t GetXlaShape(xla::Shape** out_shape);

  // Gets the required host size in bytes to copy to host.
  iree_status_t GetHostSizeInBytes(iree_host_size_t* host_size);
  iree_status_t CopyToHost(void* dst, iree_host_size_t dst_size,
                           EventInstance** done_event);

 private:
  DeviceInstance& device_;
  iree::vm::ref<iree_hal_buffer_view_t> buffer_view_;  // Owned.
  // Various things require XLA's idea of shapes, layouts, etc.
  // We keep one around for such cases.
  std::optional<xla::Shape> cached_shape_;
};

//===----------------------------------------------------------------------===//
// DeviceInstance
//===----------------------------------------------------------------------===//

class DeviceInstance {
 public:
  DeviceInstance(int client_id, ClientInstance& client,
                 iree_hal_driver_t* driver, iree_hal_device_info_t* info)
      : client_id_(client_id), client_(client), driver_(driver), info_(info) {}
  ~DeviceInstance();
  operator PJRT_Device*() { return reinterpret_cast<PJRT_Device*>(this); }
  static void BindApi(PJRT_Api* api);
  static DeviceInstance* Unwrap(PJRT_Device* device) {
    return reinterpret_cast<DeviceInstance*>(device);
  }

  // Since the PJRT device id is a simple int and the IREE device_id is
  // a pointer-sized value, we just assign a synthetic id. Currently, this
  // is the offset into the devices() array on the client. Will need to be
  // revisited if ever supporting re-scanning (but many things would seem to
  // need updates then).
  int client_id() { return client_id_; }
  iree_hal_device_info_t* info() { return info_; }

  // Not yet implemented but plumbed through.
  bool is_addressable() { return true; }
  int process_index() { return 0; }

  // Copies a host buffer to the device.
  // See PJRT_Client_BufferFromHostBuffer
  iree_status_t HostBufferToDevice(
      const void* data, PJRT_Buffer_Type type, const int64_t* dims,
      size_t num_dims, const int64_t* byte_strides, size_t num_byte_strides,
      PJRT_HostBufferSemantics host_buffer_semantics,
      EventInstance** out_done_with_host_buffer_event,
      BufferInstance** out_buffer);

  iree_status_t GetHalDevice(iree_hal_device_t** out_device);

 private:
  iree_status_t OpenDevice();
  int client_id_;
  ClientInstance& client_;
  iree_hal_driver_t* driver_;  // Owned by client.
  iree::vm::ref<iree_hal_device_t> device_;
  iree_hal_device_info_t* info_;
};

//===----------------------------------------------------------------------===//
// EventInstance
//===----------------------------------------------------------------------===//

class EventInstance {
 public:
  // Default construction is always signalled.
  EventInstance() = default;
  operator PJRT_Event*() { return reinterpret_cast<PJRT_Event*>(this); }
  static void BindApi(PJRT_Api* api);
  static EventInstance* Unwrap(PJRT_Event* exe) {
    return reinterpret_cast<EventInstance*>(exe);
  }

  iree_status_t OnReady(PJRT_Event_OnReadyCallback callback, void* user_arg);
  ErrorInstance* error() { return error_; }
  bool is_ready() { return is_ready_; }

 private:
  ErrorInstance* error_ = nullptr;
  bool is_ready_ = true;
};

//===----------------------------------------------------------------------===//
// ExecutableInstance
//===----------------------------------------------------------------------===//

// An executable loaded on all available devices.
struct LoadedExecutable {
  DeviceInstance* device_instance;
  iree::vm::ref<iree_vm_context_t> vm_context;
  iree::vm::ref<iree_vm_module_t> main_module;
  iree_vm_function_t main_function;
  iree_host_size_t arg_count;
  iree_host_size_t result_count;
};

class ExecutableInstance {
 public:
  ExecutableInstance(ClientInstance& client,
                     std::unique_ptr<CompilerOutput> binary,
                     const std::vector<DeviceInstance*>& addressable_devices)
      : client_(client),
        binary_(std::move(binary)),
        addressable_devices_(addressable_devices) {}
  operator PJRT_Executable*() {
    return reinterpret_cast<PJRT_Executable*>(this);
  }
  static void BindApi(PJRT_Api* api);
  static ExecutableInstance* Unwrap(PJRT_Executable* exe) {
    return reinterpret_cast<ExecutableInstance*>(exe);
  }

  const std::vector<DeviceInstance*>& addressable_devices() {
    return addressable_devices_;
  }

  // Loads all executables to addressable devices.
  iree_status_t LoadAll();

  // Gets one loaded executable that can be used for querying metadata
  // and such.
  iree_status_t GetDefaultLoadedExecutable(LoadedExecutable** out_loaded);

  // Gets the number of outputs.
  iree_status_t GetArgResultCount(iree_host_size_t* out_arg_count,
                                  iree_host_size_t* out_result_count);

  // Executes on a batch of devices. Since this is a complicated call,
  // we just give it the raw C argument struct vs breaking it down.
  iree_status_t BatchExecute(PJRT_Executable_Execute_Args* args);

 private:
  ClientInstance& client_;
  std::unique_ptr<CompilerOutput> binary_;
  std::vector<DeviceInstance*> addressable_devices_;
  std::vector<LoadedExecutable> loaded_executables_;
};

//===----------------------------------------------------------------------===//
// ClientInstance
// The root of the runtime hierarchy, these map to an IREE driver and are
// created against an API.
//===----------------------------------------------------------------------===//

struct ClientInstance {
 public:
  ClientInstance(std::unique_ptr<Platform> platform);
  virtual ~ClientInstance();

  // Binds monomorphic entry-points for the client.
  static void BindApi(PJRT_Api* api);

  static ClientInstance* Unwrap(PJRT_Client* client) {
    return reinterpret_cast<ClientInstance*>(client);
  }

  // Before the client is usable, it must be initialized.
  PJRT_Error* Initialize();

  // Must be defined by concrete subclasses.
  virtual iree_status_t CreateDriver(iree_hal_driver_t** out_driver) = 0;
  Platform& platform() { return *platform_; }
  Logger& logger() { return platform_->logger(); }
  iree_allocator_t host_allocator() { return host_allocator_; }
  const std::vector<DeviceInstance*>& devices() { return devices_; }
  const std::vector<DeviceInstance*>& addressable_devices() {
    return addressable_devices_;
  }
  const std::string& cached_platform_name() { return cached_platform_name_; }
  const std::string& cached_platform_version() {
    return cached_platform_version_;
  }

  iree_vm_instance_t* vm_instance() { return vm_instance_.get(); }

  // Compiles.
  // See TODOs in PJRT_Client_Compile.
  PJRT_Error* Compile(PJRT_Program* program, ExecutableInstance** executable);

  // Populates the list of modules to load into a context for an executable
  // on a device. This can be customized by subclasses. The default
  // implementation constructs a hal module and appends:
  //   {hal_module, main_module}.
  virtual iree_status_t PopulateVMModules(
      std::vector<iree::vm::ref<iree_vm_module_t>>& modules,
      iree_hal_device_t* hal_device,
      iree::vm::ref<iree_vm_module_t>& main_module);

 protected:
  iree_allocator_t host_allocator_;
  std::string cached_platform_name_;
  std::string cached_platform_version_;

 private:
  iree_status_t InitializeCompiler();
  iree_status_t InitializeVM();
  iree_status_t PopulateDevices();

  std::unique_ptr<Platform> platform_;

  // HAL.
  iree_hal_driver_t* driver_ = nullptr;
  iree_hal_device_info_t* device_infos_ = nullptr;
  iree_host_size_t device_info_count_ = 0;
  std::vector<DeviceInstance*> devices_;
  std::vector<DeviceInstance*> addressable_devices_;

  // VM.
  iree::vm::ref<iree_vm_instance_t> vm_instance_;
};

//===----------------------------------------------------------------------===//
// API binding
//===----------------------------------------------------------------------===//

// Binds all monomorphic API members and top-level API struct setup.
void BindMonomorphicApi(PJRT_Api* api);

// Fully binds the PJRT_Api struct for all types. Polymorphic types must be
// specified by template parameters.
template <typename PlatformTy, typename ClientInstanceTy>
static void BindApi(PJRT_Api* api) {
  BindMonomorphicApi(api);

  // Bind polymorphic entry-points.
  api->PJRT_Client_Create = +[](PJRT_Client_Create_Args* args) -> PJRT_Error* {
    auto platform = std::make_unique<PlatformTy>();

    // TODO: Once a client can be created with config, use it to populate
    // platform->config_vars().
    auto status = platform->Initialize();
    if (!iree_status_is_ok(status)) {
      return MakeError(status);
    }

    auto client = std::make_unique<ClientInstanceTy>(std::move(platform));
    auto* error = client->Initialize();
    if (error) return error;

    // Successful return.
    args->client = reinterpret_cast<PJRT_Client*>(client.release());
    return nullptr;
  };
}

}  // namespace iree::pjrt

#endif  // IREE_PJRT_PLUGIN_PJRT_COMMON_API_IMPL_H_
