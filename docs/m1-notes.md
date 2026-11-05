# M1 笔记

M1 = 核心库 + 命令行小工具,完成标准:与 ffmpeg 参考输出逐帧一致(设计文档 §11)。**M1 已定稿(2026-11-05)**,结论见 [m1-conclusion.md](m1-conclusion.md)。
本文记录 M1 里做的设计决定,以及它们相对设计文档 v0.2 的差异;写 `core/src/` 之前先读它。

## 进度

| 步骤 | 内容 | 状态 |
|---|---|---|
| M1.1 | 骨架:CMake、`alphaclip.h`、demux 建包索引、`info` 命令 | 完成 |
| M1.2 | VP9 解码(两个解码器)、`decode` 命令、对 ffmpeg 逐字节比对、seek | 完成,43/43 项检查通过 |
| M1.3 | 解码线程、有界队列、`ac_acquire_frame` / `ac_release_frame`、统计 | 完成,69/69 项检查通过 + 150 次压力运行无异常 |
| M1.4 | 循环(`seek(0)` + pts 偏移)、线程化播放器里的 seek | 完成,全套 99/99 项检查通过 |
| M1.5a | 对真正的核心库做畸形输入压测(`fuzz_core.py`) | 完成:3 个种子/2 个片子共 4200 例,0 崩溃、0 卡死 |
| M1.5b | Android arm64 构建核心库与 CLI | 完成:编译无警告,段对齐 16 KB;**尚未在设备上运行** |
| M1.5c | 在 Pixel 6a 上跑测试、测耗时(经用户同意) | 完成:普通构建 281/281、ASan+UBSan 构建全部通过、真机变异 2200 例无问题;耗时见结论 |
| M1+ | 补边界样本:altref 超帧、奇数分辨率、画面硬切(`samples/m1/`) | 完成,Windows 与真机都通过;关键帧错位仍没造出来 |
| M1 结论 | [m1-conclusion.md](m1-conclusion.md) | **已定稿** |

## 代码布局

```
core/include/alphaclip.h   C ABI(ac_*)
core/src/api.cpp           C ABI 实现;唯一捕获异常的地方
core/src/clip.{h,cpp}      解析后的片子 + 同步解码、seek
core/src/demuxer.{h,cpp}   libwebm webm_parser:扫描整个文件,建包索引
core/src/vp9_decoder.*     libvpx 的薄封装;native_alpha 用两个实例
core/src/player.{h,cpp}    线程化播放器:一个解码线程 + 有界队列
core/src/common.h          上限常量、内部错误类型 ac::Failure
tools/cli/main.cpp         alphaclip info / decode
core/tests/                test_vs_ffmpeg.py(对 ffmpeg 与播放器行为)、fuzz_core.py(畸形输入)
core/scripts/              build_libvpx_{win64,android_arm64}.sh、build_core_{win64,android_arm64}.sh
```

## 相对设计文档的决定与差异(需要你知道)

1. **打开时扫描整个文件,建完整的包索引,seek 不依赖 Cues。**
   核心本来就要求整个文件在内存里(`ac_open_memory` 的 `data` 必须在播放器存活期间有效),
   扫描时彩色帧体只记偏移和大小、不拷贝(alpha 载荷很小,拷贝)。因此 M0 里担心的"`webm_parser` 是流式解析器、
   容器层 seek 要自己维护 Cues 并调用 `DidSeek()`"这件事在 M1 里**不需要做**。
   代价:打开耗时与文件里的块数成正比;以后若要从磁盘流式读取,Cues 才有用。
   设计文档 R9("无 Cues 时 seek 慢或失败")对当前核心不成立,制作工具仍应写 Cues(给其他播放器用)。
2. **C ABI 在设计草案之外增加了几项**(v0.1 的 §7.2 是草案,字段名本来就说明了可能调整):
   - `ac_decode_next`:同步解码下一帧,无线程无时钟,给工具和测试用(后续 `verify` 也要用)。
   - 状态码 `AC_END`(流结束,没有更多帧)和 `AC_ERR_ARG`(参数非法);数值固定,新增只追加。
   - `ac_info.packet_count`;`ac_version()`;`ac_last_error(NULL)` 返回当前线程上最近一次失败(打开失败时没有 player)。
2b. **命令行只输出原始平面 YUV(A),没有 PNG**:设计 §11 写的是"YUV/PNG",但 PNG 需要 YUV→RGB 的矩阵和范围,它们来自 sidecar(M4)。
3. **`meta_json` 暂不支持**:传非空字符串返回 `AC_ERR_UNSUPPORTED`,传 `NULL`。sidecar 元数据(§6)要等 M4 再做,
   M1 的片子类型由 WebM 自己的 AlphaMode 和 BlockAdditional 决定。
4. **严格策略**(都返回错误而不是猜):
   - 视频轨必须是 `V_VP9`,8-bit 4:2:0;分辨率超过 8192×8192 或 7680×4320 像素返回 `AC_ERR_OOM`。
   - 有 alpha 的片子,**每个**包都要带 alpha 数据,否则 `AC_ERR_FORMAT`。BlockAddID 1 只在轨道头里 `AlphaMode=1` 时才当 alpha。
   - 第一个包必须是关键帧(有 alpha 时两路都必须是);seek 只从"两路都是关键帧"的包开始解码。
   - 时间戳必须单调不减;不支持 lacing;不支持分辨率变化(关键帧尺寸、解码出的帧尺寸都会检查)。
   - 文件必须完整解析完(`kOkCompleted`),截断的文件整体报 `AC_ERR_FORMAT`,不会"能播多少播多少"。
   - 任何 WebM 元素声明的大小超出文件末尾,立刻报错;这把解析器替我们做的内存分配限制在文件大小以内。
5. **已知局限**:VP9 的非关键帧可以带新的尺寸(帧间缩放),libvpx 会按帧头里的尺寸重新分配缓冲区;
   我们在解码后才能检查尺寸,所以一个被篡改的帧头仍可能让 libvpx 先分配一大块内存。对第一方素材不是问题,
   对不可信文件需要后续加固(M1.5 评估)。

6. **线程化播放器(M1.3)的语义**,在设计 §7.3 的基础上定下来的细节:
   - `ac_acquire_frame(clock)` 返回队列里 `pts <= clock` 的**最新**一帧,更早的算丢帧;从不阻塞。
     没有到期的帧时返回 `AC_AGAIN`("继续显示上一帧");流结束且队列取空返回 `AC_END`;解码线程出错且队列取空时返回错误码(`ac_seek` 清除)。
   - 同一时刻只能持有一帧:取下一帧前必须 `ac_release_frame`,否则 `AC_ERR_STATE`。
   - 帧缓冲池固定为 `depth + 2` 个(最多 `depth` 个在队列里、1 个在主线程手上、1 个在解码线程手上),队列深度默认 4,
     `ac_set_queue_depth` 可调(1..64,只能在 `ac_start` 之前)。
   - 线程运行期间**只有解码线程碰 Clip**;`ac_seek` 只是提交请求并清空队列(被清掉的帧计入丢帧),由解码线程执行,
     用"代数"丢弃解码途中已过时的帧。调用 `ac_start` 之后 `ac_decode_next` 返回 `AC_ERR_STATE`。
   - 新增 `ac_get_stats`:解码帧数、取走帧数、丢帧数、解码线程累计解码耗时(不含等待)、队列长度、是否已到末尾。
     设计 NFR-1 要的"解码 ms/帧、掉帧率"就是从这里来;M3 的性能基线直接用它。
   - 解码线程里的所有异常都在线程内转成错误状态,不会因为线程函数抛异常而终止进程。

7. **循环(M1.4)的语义**:
   - `ac_set_loop(player, 1)`(`ac_start` 前后都可以调,默认关):解码线程到达末尾后重新从第一帧开始,
     之后每一帧的 pts 加一个"循环周期"`ac_info.loop_period_us` = (最后一包 pts − 第一包 pts) + 一帧时长(最少 1 ms)。
     这样上一遍的最后一帧和下一遍的第一帧正好相隔一帧(样本片里是 33,333 µs),调用方的时钟永远不用回跳,循环时 `AC_END` 不会出现。
   - `ac_seek` 指向"一遍之内"的位置:它把 pts 偏移清零;循环从那一遍的剩余部分接着往下。
   - 循环只在**下一次到达末尾**时生效:关着循环播到结尾后再打开,不会重新开始(要重新开始请 `ac_seek(0)`)。
   - 一整遍都没有产生任何可显示的帧时,当作流结束,不再循环(否则解码线程会空转)。
8. **线程化 seek 的语义**:`ac_seek` 立刻返回;之后被清掉的队列帧计入丢帧;新帧在解码线程追上之后出现。
   调用方应在 seek 的同时把自己的时钟设到目标位置(绑定层的职责)。

## 测试有效性的一次验证

为了确认 seek 测试不是"永远会过",做了一次变异测试:临时去掉解码线程里"丢弃 seek 之前开始解码的过时帧"的检查,
seek-while-busy 的两组(队列深度 1 和 4)都立刻失败(报 "stale frame after the seek"),还原后全部通过。

## 畸形输入压测(M1.5a)

`core/tests/fuzz_core.py` 对 `alphaclip` 做确定性变异(固定种子,失败可复现),每例交替跑同步 `decode` 和线程化 `play`:
截断、随机字节翻转(一半落在文件头 4 KB 内)、清零/0xFF/随机区域,以及"结构命中"——把字节改在每个关心的 EBML 元素 ID
(Segment、Tracks、Cluster、SimpleBlock、BlockGroup、BlockAdditions、Cues、PixelWidth/Height、AlphaMode……)后面的大小字段上,
专门打大小解析和越界检查。退出码 0/1/2 算干净;其他退出码算崩溃;超过 20 秒算卡死。

结果:alpha 片 1500 例(种子 20261105)、alpha 片 1500 例(种子 987654)、不透明片 1200 例(种子 1),共 4200 例,
**0 崩溃、0 卡死**,绝大多数报 `AC_ERR_FORMAT`/`AC_ERR_DECODE`,少数损坏恰好不影响解码("仍能解码")。
抽样看过错误信息:单字节损坏几乎都是 libvpx 报的 `Invalid frame marker` / `Failed to decode tile data`——帧边界对不上后,
剩下的字节被当成下一帧解析而被识别出来,不是我们的检查过严。

**这不能证明内存安全**:Windows 的 llvm-mingw 没有 ASan/UBSan,没有 libFuzzer,变异是确定性的、规模有限。
还有一个已知缺口(见上面第 5 点):被篡改的帧头声明很大的尺寸时,libvpx 可能先分配很大的缓冲区,压测没有覆盖这一点。
想要更强的保证,下一步是在 Android 上用 NDK 自带的 ASan 运行时跑同一批变异,以及用 libFuzzer 做覆盖率引导的模糊测试。

## 构建与测试

```bash
sh core/scripts/build_core_win64.sh          # -> build/core-win64/bin/alphaclip.exe
sh core/scripts/build_core_android_arm64.sh  # -> build/core-android-arm64/bin/alphaclip(adb shell 用的可执行文件)
```

```bash
python -I core/tests/test_vs_ffmpeg.py --cli build/core-win64/bin/alphaclip.exe \
  --ffmpeg-dir E:/Tools/ffmpeg-9.0.2-full_build/bin --workdir <一个空目录> \
  samples/m0/alpha_native.webm samples/m0/opaque.webm samples/m1/opaque_altref.webm \
  samples/m1/opaque_odd.webm samples/m1/alpha_odd.webm samples/m1/alpha_scenecut.webm
```

同一个脚本可以通过 `--cli core/tests/adb_cli.cmd` 在连着的手机上跑(先把 Android 版 `alphaclip` 推到设备上的新目录,
细节和环境变量见 `core/tests/adb_cli.py` 的说明和 CLAUDE.md),ASan+UBSan 版同理。

测试脚本用 ffmpeg(`-c:v libvpx-vp9`,放在 `-i` 之前)独立解码出参考输出,用 ffprobe 读出每个包的时间戳和关键帧标志;
检查 `info`、全部帧逐字节一致、在片头/片中/帧间/每个关键帧及其前一刻/片尾之外的 seek、以及缺文件、空文件、随机字节、
截断文件、错误用法都干净地失败。Windows 下 Python 的 `subprocess` 需要绝对路径,脚本里已处理。

线程化播放器用 `alphaclip play` 测:
- 先等解码线程解完整个片子(`--wait-end`,队列深度 ≥ 包数),再用 1 ms 步进的时钟取帧:每帧恰好一次、按序、逐字节一致、0 丢帧。
- 粗时钟(20 ms、33.333 ms、100 ms、250 ms、1 s 步进):取出的帧和丢帧数必须与一个"最新到期帧优先"的理想模型完全一致。
- 自由运行(真实解码线程,墙上时钟 ×10)在队列深度 1/2/4/16 下:pts 严格递增、字节正确、`decoded == acquired + dropped`、不挂死。
- 解码线程仍在忙时提前关闭,不挂死、不崩溃。
- 线程化 seek:先 seek 再等整片解完、时钟从目标开始 → 恰好是目标包及之后的帧、每帧一次、0 丢帧(目标含片头之前、关键帧、关键帧前一刻、片尾之后);
  自由运行时中途向前/向后 seek,深度 1 和 4 下,seek 之后不能出现过时帧,pts 严格递增,字节正确,仍播到最后一帧。
- 循环:队列预填满后取 64 帧,第一遍与第二遍的交界处 pts 恰好差一个周期、字节一致、0 丢帧;seek 到最后一帧再循环,偏移依次累计 +P、+2P;
  自由运行跨约 2.6 遍,pts 严格递增、每帧字节按所在遍数对得上。
- 另有一轮压力运行(队列深度 1/2/3/4/8/64 × 时钟速度 1/5/20/100/1000 × 5 次 = 150 次),都无异常。
  没有 ThreadSanitizer 可用(llvm-mingw 不带),所以数据竞争只能靠这类测试和代码审查,不能算被证明。
- Windows 的 `sleep_for` 粒度是 1–15 ms:CLI 的自由运行模式因此用 `yield` 而不是睡眠,时钟用墙上时钟乘以倍数。
