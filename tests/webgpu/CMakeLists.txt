# WebGPU device bring-up test (docs/WASM.md, phase 5). Natively it creates the
# device on Dawn and runs a compute shader over a 512 MB buffer:
#   rexglue-webgpu-device
# The WebAssembly build only compiles and links it against emdawnwebgpu.
add_executable(rexglue-webgpu-device
    device_test.cpp
    ${REXGLUE_ROOT}/src/ui/webgpu/webgpu_device.cpp
)
target_link_libraries(rexglue-webgpu-device PRIVATE rexcore rex::webgpu)
