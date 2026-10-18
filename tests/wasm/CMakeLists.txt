# WebAssembly smoke test: links the runtime and GPU plugin as a game would and
# checks static plugin registration. Run: node rexglue-wasm-smoke.js
add_executable(rexglue-wasm-smoke smoke.cpp)
target_link_libraries(rexglue-wasm-smoke PRIVATE
    rexruntime
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,rexgpu-xenos>"
)
target_link_options(rexglue-wasm-smoke PRIVATE
    -sPROXY_TO_PTHREAD
    -sEXIT_RUNTIME
    --profiling-funcs  # keep function names in traps
)
