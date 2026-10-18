# WebAssembly Target

Work plan for building ReXGlue projects for the browser with Emscripten. The `rexglue` recompiler stays a native tool; the new target is the runtime and the C++ it generates.

Emscripten is Clang-based, so the existing Clang 18+ and C++23 requirements hold.

## Status

Last updated 2026-10-18, on the `wasm` branch at `1035cae`.

| Phase | State |
| --- | --- |
| 1. Build plumbing and null GPU | Done |
| 2. Platform layer | Memory, faults, threads, clock and node hosting done; fibers partial; the first run of a real title is next |
| 3. Codegen wasm mode | Mostly covered by the templates; size limits not measured yet |
| 4. GPU write tracking | Not started |
| 5. WebGPU backend | Not started |
| 6. Packaging and hosting | Not started |

Where things stand:

- The SDK (runtime, null GPU plugin, third-party libraries) builds for wasm64, and the smoke test passes under node.
- Burnout Crash (`../Crash`) has been regenerated with the current templates. Its main executable's generated code and the SDK compile for wasm64, but the build never finished on Windows ARM64: the last 6 objects (the DLL module's biggest generated files) were still compiling after hours under the x64-emulated toolchain. Nothing has run under node yet.
- Phase 2's milestone was moved to **node first** (headless, host filesystem) instead of a browser tab: it proves memory, threads and kernel startup without data import, COOP/COEP hosting or a canvas, which move to phase 6.

### Next steps

1. Finish the Crash wasm build on an x86-64 machine, preferably Linux (see [Building](#building)).
2. Run it under node and fix what stops it before kernel startup (registering `dll/crash.dll.xex` and on). This is also the first test of static DLL module registration.
3. Check CI on `1035cae`. On `4df853d` the wasm64 build passed but the smoke test failed, most likely the `CLOCK_MONOTONIC_RAW` divide-by-zero fixed in `1035cae`.
4. The lint job fails on 10 files that were unformatted before this branch: `src/graphics/d3d12/pipeline_cache.cpp`, `src/graphics/graphics_system.cpp`, `src/graphics/pipeline/shader/dxbc_translator.cpp`, `src/graphics/pipeline/shader/dxbc_translator_memexport.cpp`, `src/graphics/pipeline/shader/spirv_translator_fetch.cpp`, `src/kernel/xam/xam_content.cpp`, `src/kernel/xam/xam_info.cpp`, `src/system/lzx.cpp`, `src/system/xam/content_manager.cpp`, `src/system/xsocket.cpp`. Running `clang-format -i` (version 20.1.8, `scripts/requirements-dev.txt`) on them fixes it.
5. Choose JSPI or Asyncify for fibers once a title needs guest fibers.

## Building

### Toolchain

Emscripten **6.0.12** (pinned in CI, [build-wasm64.yaml](../.github/workflows/build-wasm64.yaml)):

```bash
git clone https://github.com/emscripten-core/emsdk.git
emsdk/emsdk install 6.0.12
emsdk/emsdk activate 6.0.12
source emsdk/emsdk_env.sh     # emsdk_env.bat / emsdk_env.ps1 on Windows
```

emsdk has no Windows ARM64 binaries: set `EMSDK_ARCH=x86_64` before every emsdk command to use the x64 ones under emulation. That works for the SDK but is far too slow for a game's generated code; use an x86-64 machine (or Linux ARM64) for games. CMake 3.25+ and Ninja are needed as usual.

### The SDK

```bash
cmake --preset wasm64
cmake --build out/build/wasm64 --config Release --parallel
node out/wasm64/Release/rexglue-wasm-smoke.js
```

The smoke test ([tests/wasm](../tests/wasm)) creates the null GPU through the statically linked plugin and checks the guest memory aliasing.

### A game

A game consumes the SDK in-tree (`REXSDK_DIR`); a packaged wasm SDK doesn't exist yet. Codegen still runs during the build, through a native `rexglue` built from the same SDK revision (found on `PATH`, or `REXGLUE_HOST_REXGLUE`). Regenerate once with it first if the templates changed (`rexglue codegen <game>_manifest.toml` in the game folder).

The game's own presets don't know about wasm, so configure by hand (shown for Crash; adjust paths):

```bash
FLAGS="-sMEMORY64 -pthread -msimd128 -fwasm-exceptions -sSUPPORT_LONGJMP=wasm"
cmake -S Crash -B Crash/out/build/wasm64 -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$EMSDK/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake" \
  -DEMSCRIPTEN_SYSTEM_PROCESSOR=wasm64 \
  -DCMAKE_C_FLAGS="$FLAGS" -DCMAKE_CXX_FLAGS="$FLAGS" -DCMAKE_EXE_LINKER_FLAGS="$FLAGS" \
  -DREXSDK_DIR=/path/to/rexglue-sdk \
  -DREXGLUE_HOST_REXGLUE=/path/to/native/rexglue \
  -DREXGLUE_USE_VULKAN=OFF -DREXGLUE_ENABLE_TRACY=OFF -DREXGLUE_ENABLE_FIDELITYFX=OFF
cmake --build Crash/out/build/wasm64 --parallel
```

### Running under node

```bash
node crash.js --gpu_plugin=xenos --gpu_backend=null --video_driver=dummy \
  --launcher_menu=false --game_data_root=/path/to/Crash/game --log_level=debug
```

Untested so far. `REXGLUE_WASM_NODE` (on by default) links host apps with `-sENVIRONMENT=node -sNODERAWFS`, so the game folder is read straight from the host filesystem; Windows-style paths are untested under it. `null_gpu_sample_dump_dir=<dir>` writes the null GPU's 1 Hz wireframe frames as BMPs. Audio falls back to nothing if SDL finds no device.

## Design notes

### Guest memory layout

[memory/guest_layout.h](../include/rex/memory/guest_layout.h). Linear memory can't be mapped twice, so the guest space is one 4.6 GB block: guest virtual addresses 1:1 in the first 4 GB, then physical memory (+4 KB). Aliased ranges are redirected with a 256-entry table on the address's top byte (every boundary is 16 MB aligned), so a guest address lives at `block + address + GuestAliasHostOffset(address)`:

| Guest range | Lands at |
| --- | --- |
| `0x7F000000`–`0x7FFFFFFF` (GPU writeback) | physical + 0 |
| `0xA0000000`–`0xBFFFFFFF` | physical + 0 |
| `0xC0000000`–`0xDFFFFFFF` | physical + 0 |
| `0xE0000000`–`0xFFFFFFFF` | physical + 0x1000 |

The table backs `detail::PhysicalHostOffset` (and so `GuestPtr`), `Memory::TranslateVirtual`, `REX_PHYS_HOST_OFFSET` in generated code, and the base pointers of the `0xA0000000`/`0xC0000000`/`0xE0000000` heaps. `HostToGuestVirtual` reports physical memory through the `0xA0000000` view. One deviation: `0x90000000` keeps its own storage instead of aliasing `0x80000000`, which only matters if a title reaches the same XEX memory through both.

[memory_wasm.cpp](../src/core/memory_wasm.cpp) replaces `memory_posix.cpp`: blocks come from malloc (fresh linear memory is zero), commit is a no-op, decommit zeroes, protection is ignored. `rexruntime` links with `-sALLOW_MEMORY_GROWTH -sMAXIMUM_MEMORY=16GB`.

### Faults and MMIO

Nothing faults on wasm. [exception_handler_wasm.cpp](../src/core/exception_handler_wasm.cpp) accepts handlers and never runs them, and `MMIOHandler`'s instruction decoding reports "unhandled". Instead, on wasm the generated code's plain `REX_LOAD_*`/`REX_STORE_*` macros ([pch_h.inja](../resources/templates/codegen/pch_h.inja)) ask `MMIOHandler` about any address in `0x7F000000`–`0x7FFFFFFF` and fall back to memory when no MMIO range claims it (GPU writeback memory is in that range). One compare per access, wasm only.

### No dynamic loading

`rexruntime` and GPU plugins build as static libraries (`REX_SHARED_LIBRARY_TYPE`). The xenos plugin registers itself at startup (`RegisterStaticGpuPlugin`, [plugin_main.cpp](../src/graphics/plugin_main.cpp)) and `rexglue_configure_target` links it whole-archive. Generated DLL modules become static libraries ([dll_targets_cmake.inja](../resources/templates/codegen/dll_targets_cmake.inja)) whose registration functions are `static` and that call `RegisterStaticModule` ([register_cpp.inja](../resources/templates/codegen/register_cpp.inja)); `KernelState::LoadUserModule` looks modules up by image base before trying `dlopen`, on every platform. Not yet exercised by a real module.

### Threads, fibers, time

- Host apps link with `-sPROXY_TO_PTHREAD -sPTHREAD_POOL_SIZE=32 -sSTACK_SIZE=8MB -sEXIT_RUNTIME` ([rexglue_helpers.cmake](../cmake/rexglue_helpers.cmake)).
- [threading_posix.cpp](../src/core/threading_posix.cpp): thread ids are numbered per thread, names go through `emscripten_set_thread_name`, affinity is a no-op, and user callbacks (APCs) wait for the target's next alertable wait slice, as there are no signals between threads.
- [fiber_wasm.cpp](../src/core/fiber_wasm.cpp): threads can become fibers (every `XThread` does), but creating or switching to guest fibers logs an error and fails until JSPI or Asyncify is chosen.
- FPSCR ([fpscr.h](../include/rex/platform/fpscr.h)) is emulated per thread with the MXCSR layout; nothing enforces rounding or flush-to-zero.
- The clock uses `CLOCK_MONOTONIC` with 1 GHz ticks (Emscripten has no `CLOCK_MONOTONIC_RAW`).

### Build specifics

- `wasm64` preset flags (every object must agree): `-sMEMORY64 -pthread -msimd128 -fwasm-exceptions -sSUPPORT_LONGJMP=wasm`. They're in `CMAKE_<LANG>_FLAGS` because compiler detection already needs `-sMEMORY64`; CMakeLists.txt checks for them.
- FFmpeg uses a plain-C config, [ffmpeg-overlay/config_wasm64.h](../thirdparty/ffmpeg-overlay/config_wasm64.h), kept outside the FFmpeg submodule.
- [hash.h](../include/rex/hash.h) includes `<arm_neon.h>` (Emscripten's SIMDe shim) before xxHash, which would otherwise include it inside `extern "C"`.
- SIMDe makes `simde__m128` and `simde__m128i` one type on wasm; `CRRegister::setFromMask` dispatches on the mask value instead ([context.h](../include/rex/ppc/context.h)).
- The recompiler and the `rexglue` CLI are left out; install/export rules are skipped (no packaged wasm SDK yet).

## Approach

- **wasm64 (memory64), not wasm32.** wasm32 caps the whole module at 4 GB, which cannot hold the 4 GB guest virtual space, 512 MB of physical memory and the host heap. Browsers allow memory64 instances up to 16 GB.
- **Everything in a worker.** Build with `PROXY_TO_PTHREAD` so `main` runs off the browser main thread, where blocking waits, synchronous OPFS access and `Atomics.wait` are all allowed.
- **Static linking.** No `dlopen`: GPU plugins and recompiled DLL modules are linked in and registered at startup.
- **CPU first, graphics later.** Boot a title's CPU code headless (null GPU) before starting the WebGPU backend, so the memory and threading design is proven before the expensive part.

## Phase 1: Build plumbing and null GPU

- [x] `wasm64` configure and build presets in [CMakePresets.json](../CMakePresets.json) using the Emscripten toolchain file. Compile flags in the preset; `PROXY_TO_PTHREAD` and memory growth are link options on host apps and `rexruntime`.
- [x] `REX_PLATFORM_WASM` and `REX_ARCH_WASM` in [platform.h](../include/rex/platform.h), with wasm branches wherever Emscripten differs from Linux.
- [x] [CMakeLists.txt](../CMakeLists.txt): no `-march`/`-mcmodel`, a `wasm64` platform, the 64-bit check satisfied by memory64.
- [x] D3D12, Vulkan, Tracy, RenderDoc and FidelityFX off for wasm.
- [x] Null graphics backend (`gpu_backend=null`), with a 1 Hz CPU wireframe view and a software presenter; CMake accepts builds with no host GPU API.
- [x] GPU plugins register statically.
- [x] Recompiled DLL modules link into the executable and register statically (untested with a real module).
- [x] Third-party libraries build for wasm: SDL3 (with dummy video/audio for headless runs), FFmpeg/libavcodec, fmt, spdlog, imgui, xxHash, toml++, simde.
- [x] CI job that builds the preset and runs the smoke test under node ([build-wasm64.yaml](../.github/workflows/build-wasm64.yaml)).

## Phase 2: Platform layer

### Guest memory

- [x] Allocate guest memory as one linear block: virtual space, then physical.
- [x] Translate the aliased ranges in software (see [Guest memory layout](#guest-memory-layout)).
- [x] Implement `rex::memory` for wasm as bookkeeping only.
- [ ] Confirm the indirect-call dispatch table in guest memory works: it stores 8-byte host function pointers ([_indirect_call.inja](../resources/templates/codegen/_indirect_call.inja)). Under memory64 these are table indices widened to 64 bits.
- [ ] Decide how to handle the guest null page. Wasm reads at address 0 succeed instead of faulting.

### Faults and exceptions

- [x] Exception handler is a no-op on wasm.
- [ ] Guest SEH ([seh_posix.cpp](../src/core/seh_posix.cpp)) turns signals into C++ exceptions. On wasm, `__try` blocks never see hardware faults; list the titles that depend on them and add explicit checks where needed.
- [x] MMIO without faults (see [Faults and MMIO](#faults-and-mmio)).

### Threads and fibers

- [x] Guest threads run as Emscripten pthreads, with a pre-started pool.
- [ ] Browser hosting needs `SharedArrayBuffer`, so the page must be served cross-origin isolated (COOP `same-origin`, COEP `require-corp`): phase 6.
- [x] [threading_posix.cpp](../src/core/threading_posix.cpp) checked against Emscripten.
- [ ] Guest fibers: thread fibers work; creating and switching fibers needs JSPI (preferred) or Asyncify. Fibers back `XThread` ([xthread.cpp](../src/system/xthread.cpp)) and the XAPI fiber hooks ([threading.cpp](../src/kernel/crt/threading.cpp)).

### Everything else

- [x] Clock and timers.
- [x] File system under node: `NODERAWFS` reads the host filesystem directly. The browser (WASMFS with OPFS, data import) is phase 6.
- [ ] Sockets ([socket_posix.cpp](../src/core/socket_posix.cpp)): compile against Emscripten's emulation; untested. Stub or proxy over WebSocket/WebRTC later.
- [x] Dynamic libraries: nothing calls `dlopen` once plugins and modules are static.
- [x] Floating point: FPSCR emulated, no flush-to-zero or rounding control.
- [ ] Debug and crash tooling (`dbg_posix.cpp`, `rex crash`, core dumps) has no wasm equivalent; disable it.

**Milestone:** a recompiled title boots under node with the null GPU and logs through kernel startup. Not reached yet: see [Next steps](#next-steps).

## Phase 3: Codegen wasm mode

- [x] No separate codegen mode needed so far: the templates branch on `REX_PLATFORM_WASM`, so the same generated code builds natively and for wasm.
- [x] MMIO checks on every scalar load and store on wasm (template macros, not codegen).
- [x] Memory macros in [pch_h.inja](../resources/templates/codegen/pch_h.inja) for the software alias translation.
- [ ] SIMD: [intrinsics.h](../include/rex/ppc/intrinsics.h) goes through simde, which maps SSE4.1 to wasm SIMD128 with `-msimd128`. The vector shift helpers use their plain-SIMDe paths on wasm. Profile the AVX/AVX2 paths, which fall back to scalar code.
- [ ] Check generated functions against browser limits (function body size, locals per function, total functions) and split oversized functions in wasm mode.
- [ ] Measure module size, compile time and browser compile time on a large title (native executables are 35 MB+). Compiling Crash's DLL module was impractically slow under the x64-emulated toolchain on Windows ARM64.

## Phase 4: GPU write tracking

The GPU caches detect CPU writes to textures and vertex data by write-protecting physical pages and catching the fault: `EnablePhysicalMemoryAccessCallbacks`, called from [shared_memory.cpp](../src/graphics/shared_memory.cpp) and [primitive_processor.cpp](../src/graphics/primitive_processor.cpp). That cannot work without page protection. The null GPU doesn't need it.

- [ ] Choose a replacement:
  - **Dirty-page marking in store macros:** stores to physical addresses set a bit in a page bitmap, which the GPU caches read and clear. Exact, at a cost on every physical store. The wasm store helpers in pch_h.inja are the natural place.
  - **Content hashing:** caches hash watched ranges before use. No store cost, but expensive for large resources and can miss rapid rewrites.
- [ ] Wire the replacement into the shared-memory, texture and primitive caches.

## Phase 5: WebGPU backend

The largest item. For scale, the Vulkan backend ([src/graphics/vulkan](../src/graphics/vulkan)) is about 21k lines and the SPIR-V translator (`spirv_translator*.cpp` in [src/graphics/pipeline/shader](../src/graphics/pipeline/shader)) about 12k.

- [ ] New `src/graphics/webgpu` and `src/ui/webgpu` (presenter, immediate drawer, ImGui) on Dawn's Emscripten bindings (emdawnwebgpu).
- [ ] Shaders in WGSL, through one of:
  - a WGSL emitter alongside the DXBC and SPIR-V translators (preferred), or
  - SPIR-V translated by Tint compiled to wasm at runtime (several MB, slow pipeline creation).
- [ ] Render targets without fragment shader interlock, which WebGPU lacks (the Vulkan backend uses it for EDRAM emulation). Start from the non-interlock render-target path and accept its accuracy limits.
- [ ] Rectangle lists and point sprites without geometry shaders: expand in the vertex shader or a compute pass.
- [ ] Shared memory in chunks: the 512 MB buffer exceeds WebGPU's storage buffer binding limit (128 MB by default, higher on some adapters).
- [ ] Texture formats: map Xenos formats to what WebGPU supports, with compute decompression or conversion for the rest.
- [ ] Present through OffscreenCanvas, since rendering happens in a worker.

## Phase 6: Packaging and hosting

- [ ] An HTML shell page: canvas, loading progress, game data import.
- [ ] Hosting with the COOP/COEP headers that `SharedArrayBuffer` needs; document a local test server setup.
- [ ] Browser file system: WASMFS with OPFS for game data and saves, plus a first-run import or streaming.
- [ ] Asset streaming and caching (OPFS, plus service-worker caching for the `.wasm` file).
- [ ] Input through SDL3 (browser Gamepad API, keyboard, mouse with pointer lock) and audio through SDL3's Web Audio backend.
- [ ] The SDL window from a worker (the main loop runs under `PROXY_TO_PTHREAD`); the software presenter paints through SDL's window framebuffer.
- [ ] A packaged wasm SDK (install/export rules).
- [ ] Browser support matrix: memory64, JSPI and WebGPU availability across Chrome, Firefox and Safari.

## Open questions

- JSPI or Asyncify for fibers, given current browser support for JSPI.
- Dirty-page marking or hashing for GPU write tracking.
- WGSL emitter or runtime Tint.
- Whether any target titles rely on hardware faults (guest SEH, null-page reads) closely enough to need emulation.
- How long a full game takes to compile for wasm on a fast x86-64 machine, and whether generated functions need splitting.
