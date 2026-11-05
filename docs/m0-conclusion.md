# M0 结论

状态:**已定稿**(2026-11-05,经用户确认)· 对应设计文档 §11 的 M0 与 §13.1 的风险 R2 · 本阶段没有写任何引擎绑定。

M0 的完成标准是"能编译出 Windows 和 Android arm64 的空库;alpha 可行性有结论",两条都已达到:
Windows 与 Android arm64(NDK r30)都编出了空库,`native_alpha` 的可行性在 Windows 和 Pixel 6a 真机上都得到验证。

## 一句话结论

**`native_alpha` 可行,R2 可以关闭。** 只用 libwebm + libvpx(不用 FFmpeg)就能从 WebM 读出 VP9 alpha:
解封装取出每个 BlockGroup 里的 BlockAdditional,再用**第二个** libvpx 解码器实例解码这路数据,其 Y 平面就是 alpha。
在自己生成的测试片上,结果与 ffmpeg 的 libvpx-vp9 解码**逐字节一致**(Y/U/V/A 四个平面,60 帧)。
`packed_*` 不需要作为 `native_alpha` 的降级方案启用,但仍按原计划作为"最简单可用"的模式保留。

## 证据

测试片是用 ffmpeg lavfi 合成的(`samples/m0/gen_test_clips.ps1`):640×360、30fps、2 秒、60 帧,
alpha 为沿圆周运动的软边圆盘(0 / 255 / 40px 渐变边),每 30 帧一个关键帧。没有使用任何第三方素材。

| 检查 | 结果 |
|---|---|
| ffprobe 独立确认 | 每个视频包都带 `Matroska BlockAdditional`,流标签 `alpha_mode=1`。ffmpeg 对 yuva420p 会自动写 AlphaMode。 |
| 我们的探针解封装 | `TrackEntry/Video/AlphaMode = 1`,60 个包全部带 alpha 数据(0 个 SimpleBlock,全是 BlockGroup),Cues 有 2 个 CuePoint,无其他 BlockAddID,无 lacing |
| 与 ffmpeg 参考输出比对 | ffmpeg 用 `-c:v libvpx-vp9` 解码为 yuva420p;**Y/U/V/A 四平面 × 60 帧全部逐字节相同** |
| 与已知 alpha 真值比对 | 平均绝对误差 0.080,最大误差 6,PSNR 58.04 dB;真值 0 的像素解码为 0–5,真值 255 的为 249–255 |
| alpha 的取值范围 | **全范围 0–255**,Y 平面直接就是 alpha,没有 16–235 的缩放 |
| 模拟 seek(两路用全新解码器,从关键帧包 30 开始) | 后 30 帧四平面与参考输出逐字节相同 |
| 从非关键帧开始 | libvpx 对每一帧返回错误码,**没有崩溃**;因此 seek 只能落在 Cues 指向的关键帧上 |
| 两路关键帧位置 | 彩色与 alpha 都是 0 和 30,对齐 |
| 畸形输入 | 150 个整文件变异(截断/随机翻转/区域清零)+ 100 个"只损坏 alpha 流"的变异:**0 崩溃、0 卡死**,全部干净失败(1 个区域清零例仍能解码) |
| 不透明对照片 | 0 个 alpha 包,60 个 SimpleBlock,判定为 opaque,60 帧解码正常 |

## 需要写进设计的点

1. **用 `webm_parser`,不是 `mkvparser`。** libwebm 传统的 `mkvparser` 完全不暴露 BlockAdditions(只有写入端 `mkvmuxer` 支持);
   事件回调式的 `webm_parser` 才有 `BlockGroup.additions` 和 `Video.alpha_mode`。它是 libwebm 的 CMake 选项 `ENABLE_WEBM_PARSER`(默认关),
   且上游只把它建成 OBJECT 库,我们要自己编它的源码。设计文档 §7.1 "libwebm 解封装"已据此改为"libwebm `webm_parser`"(v0.2)。
2. **`native_alpha` = 两个解码器实例。** libvpx 本身不解析容器,也不会从一路码流里吐出 alpha。核心需要在每个 `native_alpha` 实例里开两个 VP9 解码器。
3. **成本不是白送。** 在本次测量里(单线程、纯 C 构建、640×360、很平滑的合成 alpha;同一条件重复 5 次),
   alpha 路 0.65–0.67 ms/帧,彩色路 0.95–0.98 ms/帧,**alpha 约增加 69% 的解码时间**。
   alpha 码流本身极小(关键帧约 1.4 KB,inter 帧 35–107 字节、平均 59;60 帧合计 6.3 KB,彩色 251 KB),
   说明开销主要在整帧重建而不是熵解码。
   这对 R1(低端 Android 软解)是实质性的:并发上限(FR-10)要按"每个 native_alpha 实例约 1.7 个解码器"来估。**内存占用未测**。
   **注意:+69% 是 Windows 纯 C 构建的数。** 在 Pixel 6a 真机(NEON,见下)上同一片子测得 alpha 比彩色多 +32%(大核)到 +45%(小核),
   所以"1.7 个解码器"不能直接套到手机上;这个比例取决于构建、CPU 和 alpha 内容(本片的 alpha 是平滑的合成图)。
4. **关键帧必须在两路对齐。** 本样本对齐是因为用了固定 GOP;**ffmpeg 并没有对此做保证,场景切换等情况下两路能否错位未测。**
   若错位,seek 到彩色关键帧时 alpha 路会无法解码。M5 的制作工具 `verify` 应检查"每个 Cues 指向的块,彩色与 alpha 两路都是关键帧"。
5. **码流里的色彩信息是"未指定"。** libvpx 对这批 ffmpeg 编码的片子报告 `color space = unknown`、`range = studio(默认)`。
   这支持 §6 的设计:`matrix` / `color_range` 必须放在 sidecar 的 `acmeta.json` 里,不能依赖码流。
6. **ffmpeg 对照的坑。** ffmpeg 默认的原生 vp9 解码器**会忽略 alpha**(ffprobe 报 `yuv420p`)。
   生成 alpha 参考输出必须在 `-i` 之前指定 `-c:v libvpx-vp9`。M1 的"与 ffmpeg 逐帧对比"要沿用这个做法。
7. **非关键帧 seek 的行为是"返回错误"而不是崩溃**,符合"畸形输入不崩溃"的底线;核心的 seek 逻辑必须只落在 Cues 关键帧上。

## 工具链现状

| 工具 | 版本 | 位置 | 来源与校验 |
|---|---|---|---|
| llvm-mingw(clang + lld + libc++ + mingw-w64 UCRT) | 20261020(clang 23.1.4) | `E:\Tools\llvm-mingw-20261020-ucrt-x86_64` | github.com/mstorsjo/llvm-mingw,SHA256 与 winget 清单一致 |
| CMake | 4.4.4 | `E:\Tools\cmake-4.4.4-windows-x86_64` | github.com/Kitware/CMake,SHA256 一致 |
| Ninja | 1.13.2 | `E:\Tools\ninja-1.13.2` | github.com/ninja-build/ninja,SHA256 一致 |
| ffmpeg(full build,GPL-3.0) | 9.0.2 | `E:\Tools\ffmpeg-9.0.2-full_build` | github.com/GyanD/codexffmpeg,SHA256 一致。仅作外部工具,不链接、不进仓库 |
| GNU make | 4.3.90 | `E:\Tools\make-ndk-r23b\make.exe` | **没有下载**:复制自本机 Unity 2022.3.62f2 自带 NDK r23b |
| Android NDK | r30(`ndk;30.0.16248370`,clang 21.0.0) | `E:\AndroidSDK\ndk\30.0.16248370` | dl.google.com/android/repository/android-ndk-r30-windows.zip,727,980,788 字节,大小与 SHA1 `9bf167a1…9408` 与 SDK 官方清单一致。与 Unity 自带的 r23b 并存,互不影响 |
| libvpx | v1.17.0(`6df3ec3`) | `core/third_party/libvpx` | github.com/webmproject/libvpx,浅克隆,24.2 MB(不含 .git) |
| libwebm | libwebm-1.0.0.32(`f2a982d`) | `core/third_party/libwebm` | github.com/webmproject/libwebm,浅克隆,2.7 MB(不含 .git) |

系统 PATH 没有改动。构建脚本在进程内临时扩展 PATH。

Windows 冒烟编译已通过:C++17 的 C ABI DLL(含 `std::string/vector/unique_ptr`、结构化绑定、`if constexpr`)由 clang 链接成功,
导出函数可被 exe 调用;`-static` 后只依赖 KERNEL32 与 UCRT api-set,没有 libc++ / libunwind / winpthread 的额外 DLL。

## 构建中踩到的坑(已在 `core/spikes/m0_alpha_probe/build_win.sh` 里处理)

- libvpx v1.17.0 在 x86_64 上**必须有 nasm/yasm,除非关掉全部 x86 SIMD**:configure 的汇编器检测是无条件的,
  且 `emms_mmx.asm` 只要开着 MMX 就会被编译,与 `--disable-x86_asm` 无关。当前用 `--disable-mmx --disable-x86_asm --as=yasm` 构建成**纯 C 版本**,
  解码输出与 SIMD 版逐位一致,但**速度偏慢**,上面 Windows 的耗时数字因此偏悲观。装 nasm 后(1.2 MB,需用户批准)可重测。
- libvpx 没有 CMake 构建,只有 `configure` + `make`;Windows 上用 Git Bash 的 `sh`/`perl` + 借来的原生 `make.exe`。
  原生 make 不认识 Git Bash 的 `/e/...` 路径,要给 configure 传 `E:/...` 形式的源码路径。
- libvpx 的多线程解码依赖 pthread,MinGW 下要显式链接 `pthread`(静态 winpthreads)。
- libvpx 的 `--disable-vp8 --disable-vp9-encoder` 之后静态库约 545 KB(Windows 纯 C 版),Android arm64 版约 577 KB(含 NEON)。
- Git Bash 的 `PATH` 里只能写 `/e/...`,`E:/...` 只在参数里有效。
- Android 交叉编译 libvpx:它的 NDK standalone 模式会按 `*clang` 匹配汇编器名字来加 `-c`,而 NDK 在 Windows 上的按目标包装器是 `.cmd` 文件,
  所以脚本在 `build/ndk-wrap-arm64/` 下生成了两个很小的 sh 包装器(`aarch64-linux-android24-clang[++]`);还需要 `--enable-pic`。
  默认配置会打开 NEON 以及 dotprod/i8mm/SVE/SVE2(运行时探测);Pixel 6a 没有 i8mm/SVE,这些路径在它上面不应被选中。

## 没有覆盖到的(请不要把上面的结论外推到这些)

- **Pixel 6a 上只测了这一台设备、这一个 640×360 合成片。** 其他分辨率、真实素材、其他 SoC(尤其更低端的 Android 机)没有数据。
  也没有做 NEON 与纯 C 在同一台设备上的对比,所以不知道 NEON 带来多少加速。
- 16 KB 只验证了 ELF 的 LOAD 段对齐;APK 里 `.so` 的 zip 对齐、Google Play 控制台检查、Unity 打包链路都没有验证(属于 M3)。
- x86 SIMD 没有跑过(Windows 上只跑过纯 C 构建)。Pixel 6a 上跑的是开了运行时 CPU 探测的 NEON 构建:输出正确,
  但未单独证明具体选中了哪些内核(没有崩溃说明没有误选设备不支持的 i8mm/SVE)。
- 测试片是合成的、alpha 很平滑。真实素材(头发、粒子、高频半透明)的 alpha 质量和码率**未测**。
- **容器层的 seek**(用 Cues 位置 + `WebmParser::DidSeek()` 重新开始解析)没有实现,只验证了解码器层。
  `webm_parser` 是流式解析器,seek 需要我们自己维护 Cues 表,比 `mkvparser` 的 `Cues::Find` 麻烦,属于 M1 的工作量。
- **循环**(seek(0) + pts 偏移)的时钟逻辑属于 M1;这里只证明了"从关键帧重新开始解码"是对的。
- 模糊测试只是 250 例确定性变异,无 ASan、无 libFuzzer,**不能替代**对最终核心库的正式模糊测试。
- 多线程解码(`threads > 1`)、内存占用、两个解码器同时的缓存压力均未测。
- 只测了 `alpha_mode = 1`、BlockAddID = 1 的 ffmpeg 产物;其他编码器产出的 alpha WebM 未测。

## Android arm64(M0 完成标准的后半条)

用 NDK r30 + CMake(`android.toolchain.cmake`,`arm64-v8a`,`android-24`,`c++_static`)得到:

| 目标 | 结果 |
|---|---|
| 空库 `libac_empty.so`(C++17、标准库、C ABI 导出 `ac_spike_*`) | 编译链接成功,ELF 为 AArch64;只依赖 `libm`/`libdl`/`libc`,**没有 `libc++_shared.so`**(静态 libc++,便于放进 Unity) |
| 16 KB 页大小 | 三个 LOAD 段的 `Align` 都是 `0x4000`(未传任何额外链接参数,r30 默认如此);`alpha_probe` 可执行文件同样如此 |
| libvpx v1.17.0 交叉编译(decoder-only,`arm64-android-gcc`) | 一次通过,`libvpx.a` 577 KB;`HAVE_NEON=1`,22 个 NEON 目标文件,运行时 CPU 探测打开 |
| `alpha_probe` for Android arm64(libwebm `webm_parser` + 上面的 libvpx) | 编译链接成功 |

### Pixel 6a 真机验证(2026-11-05,经用户同意后用 adb 推送)

设备:Pixel 6a,Android 15(SDK 35),arm64-v8a,Tensor,8 核,**页大小 4 KB**,CPU 特性含 `asimddp`、不含 i8mm/SVE。
把 `alpha_probe`(arm64)和两个测试片推到 `/data/local/tmp/ghostreel_m0`(本机与设备上 `alpha_native.webm` 的 SHA-256 一致),运行后拉回输出;测完已把该目录从设备上删除。

| 检查 | 结果 |
|---|---|
| 60 帧解码 | 彩色 60/60、alpha 60/60,0 个错误,关键帧 0 和 30 两路对齐 |
| 与 ffmpeg 参考输出比对 | Y/U/V/A 四平面 × 60 帧**逐字节相同** |
| 与 Windows 纯 C 构建的输出比对 | 四平面 × 60 帧**逐字节相同**(NEON 构建与纯 C 构建输出一致) |
| 与 alpha 真值比对 | 平均绝对误差 0.080,最大误差 6,PSNR 58.04 dB,与 Windows 结果相同 |
| seek:从关键帧 30 起用全新解码器 | 后 30 帧四平面与参考输出逐字节相同 |
| seek:从非关键帧 31 起 | 程序以退出码 1 干净失败,没有崩溃 |
| 不透明对照片 | 60/60 帧解码正常,判定为 OPAQUE |
| 页大小 | 设备是 4 KB 页,16 KB 对齐的二进制能正常运行(与 Unity 论坛官方说明"16 KB 对齐的构建在 4 KB 设备上也能跑"一致) |

**单线程解码耗时**(640×360,60 帧,每种配置 5 次,单位 ms/帧;用 `taskset` 固定到某一类核):

| 核 | 最高频率 | 彩色 | alpha | alpha 相对彩色 |
|---|---|---|---|---|
| 大核 Cortex-X1 | 2.80 GHz | 0.61–0.64(1 次 0.96) | 0.20–0.21(1 次 0.31) | 约 +32% |
| 中核 Cortex-A76 | 2.25 GHz | 0.94–1.06 | 0.37–0.42 | 约 +39% |
| 小核 Cortex-A55 | 1.80 GHz | 2.31–2.41 | 1.02–1.09 | 约 +45% |
| 不固定(系统调度) | — | 0.61–1.07 | 0.20–0.42 | 随调度落在哪类核而变 |

读法与限制:
- 彩色加 alpha 两路合计,大核约 0.83 ms/帧,小核约 3.4 ms/帧。30 fps 的单帧预算是 33 ms,本片在这个分辨率下余量很大。
  **不要把它外推到 1080p 或多路并发**:目标分辨率、帧率、并发数(§13.2)都还没定,耗时也没有按像素数验证过。
- 不固定核时同一程序的耗时在 0.6 到 1.1 ms 之间跳动,是被调度到了不同类型的核上。
  实际性能基线(M3)必须说明在哪类核上测、是否固定。
- 本片 alpha 是平滑合成图,真实素材(头发、粒子)的耗时会不同。
- 只测了单线程;多线程解码、内存占用、两个解码器同时的缓存压力仍然没有数据。

## 关于 NDK 与 Unity(能否用 r30 替换 Unity 自带的 r23b)

- 我们**自己的核心库**用 r30 编,和 Unity 用哪个 NDK 无关:Unity 只负责把预编译好的 `.so` 打进 APK。不要去改 Unity 安装目录里的 NDK。
- Unity 2022.3 官方只支持它自带的 r23b(`Preferences > External Tools` 可以指向别的 NDK,但不受支持,出错后果没有资料)。
- Unity 自己对 16 KB 的支持取决于 Unity 版本:2022.3 从 **2022.3.56f1** 起支持,另需 Burst ≥ 1.8.21,且所有原生插件要按 16 KB 重编。
  本机的 2022.3.62f2 满足版本条件,2022.3.20f1c1 不满足。论坛上有人报告 2022.3.71f1 仍被 Google Play 拒绝,说法不一,**M3 要在真实 AAB 上验证**。
- 小结:编**我们的**库用 r30(默认 16 KB 对齐,M0 已验证);Unity 自己的 16 KB 支持只取决于 Unity 版本,与我们用哪个 NDK 无关。

## 遗留事项(带入后续里程碑,都不阻塞 M1)

| 事项 | 归属 | 说明 |
|---|---|---|
| 容器层 seek(Cues 表 + `WebmParser::DidSeek()`)与循环的时钟逻辑 | M1 | 见"没有覆盖到的" |
| 与 ffmpeg 逐帧对比沿用 `-c:v libvpx-vp9` | M1 | 见"需要写进设计的点"第 6 条 |
| 对真正的核心库做正式模糊测试(ASan / libFuzzer) | M1 起 | M0 只有 250 例确定性变异 |
| 检查彩色与 alpha 两路关键帧对齐,包括场景切换 | M5 的 `verify` | M0 的样本是固定 GOP |
| 真实 AAB 上验证 16 KB:APK 内 `.so` 的 zip 对齐、Play 控制台检查、Unity ≥ 2022.3.56f1、Burst ≥ 1.8.21 | M3 | 论坛对 2022.3.71f1 的报告互相矛盾 |
| 性能基线:按核类型固定(`taskset`)、多线程、内存、真实素材、目标分辨率与并发 | M3 | 目标规格(§13.2)未定,M0 的耗时不可外推 |
| NEON 与纯 C 在同一台设备上的对比;安装 nasm(约 1.2 MB,需用户批准)后重测 x86 SIMD | 可选 | 只影响"加速多少"的判断,不影响正确性 |
| 设计文档按本结论修订:§7.1 改为 `webm_parser`;R2 关闭;并发估算要区分平台 | 文档 | **已完成**:设计文档升为 v0.2(§4、§5、§7.1、§7.3、§11、§13、附录 C),改动处标了「M0」 |
| `core/third_party/{libvpx,libwebm}` 的提交方式 | 已处理,可改 | 用户未另行指定,首个提交按默认作为 **git submodule**(浅克隆,固定在 libvpx v1.17.0 `6df3ec3`、libwebm-1.0.0.32 `f2a982d`)。想改成 vendoring 或忽略,在 push 之前重做即可 |
| §13.2 的未决事项:项目名、许可证、素材是真 alpha 还是绿幕、目标分辨率/帧率/并发、音频、最低 Unity 版本、第一阶段平台 | 用户决定 | M0 没有替这些做假设 |

## 产物清单

- `core/spikes/m0_alpha_probe/`:`alpha_probe.cpp`(验证程序)、`CMakeLists.txt`、`build_win.sh`(libvpx + 探针的完整构建)、
  `build_libvpx_android_arm64.sh` / `build_probe_android_arm64.sh`(Android arm64 版)、
  `compare_yuva.py`(逐字节比对 / 对真值比对)、`fuzz_probe.py`(确定性变异)
- `core/spikes/m0_empty_lib/`:`ac_empty.cpp`、`CMakeLists.txt`、`build_android_arm64.sh`(空库 + 16 KB 对齐检查)
- 仓库外:`E:\Tools\_download\ndk-r30\android-ndk-r30-windows.zip`(已校验、已解压使用,可删除)
- `samples/m0/gen_test_clips.ps1`:生成测试片(输出 `alpha_native.webm`、`opaque.webm`、`alpha_truth.gray`)
- `.gitignore`:忽略 `build/` 和可再生的 `alpha_truth.gray`;`.gitattributes`:`*.sh` 强制 LF(Windows 的 `core.autocrlf=true` 否则会给脚本加 CRLF),`*.webm` 按二进制处理
- 以上已在首个提交里提交(本地,尚未 push)。
