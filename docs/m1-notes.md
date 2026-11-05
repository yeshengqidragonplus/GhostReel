# M1 笔记(进行中)

M1 = 核心库 + 命令行小工具,完成标准:与 ffmpeg 参考输出逐帧一致(设计文档 §11)。
本文记录 M1 里做的设计决定,以及它们相对设计文档 v0.2 的差异。M1 结束时会整理成 M1 结论。

## 进度

| 步骤 | 内容 | 状态 |
|---|---|---|
| M1.1 | 骨架:CMake、`alphaclip.h`、demux 建包索引、`info` 命令 | 完成 |
| M1.2 | VP9 解码(两个解码器)、`decode` 命令、对 ffmpeg 逐字节比对、seek | 完成,43/43 项检查通过 |
| M1.3 | 解码线程、环形队列、`ac_acquire_frame` / `ac_release_frame` | 未开始 |
| M1.4 | 循环(`seek(0)` + pts 偏移)、线程化播放器里的 seek | 未开始 |
| M1.5 | 畸形输入加固与模糊测试、Android arm64 构建并在 Pixel 6a 上跑 | 未开始 |

## 代码布局

```
core/include/alphaclip.h   C ABI(ac_*)
core/src/api.cpp           C ABI 实现;唯一捕获异常的地方
core/src/clip.{h,cpp}      解析后的片子 + 同步解码、seek
core/src/demuxer.{h,cpp}   libwebm webm_parser:扫描整个文件,建包索引
core/src/vp9_decoder.*     libvpx 的薄封装;native_alpha 用两个实例
core/src/common.h          上限常量、内部错误类型 ac::Failure
tools/cli/main.cpp         alphaclip info / decode
core/tests/                test_vs_ffmpeg.py
core/scripts/              build_libvpx_win64.sh、build_core_win64.sh
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

## 构建与测试

```bash
sh core/scripts/build_core_win64.sh          # -> build/core-win64/bin/alphaclip.exe
```

```bash
python -I core/tests/test_vs_ffmpeg.py --cli build/core-win64/bin/alphaclip.exe \
  --ffmpeg-dir E:/Tools/ffmpeg-9.0.2-full_build/bin --workdir <一个空目录> \
  samples/m0/alpha_native.webm samples/m0/opaque.webm
```

测试脚本用 ffmpeg(`-c:v libvpx-vp9`,放在 `-i` 之前)独立解码出参考输出,用 ffprobe 读出每个包的时间戳和关键帧标志;
检查 `info`、全部帧逐字节一致、在片头/片中/帧间/每个关键帧及其前一刻/片尾之外的 seek、以及缺文件、空文件、随机字节、
截断文件、错误用法都干净地失败。Windows 下 Python 的 `subprocess` 需要绝对路径,脚本里已处理。
