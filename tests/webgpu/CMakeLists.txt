# WebGPU bring-up tests (docs/WASM.md, phase 5).
#   rexglue-webgpu-device: starts the device thread and, through it, runs a
#     compute shader over a 512 MB buffer; in the browser also clears the canvas
#     the thread owns. Same code natively and in the browser (emrun).
#   rexglue-webgpu-shaders: compiles every converted internal WGSL shader
#     (src/graphics/shaders/webgpu_wgsl) on the device, compute shaders into
#     pipelines. Natively, in the browser (emrun) and under node with Dawn
#     (rex wasm dawn). No canvas: a build with OffscreenCanvas support can't
#     start under node, as starting main's pthread transfers #canvas.
#   rexglue-webgpu-xenos-shaders: translates the guest shaders in .xsh shader
#     storage files to WGSL (WgslShaderTranslator) and validates them as
#     modules and render pipelines. Native only (reads files given as
#     arguments).
#   rexglue-webgpu-presenter: a window drawn by WebGPUPresenter and the WebGPU
#     immediate drawer (ImGui) for a few seconds; saves a presented frame and
#     the guest output as BMPs in the working directory. Native only.
add_executable(rexglue-webgpu-device
    device_test.cpp
    ${REXGLUE_ROOT}/src/ui/webgpu/webgpu_device.cpp
    ${REXGLUE_ROOT}/src/ui/webgpu/webgpu_device_thread.cpp
)
target_link_libraries(rexglue-webgpu-device PRIVATE rexcore rex::webgpu)
if(EMSCRIPTEN)
    # main on a pthread, which gets the canvas (the default) and passes it to
    # the device thread; stdout sent back to emrun.
    target_link_options(rexglue-webgpu-device PRIVATE
        -sPROXY_TO_PTHREAD -sOFFSCREENCANVAS_SUPPORT -sEXIT_RUNTIME --emrun
        -sENVIRONMENT=web,worker)
    set_target_properties(rexglue-webgpu-device PROPERTIES SUFFIX ".html")
endif()

add_executable(rexglue-webgpu-shaders
    shaders_test.cpp
    ${REXGLUE_ROOT}/src/ui/webgpu/webgpu_device.cpp
    ${REXGLUE_ROOT}/src/ui/webgpu/webgpu_device_thread.cpp
)
target_link_libraries(rexglue-webgpu-shaders PRIVATE rexcore rex::webgpu)
if(EMSCRIPTEN)
    target_link_options(rexglue-webgpu-shaders PRIVATE
        -sPROXY_TO_PTHREAD -sEXIT_RUNTIME --emrun -sENVIRONMENT=web,worker,node)
    set_target_properties(rexglue-webgpu-shaders PROPERTIES SUFFIX ".html")
endif()

if(NOT EMSCRIPTEN)
    add_executable(rexglue-webgpu-presenter
        presenter_test.cpp
        ${REXGLUE_ROOT}/src/ui/windowed_app_main_sdl.cpp
    )
    target_link_libraries(rexglue-webgpu-presenter PRIVATE rexruntime rex::webgpu imgui::imgui)
endif()

if(NOT EMSCRIPTEN)
    set(REXGLUE_WEBGPU_TEST_GRAPHICS ${REXGLUE_ROOT}/src/graphics)
    add_executable(rexglue-webgpu-xenos-shaders
        xenos_shaders_test.cpp
        ${REXGLUE_WEBGPU_TEST_GRAPHICS}/format/ucode.cpp
        ${REXGLUE_WEBGPU_TEST_GRAPHICS}/pipeline/shader/shader.cpp
        ${REXGLUE_WEBGPU_TEST_GRAPHICS}/pipeline/shader/translator.cpp
        ${REXGLUE_WEBGPU_TEST_GRAPHICS}/pipeline/shader/translator_disasm.cpp
        ${REXGLUE_WEBGPU_TEST_GRAPHICS}/pipeline/shader/wgsl_translator.cpp
        ${REXGLUE_WEBGPU_TEST_GRAPHICS}/pipeline/shader/wgsl_translator_alu.cpp
        ${REXGLUE_ROOT}/src/ui/webgpu/webgpu_device.cpp
        ${REXGLUE_ROOT}/src/ui/webgpu/webgpu_device_thread.cpp
    )
    target_include_directories(rexglue-webgpu-xenos-shaders PRIVATE ${REXGLUE_ROOT}/thirdparty/renderdoc)
    target_link_libraries(rexglue-webgpu-xenos-shaders PRIVATE rexcore rex::webgpu xxHash::xxhash)
endif()
