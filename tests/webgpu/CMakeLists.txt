# WebGPU bring-up tests (docs/WASM.md, phase 5).
#   rexglue-webgpu-device: creates the device and runs a compute shader over a
#     512 MB buffer. The WebAssembly build only compiles and links it against
#     emdawnwebgpu.
#   rexglue-webgpu-presenter: a window drawn by WebGPUPresenter and the WebGPU
#     immediate drawer (ImGui) for a few seconds; saves a presented frame and
#     the guest output as BMPs in the working directory. Native only.
add_executable(rexglue-webgpu-device
    device_test.cpp
    ${REXGLUE_ROOT}/src/ui/webgpu/webgpu_device.cpp
)
target_link_libraries(rexglue-webgpu-device PRIVATE rexcore rex::webgpu)

if(NOT EMSCRIPTEN)
    add_executable(rexglue-webgpu-presenter
        presenter_test.cpp
        ${REXGLUE_ROOT}/src/ui/windowed_app_main_sdl.cpp
    )
    target_link_libraries(rexglue-webgpu-presenter PRIVATE rexruntime rex::webgpu imgui::imgui)
endif()
