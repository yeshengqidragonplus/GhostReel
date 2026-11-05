# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Current state

**M0 is complete** (conclusion finalized 2026-11-05) and **M1 (core library + CLI) is complete** (conclusion finalized 2026-11-05 in [docs/m1-conclusion.md](docs/m1-conclusion.md)): Windows and Pixel 6a match ffmpeg byte for byte, threaded player / loop / seek / fuzzing / ASan+UBSan all pass. **M2 (Unity editor prototype) is complete** (conclusion finalized 2026-11-05 in [docs/m2-conclusion.md](docs/m2-conclusion.md)): the core has a Windows shared library (`alphaclip.dll`), the Unity package `bindings/unity/package` loads it and renders the picture into a RenderTexture; the tests pass on Unity 2022.3.62f2 and, on a copy of the project, on 6000.3.2f1, each with the built-in pipeline and URP (14.0.12 / 17.3.0). Decisions and deviations are in [docs/m2-notes.md](docs/m2-notes.md); read it before touching `bindings/unity/`. M3 has not started. Design decisions and deviations from the design doc are in [docs/m1-notes.md](docs/m1-notes.md); read it before touching `core/src/`. Commits are local; nothing has been pushed. M1 produced a static library and a CLI; M2 added the Windows shared library (the Android `.so` is part of M3). The design document [plans/alphaclip-requirements-and-design.md](plans/alphaclip-requirements-and-design.md) (Chinese, draft v0.2, revised 2026-11-05 from the M0 findings) is the source of truth for requirements, architecture and milestones; read it before doing anything. The final M0 conclusion is [docs/m0-conclusion.md](docs/m0-conclusion.md) — read it too; the design doc (v0.2) already folds its corrections in (§7.1 parser choice, alpha cost, risks R2/R10); where the two differ, the conclusion has the evidence.

M0 status: Windows toolchain installed and verified; **`native_alpha` is feasible** (libwebm `webm_parser` + two libvpx decoders, bit-exact vs ffmpeg). Android arm64 also compiles with NDK r30 (empty C++17 `.so`, libvpx cross-built with NEON, and the probe; all ELF LOAD segments are 16 KB-aligned), and the probe was **run on the Pixel 6a** (Android 15, 4 KB pages) with the user's go-ahead: bit-exact vs ffmpeg and vs the Windows plain-C build, keyframe seek and clean failure from a non-keyframe both OK. What M0 did not cover (NEON-vs-C comparison on the same device, real footage, other resolutions/SoCs, multithread, memory, container-level seek) is in the conclusion's hand-off table; none of it blocks M1. `core/spikes/` (`m0_alpha_probe/`, `m0_empty_lib/`) is throwaway M0 code; the real core library is `core/{include,src}` plus the CLI in `tools/cli/`.

### Toolchain (installed in M0; nothing is on the system PATH)

Windows uses **clang via llvm-mingw** (chosen over MSVC to avoid a multi-GB VS install; MSVC is deferred until it is actually needed, e.g. Cocos static linking at M9 or Unity debugging — the core is CMake + C ABI so switching is not a rewrite). Tools live in `E:\Tools\`: `llvm-mingw-20261020-ucrt-x86_64` (clang 23.1.4), `cmake-4.4.4-windows-x86_64`, `ninja-1.13.2`, `ffmpeg-9.0.2-full_build` (GPL; external tool only — never link or ship it), `make-ndk-r23b\make.exe` (copied from Unity's NDK, needed because libvpx has no CMake build). Scripts extend PATH inside the process only.

Android uses **NDK r30** (`ndk;30.0.16248370`, clang 21.0.0) in the SDK's side-by-side location `E:\AndroidSDK\ndk\30.0.16248370` (downloaded from dl.google.com; size and SHA1 matched the SDK manifest). It is independent of Unity's bundled NDK r23b, which stays untouched: Unity only packages our prebuilt `.so`. `E:\AndroidSDK\ndk-bundle` is a legacy empty shell; do not use it. The verified zip is still in `E:\Tools\_download\ndk-r30\` and can be deleted.

Build + run the spike (from Git Bash; builds libvpx v1.17.0 decoder-only, then the probe, into `build/` which is git-ignored):

```bash
sh core/spikes/m0_alpha_probe/build_win.sh
```

The real core library and CLI (M1), Windows:

```bash
sh core/scripts/build_core_win64.sh      # builds libvpx if needed, then core + CLI -> build/core-win64/bin/alphaclip.exe
python -I core/tests/test_vs_ffmpeg.py --cli build/core-win64/bin/alphaclip.exe --ffmpeg-dir E:/Tools/ffmpeg-9.0.2-full_build/bin --workdir <empty dir> samples/m0/alpha_native.webm samples/m0/opaque.webm samples/m1/opaque_altref.webm samples/m1/opaque_odd.webm samples/m1/alpha_odd.webm samples/m1/alpha_scenecut.webm   # 281 checks
powershell -File samples/m1/gen_clips.ps1     # regenerates the M1 edge-case clips (altref superframes, odd size, scene cut)
```

```bash
python -I core/tests/fuzz_core.py --cli build/core-win64/bin/alphaclip.exe --workdir <empty dir> --cases 1500 samples/m0/alpha_native.webm   # deterministic mutation fuzzing
sh core/scripts/build_core_android_arm64.sh   # NDK r30, arm64-v8a -> build/core-android-arm64/bin/alphaclip (needs adb to run: ask the user first)
sh core/scripts/build_core_android_arm64_asan.sh   # same with AddressSanitizer + UBSan, libvpx instrumented too
```

On a device: push the binary into a NEW folder under `/data/local/tmp/` (e.g. `ghostreel_m1`), then run the host test scripts with `--cli core/tests/adb_cli.cmd` — it pushes each clip, pulls the `--out` file back and forwards stdout/exit code, so the same checks run on the phone. Env knobs: `AC_DEVICE_DIR`, `AC_TASKSET` (CPU mask), `AC_DEVICE_ENV` (e.g. `LD_LIBRARY_PATH=...` for the ASan runtimes). In Git Bash set `MSYS_NO_PATHCONV=1` first, otherwise `/data/...` in those variables becomes `C:/Program Files/Git/data/...` (the wrapper then exits 125), and `adb.exe` wants `E:/...` rather than `/e/...` paths. Clean the device folder afterwards.

The core as a shared library and the Unity prototype (M2):

```bash
sh core/scripts/build_core_win64.sh                   # also builds build/core-win64/bin/alphaclip.dll (exports 14 ac_* functions)
python -I core/tests/test_shared_lib.py --dll build/core-win64/bin/alphaclip.dll --cli build/core-win64/bin/alphaclip.exe --workdir <empty dir> samples/m0/alpha_native.webm samples/m0/opaque.webm samples/m1/alpha_odd.webm samples/m1/opaque_altref.webm   # ctypes, 78 checks
sh bindings/unity/scripts/sync_plugin_win64.sh        # copy the DLL into the Unity package; close Unity first, it locks loaded plugins
```

```powershell
powershell -File bindings/unity/scripts/run_editmode_tests.ps1 -ColorSpace Linear -Pairs "clip.webm|clip.raw;..."    # edit-mode tests (clip.raw = `alphaclip decode clip.webm --out clip.raw`)
powershell -File bindings/unity/scripts/run_editmode_tests.ps1 -ColorSpace Linear -PlayClip samples\m0\opaque.webm   # play-mode tests (real component lifecycle)
powershell -File bindings/unity/scripts/run_editmode_tests.ps1 -Demo -SnapshotDir <dir>                              # demo scene + PNG of what the camera sees
# add -Pipeline URP to any of them (switches the project, restarts the editor once); add -OfflineGuard to guarantee that nothing is downloaded
```

Python's `subprocess` on Windows needs absolute exe paths (`dir/exe` with forward slashes fails in CreateProcess). Windows `sleep_for` is 1–15 ms granular: do not pace a test loop with short sleeps.

Android arm64 spikes (Git Bash, repo root; the second script needs the first one's output):

```bash
sh core/spikes/m0_empty_lib/build_android_arm64.sh                # empty C++17 .so + llvm-readelf 16 KB alignment check
sh core/spikes/m0_alpha_probe/build_libvpx_android_arm64.sh       # libvpx v1.17.0, decoder-only, NEON
sh core/spikes/m0_alpha_probe/build_probe_android_arm64.sh        # alpha_probe against that libvpx
```

```powershell
powershell -File samples/m0/gen_test_clips.ps1        # synthetic test clips (own content only)
build\alpha_probe-win64\alpha_probe.exe samples\m0\alpha_native.webm [--dump out.yuva] [--start N] [--verbose]
python -I core/spikes/m0_alpha_probe/compare_yuva.py exact|truth ...   # bit-exact vs ffmpeg / vs ground truth
python -I core/spikes/m0_alpha_probe/fuzz_probe.py <probe.exe> <clip.webm> <workdir>
```

Gotchas already paid for — do not rediscover:
- **ffmpeg reference decode of alpha needs `-c:v libvpx-vp9` before `-i`**; the default native `vp9` decoder silently drops alpha.
- libvpx v1.17.0 on x86_64 requires nasm/yasm unless all x86 SIMD is off. nasm is not installed, so the Windows libvpx is a **plain-C build** (`--disable-mmx --disable-x86_asm --as=yasm`): bit-exact but slow — Windows timings are pessimistic until nasm is approved and installed.
- libvpx's native Windows `make.exe` needs `E:/...` mixed-style paths, not Git Bash `/e/...`. libvpx's threaded decode needs `-lpthread` (static winpthreads) on MinGW.
- libwebm: use the `webm_parser` sources (option `ENABLE_WEBM_PARSER`, upstream only builds them as an OBJECT lib — compile the sources directly). `mkvparser` cannot read BlockAdditions.
- Git Bash `PATH` entries must be `/e/...`; the `E:/...` form is only valid inside arguments, not in `PATH`.
- On-device runs: ask the user before pushing anything with adb (they approved it once, for M0). Push only into a new folder under `/data/local/tmp/`, delete it afterwards, and set `MSYS_NO_PATHCONV=1` in Git Bash or `/data/...` paths get rewritten. The Pixel 6a is 4 KB-page, cores 0–3 = Cortex-A55, 4–5 = A76, 6–7 = X1; timings jump 0.6→1.1 ms/frame unless pinned, so use `taskset` (masks `0f`/`30`/`c0`) and say which core type a number came from.
- libvpx for Android: configure's standalone-NDK mode matches the assembler name against `*clang` (to add `-c`), and the NDK's Windows per-target wrappers are `.cmd` files, so the build script generates tiny `sh` wrappers (`aarch64-linux-android24-clang[++]`) under git-ignored `build/ndk-wrap-arm64/`. Use `--enable-pic`. The default configure turns on NEON plus dotprod/i8mm/SVE/SVE2 with runtime CPU detection (the Pixel 6a has no i8mm/SVE, so those paths must not be selected there).
- Unity prototype: 2022.3.62f2 is the development baseline and `bindings/unity/TestProject` stays a 2022.3 project. **Never open it in place with Unity 6** (it upgrades the project files); to check 6000, copy `bindings/unity` elsewhere and pass `-Unity` / `-Project` to `run_editmode_tests.ps1`. The editor locks a loaded `alphaclip.dll`; sync it only with Unity closed.
- Unity packages and downloads: URP, Core RP, Shader Graph and universal-config are **built into the editor** (`<editor>\Editor\Data\Resources\PackageManager\BuiltInPackages`), no download; the public registry's version list for URP stops at 10.10.1 (14.x / 17.x are not offered there), so do not look for them in the registry. Their non-built-in dependencies (burst, mathematics, searcher, collections ...) come from the local cache `%LOCALAPPDATA%\Unity\cache\packages\packages.unity.com` when the manifest pins a cached version — on 2022.3 only: **Unity 6's UPM does not reuse that extracted cache** (it tried to download burst 1.8.21 anyway), so in the Unity 6 project copy burst is a `file:` reference to a folder copied out of the cache and the five small URP 17 dependencies (mathematics 1.3.2, collections 2.4.3, searcher 4.9.3, nuget.mono-cecil 1.11.4, test-framework.performance 3.0.3, 3.75 MB, downloaded once with the user's approval) are local `file:` tgz references; write manifests without a BOM (PowerShell 5.1 `Set-Content -Encoding UTF8` adds one). `run_editmode_tests.ps1 -OfflineGuard` starts the editor with a dead HTTP(S) proxy so a download attempt fails visibly instead of happening; use it whenever nothing may be downloaded, and ask the user before anything is (state package, version, source `download.packages.unity.com`, size).
- Unity batch-mode facts paid for in M2: play-mode tests must be launched without `-quit` (the test driver exits with `EditorApplication.Exit`); `WaitForEndOfFrame` never resumes; edit mode does not send `OnDestroy`; `Process.PrivateMemorySize64` does not move under Mono (use `GetProcessMemoryInfo`) and process private bytes swing ±150 MB because of the graphics driver, so judge leaks with exact counters, Unity's profiler counters and a deliberate-leak control; `Time.unscaledTimeAsDouble` runs ~1.3x faster than wall time in batch-mode play mode (1.6x on Unity 6) and the player's clock starts at the first `Update` after `Play()`/`Seek()`, not at the call (a mid-frame hitch otherwise fast-forwards the start of the clip: seen as a 1.3 s lag on Unity 6 + URP 17); Windows PowerShell 5.1 has no `$PSScriptRoot` in `param()` defaults and its variables are case-insensitive (a local `$method` silently overwrote the `-Method` parameter); Python's default file encoding here is GBK, so open UTF-8 files with `encoding="utf-8"`. The WebFetch summariser often omits API signatures: fetch the specific method page.
- Unity 2022.3 officially supports only its bundled NDK r23b (a custom NDK is possible under Preferences > External Tools, but unsupported). Unity itself supports 16 KB pages from 2022.3.56f1; it also needs Burst ≥ 1.8.21 and every native plugin rebuilt with 16 KB alignment. Forum reports on 2022.3.71f1 still being rejected by Google Play conflict, so verify on a real AAB at M3. Android API level 24 in the spike scripts is an arbitrary smoke value (minimum API level is undecided).

The product is a cross-engine "transparent video" plugin: standard VP9/WebM video + alpha data, decoded in software by one C++ core, composited in a GPU shader, with thin bindings for Unity, Godot 4 and Cocos Creator 3.x, plus offline authoring tools. The repo is named GhostReel; the doc calls the project **AlphaClip**, which is only a placeholder name (naming is an open decision, §13.2). The design doc says to move itself into `docs/` once the real project layout is created (Appendix B).

## Architecture (big picture)

Three layers, one rule each:

1. **C++17 core (`core/`, C ABI `ac_*`)** — bytes in, YUV(A) frames out. It must **never touch a graphics API**. Pipeline: libwebm `webm_parser` demux → decode thread (libvpx; **two decoder instances** for `native_alpha`) → bounded ring queue (default 4 frames). The main thread calls `ac_acquire_frame(clock_us)`, which returns the newest frame with `pts <= clock` and drops older ones. The clock is supplied by the binding (engine game time; audio clock later). Looping = `seek(0)` plus a pts offset so the clock stays continuous. Every function returns an `ac_status` — no exceptions across the ABI, and malformed input (corrupt WebM, oversized dimensions, bad metadata) must return an error, never crash.
2. **Shaders (`shaders/`)** — a single GLSL ES 3.0 master source, with a generator script emitting Unity ShaderLab/HLSL, Godot Shading Language, Cocos `.effect` and the WebGL preview version. Never hand-edit the generated variants; golden-image tests per engine guard against drift.
3. **Engine bindings (`bindings/{unity,godot,cocos}`)** — each does only three things: hand bytes to the core, upload Y/U/V[/A] planes as textures, draw with the shader. Shared behavior is specified in §9.4 (destroy ⇒ `ac_close`, global instance cap with degradation to a static first frame, pause decode on app background).

Rendering contract: planes are single-channel `R8` textures (U/V at half resolution) and must be **non-sRGB**. Output is **premultiplied alpha** (blend `One OneMinusSrcAlpha`). Edge post-processing (shrink/feather/denoise) is a separate optional pass, off by default. Unity Linear color space needs an explicit sRGB↔linear conversion in the `keyed` path.

### Alpha modes (selected via sidecar metadata)

`opaque`, `packed_right` / `packed_bottom` (alpha as grayscale next to the picture, so `content_size` ≠ video size), `native_alpha` (WebM BlockAdditional, yuva420p — **verified feasible in M0**: alpha is a second VP9 stream in BlockAdditional (BlockAddID 1), its Y plane is the full-range 0–255 alpha, and the alpha stream costs ≈ +69% decode time per frame on the Windows plain-C build but ≈ +40% (+39…+44% across Cortex-X1/A76/A55) on the Pixel 6a NEON build (640×360 synthetic clip, single thread; per-frame times jitter ±25% on that phone, so treat them as ranges, see `docs/m1-conclusion.md`); the colour and alpha streams' keyframes must align, which the authoring tool's `verify` must check), and `keyed` (green-screen video + parameters + 32×32×32 LUT laid out as a 1024×32 strip, manually trilinear-sampled in the shader).

### File format (self-designed, v0.1)

The `.webm` is always a plain standard VP9 file playable anywhere. Alpha/color parameters live in same-named sidecars: `clip.acmeta.json` (required even for `opaque`; versioned via `schema`, and the core rejects unknown versions), plus `clip.aclut.png` for `keyed`. Field names may change after M1; the core's schema validation is authoritative. The authoring tool should still write WebM **Cues** (other players need them to seek), but the M1 core builds a full packet index at open and does not depend on them (see `docs/m1-notes.md`).

## Working rules specific to this project

- **Work milestone by milestone** (M0–M10, §11); each step must be independently verifiable and stoppable. Do only the milestone the user asked for. **M0** (toolchain setup + libvpx WebM-alpha feasibility, *no engine bindings*) is finished and its conclusion is final; **M1** (core library + command-line tool, frame-exact vs ffmpeg) is finished and its conclusion is final. **M2** (Unity editor prototype, Windows editor, built-in pipeline and URP) is finished and its conclusion is final. **M3** (Android on a real device) starts only when the user asks; before it, the user should decide whether it targets Unity 6000 or stays on 2022.3 (m2-conclusion's hand-off table lists what that touches).
- **Ask before downloading anything** — state filename, source and size and wait for confirmation (the user's explicit standing requirement for this project).
- **Verify engine API names against the official docs for the target engine version** rather than relying on memory (texture upload, native plugin, GDExtension/`godot-cpp` version match, Cocos native extension). The doc deliberately specifies structure, not API names.
- **Do not copy the Leviathan game's format, code, shaders or assets.** The design only borrows generic ideas (chroma key + LUT, side-by-side alpha). `E:\MergeIsland\exported_videos\` holds third-party videos: never put them in the repo and never use them as test material — generate test clips ourselves.
- **Licensing:** permissive dependencies only (libvpx, libwebm, optional dav1d). FFmpeg only if unavoidable, dynamically linked. H.264/HEVC are out of scope. Legal sign-off on VP9 is the company's call, not ours.
- **Android native libs** must satisfy Google Play's 16 KB page-size requirement (check the current official docs and NDK version at M3). NDK r30 links with 16 KB (`0x4000`) LOAD-segment alignment by default (checked on the M0 spikes with `llvm-readelf -lW`); the APK-level zip alignment and the Play Console check are still M3's job.
- **Don't assume unresolved decisions** (§13.2): project name, license, real-alpha vs green-screen asset mix, target resolution/fps/concurrency, audio, minimum Unity version, first-phase platforms (proposed: Android arm64 + Windows editor).
- Testing approach once code exists: decoded YUV compared frame-by-frame against ffmpeg reference output (MD5/PSNR); per-engine shader golden images (both Linear and Gamma in Unity); performance baseline on a Pixel 6a (decode ms/frame, upload ms/frame, main-thread time, memory).

## Local environment (as of 2026-11-04, from the design doc's Appendix C)

Unity via Hub at `D:\Program Files\Unity\Hub\Editor`: 2022.3.20f1c1 and 2022.3.62f2 (both bundle Android NDK r23b `23.1.7779620`, which Unity 2022.3 officially requires; 2022.3.20f1c1 predates Unity's 16 KB support, 2022.3.62f2 is past 2022.3.56f1) plus `Unity 6000.2.10f1` and `Unity 6000.3.2f1` (these two folders carry a "Unity " prefix with a space, e.g. `D:\Program Files\Unity\Hub\Editor\Unity 6000.3.2f1\Editor\Unity.exe`; no Android module installed; the user's real projects are on Unity 6000); Cocos Creator 3.8.8 at `C:\ProgramData\cocos\editors\Creator\3.8.8`; Godot 4.7 at `D:\Tools\godot-4.7` (a 4.7.2 build is in `E:\Tools`); Android SDK at `E:\AndroidSDK` (NDK r30 installed under `ndk\30.0.16248370` during M0; `ndk-bundle` is an empty shell); Pixel 6a connected over adb (`adb devices` lists one device, checked 2026-11-05); Java 11, Python 3.12, Node and .NET available. The author's own Unity projects already use YooAsset. Free space: C: ~13 GB, D: ~28 GB, E: ~128 GB — put big things on E:.

Target layout (Appendix B). Created so far: `docs/` (only `m0-conclusion.md`; the design doc has not been moved there), `core/spikes/{m0_alpha_probe,m0_empty_lib}` (throwaway M0 code, not in Appendix B), `core/third_party/{libvpx,libwebm}` (git submodules, shallow, pinned to libvpx v1.17.0 `6df3ec3` and libwebm-1.0.0.32 `f2a982d`; a fresh checkout needs `git submodule update --init`, which downloads ~27 MB — ask first; the submodule choice was a default, vendoring or ignoring is still possible before the first push), `samples/{m0,m1}/`, `core/{include,src,scripts,tests}`, `tools/cli`, `bindings/unity`. Not yet created: `shaders/`, `bindings/{godot,cocos}`, `tools/preview`.
