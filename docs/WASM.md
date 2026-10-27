# WebAssembly Target

Work plan for building ReXGlue projects for the browser with Emscripten. The `rexglue` recompiler stays a native tool; the new target is the runtime and the C++ it generates.

Emscripten is Clang-based, so the existing Clang 18+ and C++23 requirements hold.

## Status

Last updated 2026-10-23, on the `wasm` branch.

| Phase | State |
| --- | --- |
| 1. Build plumbing and null GPU | Done |
| 2. Platform layer | Milestone reached: Blade runs under node. Shutdown, executable path and fibers remain |
| 3. Codegen wasm mode | Done for Blade: within V8's limits, and floating point matches native bit for bit. Oversized functions (Crash) remain |
| 4. GPU write tracking | Done: software write watches; audited natively on Blade with Vulkan, no missed writes |
| 5. WebGPU backend | In progress: Dawn, device thread, presenter, internal shaders and the WGSL emitter's first three stages (ALU and control flow, fetches, render target output) done; GPU backend drawing Blade's dashboard like the Vulkan backend |
| 6. Packaging and hosting | Not started |

Where things stand:

- The SDK (runtime, null GPU plugin, third-party libraries) builds for wasm64, and the smoke test passes under node (checked locally; CI not rechecked).
- **Blade** (the 1888 dashboard, `../Blade`) builds for wasm64 in about 100 s on a 16-core x86-64 Linux machine (`blade.wasm` is 46 MB) and runs under node with the null GPU: kernel startup, XAM/XConfig/audio setup, then the dashboard at a steady 59 fps with the same 181 draws and 2081 vertices per frame as a native build of the same revision. The null GPU's wireframes match native's (the remaining 0.08% of pixels is animation, as between two native frames), emulating the guest's rounding modes costs about 1% of CPU time, and the write-watch checks on stores about another 2%.
- **London 2012** (`~/Source/Xenia Recomps/London`, 6 GB of game data) is the second title: it builds for wasm64 (with `-DLONDON_LAUNCHER=OFF`, as its launcher restarts the process with `execv`) and runs under node on the WebGPU backend through Dawn (`rex wasm dawn --gpu_backend=webgpu`) at about 58 fps, its frame 600 identical to a native build's. It needed offline sockets, a NaN-free depth clear value in render passes and the oversized function stopgap. The browser needs its data served differently (phase 6): the page currently preloads the whole game folder.
- **Burnout Crash** (`../Crash`) compiles except its DLL module's oversized functions (see [Oversized functions](#oversized-functions)); it hasn't linked, so static DLL module registration is still untested.
- Phase 2's milestone was moved to **node first** (headless, host filesystem) instead of a browser tab: it proves memory, threads and kernel startup without data import, COOP/COEP hosting or a canvas, which move to phase 6.

### Next steps

1. Finish Crash's build now that oversized functions build at `-O0`, and run it, the first test of static DLL module registration. Split oversized functions in codegen for good (phase 3) if `-O0` code turns out too slow or Crash's 5.4 MB function exceeds V8's limit.
2. Audit a heavier title for host-side writes to watched memory (phase 4).
3. Check CI on this branch. On `4df853d` the wasm64 build passed but the smoke test failed, most likely the `CLOCK_MONOTONIC_RAW` divide-by-zero fixed in `1035cae`.
4. The lint job fails on 10 files that were unformatted before this branch: `src/graphics/d3d12/pipeline_cache.cpp`, `src/graphics/graphics_system.cpp`, `src/graphics/pipeline/shader/dxbc_translator.cpp`, `src/graphics/pipeline/shader/dxbc_translator_memexport.cpp`, `src/graphics/pipeline/shader/spirv_translator_fetch.cpp`, `src/kernel/xam/xam_content.cpp`, `src/kernel/xam/xam_info.cpp`, `src/system/lzx.cpp`, `src/system/xam/content_manager.cpp`, `src/system/xsocket.cpp`. Running `clang-format -i` (version 20.1.8, `scripts/requirements-dev.txt`) on them fixes it.
5. Choose JSPI or Asyncify for fibers once a title needs guest fibers.
6. Send SDL the dummy audio memory64 fix (see [Audio](#audio)) so the overlay can go.

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

Run wasm64 output with node 24 or newer, such as the one emsdk installs (`emsdk/node/24.*/bin/node`); `emsdk_env` leaves a system node first on `PATH`, and node 22 doesn't enable memory64 by default.

### The SDK

```bash
cmake --preset wasm64
cmake --build out/build/wasm64 --config Release --parallel
node out/wasm64/Release/rexglue-wasm-smoke.js
```

The smoke test ([tests/wasm](../tests/wasm)) creates the null GPU through the statically linked plugin and checks the guest memory aliasing.

### A game

A game consumes the SDK in-tree (`REXSDK_DIR`); a packaged wasm SDK doesn't exist yet. Codegen still runs during the build, through a native `rexglue` built from the same SDK revision (found on `PATH`, or `REXGLUE_HOST_REXGLUE`). Regenerate once with it first if the templates changed (`rexglue codegen <game>_manifest.toml` in the game folder).

The game's own presets don't know about wasm, so configure by hand (shown for Blade; adjust paths):

```bash
FLAGS="-sMEMORY64 -pthread -msimd128 -fwasm-exceptions -sSUPPORT_LONGJMP=wasm"
cmake -S Blade -B Blade/out/build/wasm64 -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$EMSDK/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake" \
  -DEMSCRIPTEN_SYSTEM_PROCESSOR=wasm64 \
  -DCMAKE_C_FLAGS="$FLAGS" -DCMAKE_CXX_FLAGS="$FLAGS" -DCMAKE_EXE_LINKER_FLAGS="$FLAGS" \
  -DREXSDK_DIR=/path/to/rexglue-sdk \
  -DREXGLUE_HOST_REXGLUE=/path/to/native/rexglue \
  -DBLADE_LAUNCHER=OFF \
  -DREXGLUE_USE_VULKAN=OFF -DREXGLUE_ENABLE_TRACY=OFF -DREXGLUE_ENABLE_FIDELITYFX=OFF
cmake --build Blade/out/build/wasm64 --parallel
```

`BLADE_LAUNCHER=OFF` builds the stock dashboard: launcher mode starts other titles with `fork`/`execv`, which Emscripten only stubs. Crash configures the same way, without that option.

### WebGPU (in development)

```bash
cmake --preset linux-amd64 -B out/build/linux-amd64-webgpu -DREXGLUE_USE_WEBGPU=ON
cmake --build out/build/linux-amd64-webgpu --config Release --target rexglue-webgpu-device
out/linux-amd64/Release/rexglue-webgpu-device
out/linux-amd64/Release/rexglue-webgpu-shaders                        # compiles the internal shaders
out/linux-amd64/Release/rexglue-webgpu-presenter --video_driver=x11   # opens a window
out/linux-amd64/Release/rexglue-webgpu-xenos-shaders ~/.local/share/*/cache/shaders/shareable/*.xsh
```

`rexglue-webgpu-xenos-shaders` takes any shader storage files (the `.xsh` caches written by the Vulkan and D3D12 backends, and by Xenia). With `--dump DIR` it writes the WGSL and ucode disassembly of rejected shaders to `DIR`, and with `--dump-all` of every shader, for checking with another compiler, such as Firefox's naga (`cargo install naga-cli`, then `naga FILE.wgsl`).

After changing the SPIR-V internal shaders (or the Dawn pin), regenerate their WGSL with `scripts/convert_shaders_wgsl.py`; it needs the Dawn download for `tint`.

The first configure downloads Dawn (about 30 MB) into `out/deps`. For wasm, add `-DREXGLUE_USE_WEBGPU=ON` to the `wasm64` preset; `rexglue-webgpu-device` then builds as a page to run with emrun, which serves it cross-origin isolated and prints its output:

```bash
emrun --browser=firefox --browser_args='--headless --no-remote --profile <dir>' \
  --kill_exit out/wasm64/Release/rexglue-webgpu-device.html
```

Firefox on Linux still has WebGPU off by default (158): give the profile a `user.js` with `dom.webgpu.enabled`, `dom.webgpu.workers.enabled` and `gfx.webgpu.ignore-blocklist` set to true.

### rextools

rextools' `rex` drives all of this for a project (`rex wasm help`): `rex wasm build [--web]`, `rex wasm run` (node), `rex wasm dawn` (node with Dawn's WebGPU), `rex wasm browser` (the default browser, game data packaged into the page), and `rex dawn build|run` (native, with Dawn).

### Running in the browser

The browser build (`REXGLUE_WASM_NODE=OFF`) needs, at link: `-sOFFSCREENCANVAS_SUPPORT` (the canvas goes to main's pthread and on to the WebGPU device thread), `-sFORCE_FILESYSTEM` (for a data package's loader) and, for emrun, `--emrun`. The game folder can be packaged with Emscripten's `file_packager` into a `.data` file the page loads before the game (mounted at `/game`); `rex wasm build --web` does this, which suits small games until there's streaming (phase 6). In the browser the app also logs to the console. Built with OffscreenCanvas support, the page's canvas belongs to the UI thread, so without WebGPU the software presenter draws on it with its 2D context there (SDL's window framebuffer is on the browser's main thread, which no longer has the canvas).

### Running under node

```bash
node blade.js --gpu_plugin=xenos --gpu_backend=null --video_driver=dummy \
  --game_data_root=/path/to/Blade/assets --log_level=debug \
  --log_file=/path/to/blade.log --null_gpu_sample_dump_dir=/path/to/frames
```

`REXGLUE_WASM_NODE` (on by default) links host apps with `-sENVIRONMENT=node -sNODERAWFS`, so the game folder is read straight from the host filesystem; Windows-style paths are untested under it. As natively, the console only shows startup and everything after goes to the log file; give `--log_file` explicitly (next step 2). `null_gpu_sample_dump_dir=<dir>` writes the null GPU's 1 Hz wireframe frames as BMPs. With no audio device, audio runs on SDL's dummy driver (see [Audio](#audio)).

To compare against native, build the same revision natively with the SDK in-tree and run it with `--gpu_backend=null --video_driver=offscreen` (native SDL has no dummy video driver).

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

A static runtime also changes how projects override kernel exports. Natively, a project's `REX_HOOK` of an export (Blade replaces `ExGetXConfigSetting`, `ExSetXConfigSetting` and `VdSetDisplayModeOverride`) takes precedence over `rexruntime`'s shared-library definition. On wasm, `REX_EXPORT*` definitions are weak ([hook.h](../include/rex/hook.h)), so the project's strong definition wins the link, and the export registrar's `&name` resolves to it too.

### Threads, fibers, time

- Host apps link with `-sPROXY_TO_PTHREAD -sPTHREAD_POOL_SIZE=32 -sSTACK_SIZE=8MB -sEXIT_RUNTIME` ([rexglue_helpers.cmake](../cmake/rexglue_helpers.cmake)).
- [threading_posix.cpp](../src/core/threading_posix.cpp): thread ids are numbered per thread, names go through `emscripten_set_thread_name`, affinity is a no-op, and user callbacks (APCs) wait for the target's next alertable wait slice, as there are no signals between threads.
- [fiber_wasm.cpp](../src/core/fiber_wasm.cpp): threads can become fibers (every `XThread` does), but creating or switching to guest fibers logs an error and fails until JSPI or Asyncify is chosen.
- FPSCR ([fpscr.h](../include/rex/platform/fpscr.h)) is emulated per thread with the MXCSR layout. Its rounding mode is honored in software (see [Floating point](#floating-point)); flush-to-zero isn't.
- The clock uses `CLOCK_MONOTONIC` with 1 GHz ticks (Emscripten has no `CLOCK_MONOTONIC_RAW`).

### Floating point

Natively, FPSCR's rounding mode is copied into the host's (MXCSR), which every scalar operation honors. Titles do switch it: Blade lays out text with round-up and round-down in effect, and without them its glyphs came out a few pixels narrower on wasm. Wasm has no rounding control, so:

- Codegen emits scalar FPU arithmetic through macros ([fpu.h](../include/rex/ppc/fpu.h)): `REX_FADD`, `REX_FSUB`, `REX_FMUL`, `REX_FDIV`, `REX_FSQRT`, `REX_FMA`, `REX_FSINGLE` (single-precision results, `frsp`, `stfs`) and `REX_FCFID`. Natively they are the plain expressions codegen emitted before. On wasm, round-to-nearest is the plain expression after one check of the emulated mode; a directed mode takes the nearest result's exact error (TwoSum, a Dekker product on frexp-scaled mantissas, an exact expansion sign for fused multiply-add) and moves one ulp when the exact value lies on the rounding side. Signed zeros and overflow follow IEEE as x86 does.
- `fctid`/`fctiw` call `simde_mm_cvtsd_si64`/`si32`, which [intrinsics.h](../include/rex/ppc/intrinsics.h) redirects on wasm to versions that round with the emulated mode; SIMDe's fallback used `round()`, which takes ties away from zero.
- `vmsum4fp` calls `simde_mm_dp_ps`, redirected on wasm to add the products pairwise as `dpps` does; SIMDe's fallback added them in sequence.
- VMX arithmetic rounds to nearest whatever FPSCR says, as on PPC (natively it follows MXCSR, which differs from PPC when a title sets a directed mode).

Checked with differential tests run natively and under node over 200k to 300k inputs each (extremes, denormals, NaNs and halves included): the 65 SIMDe operations Blade uses, and the 12 scalar macro forms in all four rounding modes. All match bit for bit except `cvttsd_si64` at 2^63 and above, which generated code clamps first. The smoke test checks known cases of each.

Projects regenerated with an older `rexglue` still build; their scalar arithmetic just doesn't see directed rounding on wasm.

### Write watches

[write_watch.h](../include/rex/memory/write_watch.h): one bit per 4 KB guest page (128 KB for the 4 GB space), kept equal to the physical heaps' `notify_on_invalidation` flags by the two places that change them (`PhysicalHeap::EnableAccessCallbacks` sets, `TriggerCallbacks` clears; [xmemory.cpp](../src/system/xmemory.cpp)). The per-view semantics carry over: the 0xA0000000, 0xC0000000 and 0xE0000000 views of a physical page have separate bits, as natively they are separately protected mappings.

Generated code calls `CheckWriteWatch(address, size)` around each write: below 0xA0000000 it is one compare; above, one bit test. A hit calls `OnWatchedWrite`, which runs `TriggerPhysicalMemoryCallbacks` per watched page with length 1 and loose unwatching, as the native fault handler does.

- **Order on wasm: write, then check.** If a cache starts watching the page between the two, it either copies the new data or the check sees its watch. Checking first could miss both. The window isn't fully closed without a fence between the write and the check (StoreLoad ordering), which would cost every physical store; the remaining race needs the CPU to write a page at the moment the GPU starts watching it.
- **Native audit mode:** the CMake option `REXGLUE_SOFTWARE_WRITE_WATCH` builds the checks natively, before each write instead, with page protection still on. A write the checks cover unwatches the page before it lands; one they miss faults on a page that is still watched, and `Memory::AccessViolationCallback` logs it with a backtrace (Linux). This is how to find host-side writers, which wasm can't see at all.
- Generated by an older `rexglue`, a project still builds but its guest writes don't check watches on wasm; regenerate it (`rexglue codegen --ignore-stamp` when only the SDK changed).

### Audio

Titles treat a failed XAudio client registration as fatal (Blade exits with `XAPI RETURN VALUE` 0x80300004, natively too), and node has no audio device. When SDL can't initialize audio, [sdl_audio_driver.cpp](../src/audio/sdl/sdl_audio_driver.cpp) retries with SDL's dummy driver, which consumes frames in real time. Only builds that include it have it (wasm does; native doesn't).

On Emscripten, SDL's dummy driver drives its timer from JS with `dynCall('vi', ...)`, passing the device pointer as an i32, which throws under memory64 ("Cannot convert ... to a BigInt"). [thirdparty/CMakeLists.txt](../thirdparty/CMakeLists.txt) builds a copy of `SDL_dummyaudio.c` with `'vp'` in its place, leaving the submodule untouched, and warns if SDL changes that line.

### Oversized functions

Codegen warns about functions over `max_file_size_bytes` (1 MB of C++). Crash's DLL module has six, of 1.3 to 5.4 MB. Natively they compile normally; for wasm at `-O3` some take hours. Stack samples show two causes: `WebAssemblyRegStackify`, a wasm backend pass that is quadratic here (`LiveIntervals::shrinkToUses`), and the mid-level optimizer (loop vectorizer, load elimination). Timings for `crash_recomp.81.cpp` (1.3 MB, one function) on a 16-core x86-64 machine:

| Level | Compile time | Wasm code for the function |
| --- | --- | --- |
| `-O0` | 3 s | 0.6 MB |
| `-O1` | 49 min | 1.65 MB |
| `-O3` | 41 min | 1.53 MB |

`crash_recomp.207.cpp` (3.4 MB) was still in the mid-level optimizer after more than 2 hours of CPU time. RegStackify runs at every level above `-O0`, so `-O1` is no fix. At `-O3` the wasm code is about 1.2 times the C++ size, which puts the 5.4 MB function near V8's 7.65 MB limit on a function body. The `-O0` output is smaller, likely because the per-access MMIO check isn't inlined. A per-file `-O0` on wasm needs those files built without the precompiled header, which is built at `-O3` and which Clang refuses to mix with another level (it finds `<header>.pch` even through a plain `-include`).

The stopgap is in place: codegen lists the recompiled files holding a function over `max_file_size_bytes` as `GENERATED_OVERSIZED_SOURCES` in `sources.cmake`, and on Emscripten `rexglue_configure_target` ([rexglue_helpers.cmake](../cmake/rexglue_helpers.cmake)) builds them at `-O0` without the precompiled header (they include the project header through their `_funcs` header). London (the 2012 Olympics game, 43,429 functions, seven files with functions of 1.1 to 1.2 MB) builds for wasm64 this way in about 6 minutes; its module is 190 MB. Projects pick the list up when codegen next runs (`rex wasm build --force-codegen`).

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
- [x] Third-party libraries build for wasm: SDL3 (with dummy video/audio for headless runs; see [Audio](#audio)), FFmpeg/libavcodec, fmt, spdlog, imgui, xxHash, toml++, simde.
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
- [x] Sockets, offline for now: Emscripten's socket emulation needs a WebSocket server even to bind (under node it `require`s `ws`; browsers can't listen), so on wasm `XSocket` ([xsocket.cpp](../src/system/xsocket.cpp)) uses no host sockets. Creating, binding, listening and options succeed, sends are dropped, receives and accepts would block, connects fail, and `select` reports every socket writable and none readable. That's a console on a disconnected network: London, which quits if socket creation fails, runs on. A real network (WebSocket or WebRTC proxy) comes later.
- [x] Dynamic libraries: nothing calls `dlopen` once plugins and modules are static.
- [x] Floating point: FPSCR emulated; rounding modes honored in software (see [Floating point](#floating-point)), flush-to-zero not.
- [ ] Debug and crash tooling (`dbg_posix.cpp`, `rex crash`, core dumps) has no wasm equivalent; disable it.

- [x] Executable path: `GetExecutablePath` reports the program's `.js` file, through `EM_JS`: under node its host path (NODERAWFS), so `<app>.toml` and `logs/` sit next to it as natively; in the browser `/<name>.js` in the page's file system.
- [x] Shutdown: when the title ends by itself, the app now hard-exits as on window close (`ReXApp::ExitAfterTitle`), natively too, instead of running the subsystem teardown, which hung under node and aborted natively. Under Emscripten an exit from a pthread only reaches the main thread, which the runtime keeps alive, so the exit is proxied there and forced, with `process.exit` under node (whose event loop open handles keep alive). Tested under node with a simulated title exit: the process exits with 0 at once.
- [x] Audio with no device: SDL's dummy driver (see [Audio](#audio)).
- [x] Projects' hooks of kernel exports win the static link (see [No dynamic loading](#no-dynamic-loading)).

**Milestone:** a recompiled title boots under node with the null GPU and logs through kernel startup. **Reached 2026-10-23 with Blade**, which runs on to the dashboard at 59 fps with the same draws as native.

## Phase 3: Codegen wasm mode

- [x] No separate codegen mode needed so far: the templates branch on `REX_PLATFORM_WASM`, so the same generated code builds natively and for wasm.
- [x] MMIO checks on every scalar load and store on wasm (template macros, not codegen).
- [x] Memory macros in [pch_h.inja](../resources/templates/codegen/pch_h.inja) for the software alias translation.
- [x] SIMD: [intrinsics.h](../include/rex/ppc/intrinsics.h) goes through simde, which maps SSE4.1 to wasm SIMD128 with `-msimd128`. Of the AVX/AVX2 operations Blade uses, `permute_ps` is a single wasm shuffle; the variable shifts (`sllv_epi32` and the SDK's 8- and 16-bit helpers built on it) are a 4-lane loop, as natively on x86-64-v2, since wasm SIMD128 has no per-lane variable shift.
- [x] Floating point bit-identical to native (see [Floating point](#floating-point)).
- [x] Oversized functions (see [Oversized functions](#oversized-functions)): built at `-O0` on wasm as a stopgap (codegen lists them). Splitting them in codegen, which would also keep them under V8's function body limit, remains open.
- [x] Browser limits on Blade: largest function body 601 KB (V8's limit is 7.65 MB), at most 112 locals in a function (50,000), 22,119 functions (1,000,000).
- [ ] Measure module size, compile time and browser compile time on a large title (native executables are 35 MB+). Blade: 46 MB `.wasm`, about 100 s for a full build on 16 cores; V8 compiles it in 0.36 s with Liftoff (11.5 s with TurboFan on all cores, 55 s on one), lazily per function by default. Crash: blocked on oversized functions.

## Phase 4: GPU write tracking

The GPU caches detect CPU writes to textures and vertex data by write-protecting physical pages and catching the fault: `EnablePhysicalMemoryAccessCallbacks`, called from [shared_memory.cpp](../src/graphics/shared_memory.cpp) and [primitive_processor.cpp](../src/graphics/primitive_processor.cpp). That cannot work without page protection. The null GPU doesn't need it; the WebGPU backend will.

- [x] Replacement chosen: software write watches, marked per page and checked on every write (see [Write watches](#write-watches)). Content hashing would cost nothing per store but is expensive on large resources and misses rewrites.
- [x] Every guest write path checks: `REX_STORE_*`, `REX_MM_STORE_*`, `dcbz`, `dcbz128`, `stvx` and `stwcx.`/`stdcx.`. The caches need no change: a hit makes the same `TriggerCallbacks` call as the native fault.
- [x] Audited natively (`REXGLUE_SOFTWARE_WRITE_WATCH`): Blade on Vulkan for 25 s, hundreds of watched-page writes, none missed by the checks.
- [ ] Audit a heavier title (one that streams textures or decodes video into GPU memory) for host-side writers. File reads already trigger callbacks explicitly ([xfile.cpp](../src/system/xfile.cpp)); any other host code writing watched memory must call `rex::memory::OnWatchedWrite` on wasm.

## Phase 5: WebGPU backend

The largest item. For scale, the Vulkan backend ([src/graphics/vulkan](../src/graphics/vulkan)) is about 21k lines, its presenter and drawers ([src/ui/vulkan](../src/ui/vulkan)) about 7k, and the SPIR-V translator (`spirv_translator*.cpp` in [src/graphics/pipeline/shader](../src/graphics/pipeline/shader)) about 12k. What the backends share stays as is: the shader front end ([translator.cpp](../src/graphics/pipeline/shader/translator.cpp)), the render target, texture and primitive caches, shared memory with its write watches (phase 4), and the resolve readback modes.

### Decisions (2026-10-23)

1. **Base: port the Vulkan backend.** It already has a fallback for nearly everything WebGPU lacks: the non-interlock render target path (`kHostRenderTargets`), 2x MSAA through 4x, stencil transfers without stencil export, missing clip distances and depth clamp, and rectangle lists and point sprites expanded in the vertex shader (`kRectangleListAsTriangleStrip`, `kPointListAsTriangleStrip`) instead of geometry shaders. D3D12 is the other candidate, but SPIR-V is structurally closer to WGSL.
2. **Shaders: a WGSL emitter**, a third translator beside DXBC and SPIR-V on the shared front end, modeled on `spirv_translator`. It writes WGSL text directly: one compile in the browser, no extra wasm payload, and errors point at our code. Rejected: converting SPIR-V at runtime with Tint compiled to wasm (several MB, a second compile per shader, and Tint's SPIR-V reader only accepts SPIR-V 1.3 for Vulkan 1.1 and refuses some of what the translator emits). The ~97 fixed internal shaders (texture loads, resolves, gamma; [src/graphics/shaders/vulkan_spirv](../src/graphics/shaders/vulkan_spirv), whose `.xesl` sources aren't in this repository) are converted once, offline, with Dawn's Tint, and the WGSL is checked in like the SPIR-V headers.
3. **Threading: a WebGPU device thread.** In the browser a WebGPU device belongs to the worker that created it, mapping buffers is asynchronous, and a canvas presents only when its worker returns to its event loop. So one pthread owns the device and the OffscreenCanvas and runs only queued work, returning to its event loop in between. The command processor records commands (the Vulkan backend already records into a deferred command buffer) and hands batches over through Emscripten's proxying queue; it blocks on a futex only for object creation and readbacks. Only standard pthreads: no JSPI. Rejected: JSPI on the command processor thread (fewer moving parts, but JSPI with pthreads, `PROXY_TO_PTHREAD` and memory64 together is barely exercised) and Asyncify (it would instrument all 46 MB of guest code).
4. **API: `webgpu.h`, developed natively on Dawn.** Natively, Dawn (Chrome's WebGPU implementation) is an optional backend (`gpu_backend=webgpu`), so the backend is written and debugged in the desktop build with RenderDoc and compared with Vulkan on the same title; in the browser the same code builds with Emscripten's emdawnwebgpu port (`--use-port=emdawnwebgpu`). Natively the device thread can be the command processor thread itself.

### Constraints that shape it

- **Browsers:** WebGPU is in Chrome/Edge (113+), Firefox (141+, Windows first) and Safari 26. Memory64 is in Chrome 133+ and Firefox 134+, but in Safari only in Technology Preview (251, August 2026), so Chrome/Edge and Firefox on Windows are the realistic targets for now.
- **Shared memory:** one 512 MB storage buffer needs `maxBufferSize` and `maxStorageBufferBindingSize` of at least 512 MB, requested at device creation (the defaults are 256 and 128 MB; desktop adapters report 1 GB or more). Fall back to binding it in windows if an adapter can't.
- **Optional features to request, with fallbacks when missing:** `texture-formats-tier1` (16-bit unorm/snorm render targets for k_16_16 and k_16_16_16_16, else the existing float fallback), `dual-source-blending`, `clip-distances`, `depth-clip-control`, `float32-blendable`, `texture-compression-bc`, `primitive-index`.
- **Formats and samples:** no 2x MSAA (only 1x and 4x), depth24plus may be 32-bit float, no 10:11:11 or 2:10:10:10 float targets beyond `rg11b10ufloat`; the existing format fallbacks of the host render target path cover these.
- **Shaders:** WGSL rejects derivatives (implicit-LOD sampling) in non-uniform control flow, which Xenos flow control produces; use `diagnostic(off, derivative_uniformity)` or explicit LOD and gradients. No `discard` in vertex stages, no 64-bit types, no geometry or tessellation stages.
- **Resolve and memexport readback:** use the delayed modes (`readback_resolve=fast|some`, `readback_memexport_fast`), which fit asynchronous buffer mapping; full synchronous readback costs a round trip through the device thread.
- **Presentation:** in the browser, the device thread draws into the OffscreenCanvas's current texture and the frame shows when it returns to its event loop; the UI's presenter must not call WebGPU from the UI thread. (Another Emscripten emulator port reports `wgpuSurfacePresent` aborting in emdawnwebgpu on a pthread; it presents by submitting to the canvas texture.)

### Plan

- [x] Dawn as an optional dependency (`REXGLUE_USE_WEBGPU`, [cmake/dawn.cmake](../cmake/dawn.cmake)): natively the prebuilt release of the revision Emscripten's emdawnwebgpu port pins (v20260423.175430, Dawn `31e25af`), downloaded once into `out/deps` and checked against its SHA-256; in the browser the emdawnwebgpu port. The prebuilts are Dawn's unsigned GitHub nightlies for Linux x64, Windows x64 and macOS; elsewhere set `REXGLUE_DAWN_DIR` to a Dawn built from source at that revision. Moving to a newer emsdk means moving the pin with it.
- [x] Device creation ([ui/webgpu/device.h](../include/rex/ui/webgpu/device.h)): the adapter's full limits and the optional features it has; asynchronous for the browser's device thread, blocking natively. `rexglue-webgpu-device` ([tests/webgpu](../tests/webgpu)) runs a compute shader over a 512 MB storage buffer and reads it back: passes natively on Dawn/Vulkan (RX 9070 XT: 4 GB buffers and bindings, all seven optional features). For wasm64 it builds against emdawnwebgpu and, under node, gets as far as reporting that `navigator.gpu` is missing, so memory64 and the port work together.
- [x] `gpu_backend=webgpu` selects the backend in progress ([src/graphics/webgpu](../src/graphics/webgpu)); "any" never picks it.
- [x] `src/ui/webgpu` natively: provider, presenter ([presenter.h](../include/rex/ui/webgpu/presenter.h)) and immediate drawer, so ImGui overlays draw too. The presenter stretches the guest output bilinearly into the letterboxed rectangle (the FidelityFX effects aren't ported) and gives refreshers the guest output texture, sampled, storage, render attachment and copy destination. WebGPU's reference counting and in-order queue replace the Vulkan presenter's submission tracking. Natively the UI thread paints while the GPU thread refreshes, on a device with Dawn's `ImplicitDeviceSynchronization`. `rexglue-webgpu-presenter` ([tests/webgpu](../tests/webgpu)) opens a window for a few seconds with a guest output refreshed from another thread under an ImGui window and saves a presented frame: correct on Dawn/Vulkan, letterboxing and overlay included. On Linux the prebuilt Dawn takes only Xlib windows (no Wayland, and no XCB), so on a Wayland desktop run with `--video_driver=x11` (XWayland), or build Dawn with `DAWN_USE_WAYLAND` and set `REXGLUE_DAWN_WAYLAND`.
- [x] The presenter in the browser: its calls are on the device thread, it presents to the canvas the thread owns, and it leaves `wgpuSurfacePresent` out there (emdawnwebgpu aborts on it; the canvas shows the frame when the thread returns to its event loop). Blade runs in Firefox 158 with the null GPU presenting its wireframes through it (`null_gpu_presenter=webgpu`, below).
- [x] Something to show before the backend exists: `null_gpu_presenter=webgpu` makes the null GPU present its sampled wireframes through the WebGPU provider (`ui/webgpu/factory.h`, so the GPU plugin needs neither webgpu.h nor its own Dawn), through a CPU upload any refresh context can offer (`GuestOutputRefreshContext::WriteRgba8`). Without a WebGPU device it falls back to software. Natively, under node with Dawn (the `webgpu` npm package) and in Firefox, Blade runs at 60 fps that way.
- [x] Device thread ([device_thread.h](../include/rex/ui/webgpu/device_thread.h)): owns the device and makes every WebGPU call, releases included, through `Call` (run there and wait) or `Post` (queue and return); nothing on it waits for the GPU, asynchronous results come back on it through callbacks (`kDeviceThreadCallbackMode`) and other threads block for them if they must. In the browser it's a pthread living in its event loop, fed through an Emscripten proxying queue, and it takes over the canvas: from the calling thread if that owns it (with `PROXY_TO_PTHREAD`, main's pthread gets `#canvas` by default), else from the browser's main thread. Natively it's a thread running a task queue and Dawn's event processing, so the native build exercises the same threading (Dawn's implicit device synchronization is no longer requested). The presenter and immediate drawer go through it: the paint runs there, UI drawers included, while the UI thread waits; refreshers run there; immediate textures are created through it and released on it. `rexglue-webgpu-device` passes natively and in headless Firefox 158 on Linux (Vulkan, RX 9070 XT): device created on the thread, a 512 MB compute and readback whose map completes on the thread while main waits, and 30 frames presented to the canvas it owns.
- [x] Command batching: the command processor records each submission into a `WebGPUCommandList` on its own thread, with no WebGPU calls: commands are closures replayed into a command encoder on the device thread, and uploads go to an arena that becomes the submission's one staging buffer, so copies out of it are ordered with the draws (`wgpuQueueWriteBuffer` would land before the whole submission) and converted index buffers are drawn from it directly. At most two submissions are in flight. Natively the plugin compiles against Dawn's headers only and calls the runtime's Dawn, which `librexruntime.so` exports (a second, static Dawn in the plugin couldn't use the runtime's device); in wasm it all links into one module.
- [x] WGSL emitter, stage 1 ([wgsl_translator.h](../include/rex/graphics/pipeline/shader/wgsl_translator.h)): `WgslShaderTranslator` mirrors the SPIR-V translator's control flow (a loop around a switch on the program counter; WGSL cases can't fall through, so each case continues to the next label explicitly, and `exece` sets the counter to -1), exec conditionals and predication, operand addressing, every ALU vector and scalar operation, result write masks, constants and clamping, the vertex index (with index loads and endianness) and position epilogue (W and XY/Z formats, NDC transform, clip distances, kill by NaN W, point sprites from triangle strips), and pixel shader interpolators and PsParamGen. Its modification bits and system constants follow the SPIR-V translator's, minus what WebGPU lacks (tessellation, fragment shader interlock, cull distances), plus the vertex shader's centroid mask, which WebGPU requires to match between stages. Memexport is skipped, reported as a non-fatal translation error (fetches came in stage 2). `rexglue-webgpu-xenos-shaders` ([tests/webgpu](../tests/webgpu)) reads `.xsh` shader storage files, translates every shader under several modifications (host vertex shader types, PsParamGen with and without points, centroid, clip distances) and creates each as a module and a render pipeline with an automatic layout (pixel shaders behind a vertex shader with matching outputs); an invalid canary must be rejected. On 1463 shaders from 11 titles' caches (501 vertex, 962 pixel; 3888 translations), Dawn/Vulkan accepts all, and so does naga 30 (Firefox's compiler, run on the dumped WGSL with `--dump-all`).
- [x] WGSL emitter, stage 2 ([wgsl_translator_fetch.cpp](../src/graphics/pipeline/shader/wgsl_translator_fetch.cpp)): vertex fetch from shared memory (every vertex format, endianness, normalization and exponent bias; shared memory can be split into 2 or 4 storage bindings for devices whose binding size limit is under 512 MB), and texture fetch as in the SPIR-V translator: explicit LOD or gradients (never implicit), offsets, unnormalized coordinates, stacked textures as 2D arrays filtered between layers in the shader, cube face coordinates, signedness, biased and gamma conversion, exponent bias, `getWeights`, `getGradients`, `setTexLOD` and `setGradientH/V`, and draw resolution scaling. Textures and samplers go in group 2 (vertex) or 3 (pixel), textures first; `WgslShader` gives the backend the bindings, like `SpirvShader`. Differences from SPIR-V: the guest swizzle is always applied in the shader (WebGPU views can't swizzle); WGSL has no LOD query, so `getCompTexLOD` computes the LOD from the coordinate derivatives and the texture size, clamped to its levels (no anisotropy or sampler LOD clamp); stacked layers are mixed from the first layer to the second (the SPIR-V code adds the difference to the second sample instead); and 2D register gradients keep horizontal and vertical apart (the SPIR-V code loads them swapped). The last two look like bugs in the SPIR-V translator, to check against hardware behavior. Corpus results unchanged: all 3888 translations accepted by Dawn/Vulkan and naga 30, also with `--resolution-scale 3`. 1D textures are the one path no shader in the corpus uses.
- [ ] Texture bindings per stage: each fetch constant binds a signed and an unsigned view (plus two 3D views if it may be stacked), so 64 of the 1463 corpus shaders need more than WebGPU's default 16 sampled textures per stage (up to 26). Check the browsers' adapter limits; otherwise bind only the variants a draw's fetch constants need (with the pipeline layout following), or sample signed formats through the unsigned view.
- [x] WGSL emitter, stage 3: render target output, as the SPIR-V translator's host render target path (WebGPU has no fragment shader interlock): alpha test on render target 0, alpha to coverage with the guest's dithering through `@builtin(sample_mask)` (guest 2x MSAA on host 4x samples 0 and 3, since WebGPU has only 1x and 4x), color exponent bias, PWL gamma encoding for gamma render targets (before blending, as in the other host render target paths), and float24 depth, truncating or rounding, from oDepth or from the fragment depth at sample rate. The early depth hint skips alpha test and alpha to coverage as in SPIR-V, though WGSL can't request early fragment tests. `alpha_to_mask` joins the system constants. The corpus test adds the early hint and both float24 modes (4850 translations, all accepted by Dawn/Vulkan and naga), and runs the translator's float24 and gamma helpers, taken from its output, over 17.5 million inputs against the CPU versions in `xenos.h`: depth matches bit for bit, gamma within 6e-8.
- [ ] WGSL emitter, stage 4: memexport, and the memexport compute shader for vertex shaders (WebGPU allows no storage writes from vertex shaders).
- [ ] WGSL emitter: differential tests against the SPIR-V translator (same inputs through both, results compared), once fetches work.
- [x] Internal shaders ([scripts/convert_shaders_wgsl.py](../scripts/convert_shaders_wgsl.py)): all 97 SPIR-V headers in `src/graphics/shaders/vulkan_spirv` convert with Tint (the `tint` from the downloaded Dawn) into `src/graphics/shaders/webgpu_wgsl/<name>.h` (`const char <name>_wgsl[]`), checked in, plus an index, `all_shaders.h`; `--check` fails when any output is stale. Two adaptations, noted in each header: the gamma ramp shaders' texel buffers become `texture_1d` before conversion (they're only fetched from; bind a 1D texture with the ramp's texels), and push constants, which Tint emits as `var<immediate>` (Chrome-only, 64 bytes), become a uniform buffer at binding 0 of the first bind group the shader leaves free (group 1, 2 or 3). Each result is validated by Tint again. `rexglue-webgpu-shaders` ([tests/webgpu](../tests/webgpu)) creates every shader on a real device, and every compute shader as a pipeline: Dawn/Vulkan accepts all 97 (91 compute); Firefox 158 (wgpu/naga, a separate compiler) accepts 95 and lacks `texture-formats-tier1`, which the other two need to write `rgb10a2unorm` storage textures (`apply_gamma_pwl_cs`, `apply_gamma_table_cs`); without the feature the backend uses their fragment shader twins, as the Vulkan backend chooses between them.
- [x] Backend skeleton: graphics system, command processor (shaders loaded and analyzed, primitive processing, vertex buffers requested, one submission per frame, the presenter refreshed with a clear color for now), shared memory (one 512 MB buffer: WebGPU has no sparse buffers, and no barriers to track) and the primitive processor (full 32-bit indices; fans, line loops and quads converted; points and rectangles for vertex shader expansion). Blade runs on it natively at 60 fps: 180 draws a frame processed (native draws 181), about 30 KB uploaded per frame.
- [x] First rendered frames: pipeline cache (shaders translated with `WgslShaderTranslator` and created as modules, render pipelines from guest state with an explicit layout: shared memory, constants with dynamic offsets into the submission's uniform buffer, and per-shader texture groups), the draws (system constants, packed float, bool/loop and fetch constants, unchanged blocks reused within a submission; viewport and scissor through `draw_util` with Y up as in Direct3D 12), a minimal render target cache (one host texture per EDRAM surface, no aliasing, MSAA or float24 yet), clears from resolves, and the swap showing the last resolved area (copied out at resolve time, as the clear usually follows). Textures are 1x1 white placeholders until the texture cache. Blade drew 179 of its 180 draws a frame this way at 60 fps, the dashboard's layout visible with text as solid blocks; `--webgpu_capture_frame=N` saves a frame's guest output as a BMP.
- [x] Rectangle lists in the WGSL translator (`kRectangleListAsTriangleStrip`), as in the SPIR-V translator without geometry shaders: the guest vertex shader runs for the rectangle's 3 vertices in a loop (fresh state each iteration), host vertices 0-2 take them ordered from the corner opposite the longest edge, and vertex 3 completes the parallelogram. No clip distances on rectangles yet. The corpus test adds the variant (5351 translations, all accepted by Dawn and naga), and Blade now draws all 180 draws a frame.
- [x] Texture cache ([texture_cache.h](../include/rex/graphics/webgpu/texture_cache.h)), on the shared `TextureCache` like the Vulkan one: guest textures untiled and converted by the converted WGSL load compute shaders into a buffer, then copied in (rows aligned to 256 bytes for WebGPU), in order with the rest of the command list; samplers from fetch constants (repeat, mirrored repeat and clamp to edge only); per-draw bind groups from the views and samplers, built on the device thread; the guest and host-format swizzle and signedness in the system constants (WebGPU views can't swizzle); fetch constant writes invalidating bindings. Format differences from Vulkan: no packed 16-bit, 16-bit unorm/snorm or 4:2:2 formats (converted to 8-bit or float where load shaders exist: 5:6:5, 4:4:4:4, 4:2:2 and 16-bit; 5:5:5:1, 6:5:5, 10:11:11 and DXT3A-as-1111 are not supported yet, logged once each); block-compressed formats need `texture-compression-bc` and whole blocks (decompressed otherwise); 32-bit float and depth formats need `float32-filterable` (now requested); signed versions are separate textures. Blade's dashboard now renders like on Vulkan: at frame 600, 0.04% of pixels differ by more than 5%, none by more than 25%; the remaining difference is uniform (WebGPU about 10 levels brighter in the midtones), the display gamma ramp the swap doesn't apply yet. `--gpu_capture_frame=N --gpu_capture_path=F.bmp` (any backend) saves a frame's guest output, for such comparisons.
- [x] Swap through the display gamma ramp: the 256-entry table for 8-bit frontbuffers, the piecewise linear ramp for 10-bit ones, as the Vulkan backend's `apply_gamma` shaders, in a small fragment shader (the ramp in a uniform buffer rather than a texel buffer, which WebGPU lacks) drawing the resolved area into the guest output. Blade's frame 600 now matches Vulkan's to a mean difference of 0.75 levels; 0.015% of pixels differ by more than 5%, along text and line edges.
- [x] In the browser: Blade runs on the backend in Firefox (headless, WebGPU enabled in the profile) at 60 fps with all its draws, and frame 600 matches Vulkan's (0.02% of pixels differ by more than 5%, none by more than 25%). Two fixes were needed. emdawnwebgpu passes `setBindGroup`'s dynamic offsets as all of `HEAPU32` plus an offset, and Firefox rejects views over 2 GB, which a wasm64 heap holding guest memory always is; [cmake/emdawnwebgpu_heap_views.js](../cmake/emdawnwebgpu_heap_views.js), a `--pre-js`, wraps the encoders' `setBindGroup` to pass just the offsets. And browser presentation doesn't block as a FIFO swap chain does, so the UI thread repainted over a thousand times a second, starving the device thread (the guest ran at a few frames a second or stalled); the presenter now requests an animation frame on each present and the next paint waits for it (at most 100 ms, as hidden tabs get none). Under emrun, `--gpu_capture_frame` also hands the file to emrun's server, which saves it in `dump_out` next to the page.
- [x] Gamepads in the browser. Two problems kept them from reaching SDL. First, SDL registers its browser event callbacks (keyboard, mouse, focus, resize, gamepad connection) on the UI thread. That thread is a worker blocked in `SDL_WaitEvent` and never returns to its event loop, so proxied callbacks never ran; on Emscripten the UI loop now waits in 4 ms steps and runs the thread's queued calls. Second, SDL's Emscripten joystick driver reads `navigator.getGamepads()` in the connection callback, which workers don't have. An overlay in [thirdparty/CMakeLists.txt](../thirdparty/CMakeLists.txt) (a patched copy, the submodule untouched) registers the connection callbacks on the main browser thread, and lets the initial scan on the worker do without vendor and product IDs. SDL's default Emscripten mapping covers the standard layout. Tested in headless Firefox with a simulated pad: a fake `navigator.getGamepads()` plus a synthetic `gamepadconnected` event (Firefox's own gamepad test service is automation-only). Blade's dashboard follows its d-pad and A presses.
- [x] Audio in the browser. SDL's JavaScript turns memory64 pointers into `HEAP32` indices with `Number(ptr / 4n)`, which assumes BigInts. Emscripten passes pointers to `EM_ASM` as Numbers, so audio output threw "can't convert BigInt to number" on every buffer, and pointer events would have too. An SDL.c overlay accepts either. Web Audio now receives what the game mixes: the peaks of Blade's dashboard sounds match the APU's, measured in Firefox with autoplay allowed. As usual in browsers, sound starts after the first click or key press on the page; gamepad input doesn't count as user activation.
- [x] Resolves to memory and the swap from the real frontbuffer, as the Vulkan backend's host render target path. A resolve dumps the tiles it reads from the source render target into a 10 MiB EDRAM storage buffer. The dump is a small internal WGSL shader in [render_target_cache.cpp](../src/graphics/webgpu/render_target_cache.cpp) that writes the guest layout and encoding: 8-bit, 2:10:10:10, 7e3 float, 16-bit fixed with the -32...32 range, 16- and 32-bit float, and every MSAA sample. The converted resolve copy shaders then write the destination to shared memory, followed by `MarkRangeAsResolved`. The swap takes the frontbuffer from the texture cache (fetch constant 0, which VdSwap sets) and applies its swizzle in the gamma ramp pass. Blade's frame 600 matches Vulkan natively and in Firefox (0.015% of pixels differ by more than 5%, none by more than 25%). Only Blade's 8-bit resolves are exercised so far; the other encodings and MSAA are untested. Depth resolves dump through single-aspect depth and stencil views of the depth render target, encoding `(depth << 8) | stencil` with 24-bit unorm or 20e4 depth (as `Float32To20e4`) and the depth tiles' swapped halves. They run without validation errors in London (natively; the pipeline is also accepted by Firefox), but their output isn't verified against Vulkan yet.
- [ ] `src/graphics/webgpu`, the rest: render target aliasing and format transfers, MSAA, float24 depth, memexport, the missing texture formats, resolution scaling.
- [ ] Bring-up natively on Blade against Vulkan, then the browser (OffscreenCanvas on the device thread; phase 6 hosting).

## Phase 6: Packaging and hosting

- [x] An HTML shell page (rextools `rex wasm build --web`): the canvas and a game folder picker. Loading progress is still missing.
- [ ] Hosting with the COOP/COEP headers that `SharedArrayBuffer` needs; document a local test server setup.
- [x] Game data from the user's disk, any size, read in place: the page asks for the game's folder (`<input webkitdirectory>` in every browser; `showDirectoryPicker()` on Chromium, whose handle it keeps in IndexedDB so later visits need one permission click) and finds the folder holding `default.xex` in it. The picked `File` objects are references to the user's disk, nothing is copied or uploaded. [browser_game_files.js](../src/filesystem/devices/browser_game_files.js), a `--pre-js`, holds the program until the pick (a run dependency) and sends the list to every worker, wrapping `Worker` so later ones get it too: game threads read synchronously and never return to their event loop to receive it afterwards. [BrowserFileDevice](../include/rex/filesystem/devices/browser_file_device.h) mounts it as the game data (`game:`, `d:`) and reads slices with `FileReaderSync` on the calling thread. The XEX check at startup now goes through the VFS. Projects that mount the game folder themselves mount a `BrowserFileDevice` when `BrowserFileDevice::IsAvailable()` (Blade's `\Device\Flash`). Tested in headless Firefox with the pick simulated (Files made from Blade's data, handed to the page's handler): Blade runs from them, frame 600 as on Vulkan. `rex wasm build --web --package-data` keeps the packaged `<name>.data` for headless tests.
- [ ] Saves and settings in the browser: WASMFS or IDBFS with OPFS, so they persist (now the page's in-memory file system).
- [ ] Caching the `.wasm` file (service worker); London's is 190 MB.
- [ ] Input through SDL3 (browser Gamepad API, keyboard, mouse with pointer lock) and audio through SDL3's Web Audio backend.
- [ ] The SDL window from a worker (the main loop runs under `PROXY_TO_PTHREAD`); the software presenter paints through SDL's window framebuffer.
- [ ] A packaged wasm SDK (install/export rules).
- [ ] Browser support matrix: memory64, JSPI and WebGPU availability across Chrome, Firefox and Safari.

## Open questions

- JSPI or Asyncify for fibers, given current browser support for JSPI.
- Whether any target titles rely on hardware faults (guest SEH, null-page reads) closely enough to need emulation.
- How long a full game takes to compile for wasm on a fast x86-64 machine, and whether generated functions need splitting.
