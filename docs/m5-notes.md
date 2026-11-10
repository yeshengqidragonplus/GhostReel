# M5 笔记

M5 = 制作工具:`alphaclip encode` / `inspect` / `verify` 三个命令(设计文档 §3.3 的 TR-1 到 TR-3),完成标准"一条命令把素材变成可播放的包"(§11)。
结论见 [m5-conclusion.md](m5-conclusion.md)(定稿)。本文记录 M5 里做的设计决定、命令的用法和规则、量到的东西和踩过的坑。
改 `tools/cli/` 或 `core/src/{demuxer,analyze}.*` 之前先读它;更早的决定见 [m1-notes.md](m1-notes.md) 到 [m4-notes.md](m4-notes.md)。

## 代码布局(M5 新增和改动)

```
core/src/demuxer.{h,cpp}        ParseWebm(data, size, collect_cues = false):打开 collect_cues 时多记录 Segment 数据的起点、每个 Cluster 的位置、
                                每个包所在元素(SimpleBlock / BlockGroup)的位置和 Cues 里每个 CueTrackPositions;播放器不要这些,也不开
core/src/analyze.{h,cpp}        AnalyzeWebm:不解码;给出每个包在颜色流和 alpha 流上是不是关键帧、是不是"两路都是"(seekable),
                                并核对每个 Cue 指向的包。只在静态库里(CLI 链接),共享库(DLL / .so)不含它
core/src/meta.{h,cpp}           Meta 多了 schema(inspect 要打印)
tools/cli/util.{h,cpp}          UTF-8 参数(Windows 上用 CommandLineToArgvW)、UTF-8 路径的文件函数、启动外部程序(Child:可以从管道读它的 stdout)
tools/cli/{inspect,verify,encode}.cpp, commands.h    三个命令;main.cpp 只做分发(info / decode / play 不变)
tools/cli/CMakeLists.txt        CLI 现在用核心的内部头文件(core/src),Windows 上多链接 shell32
core/tests/test_authoring.py    181 项:素材、七种 encode、选项和拒绝、手工拼的坏文件、"透明处填黑"的量化
core/tests/fuzz_core.py         新增 --command authoring(对 inspect 和 verify --no-ffmpeg 做变异),Cues 的各个元素 ID 也加进了"结构命中"变异
```

核心的运行时行为没有变:`ac_*` 的 ABI、`ac_version`("0.1.0-m4")、DLL 的 14 个导出都没动。Unity 包里的 `alphaclip.dll` 和 `libalphaclip.so` 仍是 M4 同步进去的那份,**M5 没有重新同步**
(重新构建出来的二进制只因为解析器多了几个回调、`PacketRef` 多了两个不填的字段而不同)。

## 四个决定(2026-11-06,用户确认)

| 决定 | 选了 | 原因和后果 |
|---|---|---|
| 编码后端 | **外部 ffmpeg**(libvpx-vp9),工具把它当程序启动 | ffmpeg 是 GPL,所以不链接、不随包发行(CLAUDE.md 的既定规则);输入天然支持 PNG 序列、MOV(ProRes 4444)、带 VP9 alpha 的 WebM;不需要下载任何东西。代价:使用者要自己有 ffmpeg(`--ffmpeg`、环境变量 `ALPHACLIP_FFMPEG` 或 PATH)。自带 libvpx 编码器 + libwebm 封装器留作以后(需要新增读 PNG 的依赖,要下载) |
| 颜色默认怎么存 | **预乘**(`premultiplied: true`);`--straight` 保留原样 | 见下面"量到的":在柔边素材上所有存法都干净;在 1.5 像素的硬边上,"透明处填黑的直通色"放大后会出暗圈,预乘不会。**没有做扩边**(把颜色向透明区域外延伸),所以"直通色"只能保持素材原有的颜色 |
| 实现形式 | **C++,加进 `alphaclip.exe`** | 一个静态链接的 exe,使用者不用装 Python;inspect / verify 直接用核心的内部接口(包索引、关键帧) |
| `verify` 的范围 | 基本检查 + **对 ffmpeg 逐字节比对** | 比对用 ffmpeg 独立解码同一个文件(`-c:v libvpx-vp9`);没有 ffmpeg 时跳过并提示,只有明确给了 `--ffmpeg` 却起不来才算失败。没做对源素材的 PSNR(用户没选) |

## `alphaclip encode <input> --out <clip.webm> [选项]`

输入是视频文件(带不带 alpha 都行)或 printf 式的图片序列(`frames/shot_%04d.png`,必须给 `--fps`)。输出 `clip.webm` 加同名 `clip.acmeta.json`,然后跑 `verify`。

| 选项 | 默认 | 说明 |
|---|---|---|
| `--mode` | `auto` | `auto` = 输入有 alpha 就 `native_alpha`,否则 `opaque`;也可 `packed_right`、`packed_bottom`、`opaque`。要 alpha 的模式而输入没有 alpha → 拒绝;packed 模式下画面尺寸是奇数 → 拒绝(sidecar 要求 content_size 为偶数) |
| `--straight` | 关 | 颜色不乘 alpha,sidecar 写 `premultiplied: false` |
| `--fps N` | 序列必填 | 数字或分数(`30000/1001`);对序列是输入帧率,对视频是重采样 |
| `--crf N` | 24 | 0–63。**24 只是占位**,等有真实素材和目标规格(§13.2)再定;M5 的测试用 10 把编码噪声压到最小 |
| `--gop N` | 两秒的帧数 | 固定间隔的关键帧(`-g N -keyint_min N`),所以颜色流和 alpha 流的关键帧在同样的位置(R10) |
| `--matrix`、`--range` | `bt601`、`limited` | 写进码流的颜色标记和 sidecar。ffmpeg 对**没标记的 YUV 输入按 BT.601 读**;输入是 BT.709 的 YUV 而没标记时要先自己转或标记 |
| `--loop` | 关 | sidecar 的 `loop_hint` |
| `--cpu-used N`、`--threads N` | 2、核数(最多 8) | libvpx 的速度档和线程数 |
| `--ffmpeg` | 环境变量,再 PATH | 文件或文件夹;`ffprobe` 取同一个文件夹里的 |
| `--force`、`--no-verify`、`--quiet`、`--verbose` | | 已有的输出不覆盖(除非 `--force`);`--verbose` 打印 ffmpeg 命令行 |

- **颜色一律先转 RGB(`format=gbrap`)再按指定的矩阵和范围转回 YUV**:swscale 不在 YUV 和 YUV 之间换矩阵,直接转会悄悄把标记写错。预乘在 RGB 域里做(`premultiply=inplace=1`)。
- 滤镜图:`native_alpha` = `format=gbrap[,premultiply=inplace=1],scale=out_color_matrix=M:out_range=R,format=yuva420p`;
  `packed_*` = 把 `gbrap` 拆成两路,一路是颜色(`...,format=yuv420p`,alpha 被转换丢掉),一路是 `alphaextract,scale=in_range=pc:out_range=R,format=yuv420p`(灰度图当亮度存,和颜色同一个 range),再 `hstack` / `vstack`;
  `opaque` = `format=gbrap,scale=...,format=yuv420p`(alpha 直接丢掉,不是"叠在黑底上")。
- 编码参数:`-c:v libvpx-vp9 -b:v 0 -crf C -deadline good -auto-alt-ref 0 -lag-in-frames 0 -row-mt 1`,`-fps_mode cfr`,`-fflags +bitexact`(文件里不写 ffmpeg 版本,输出可重现)。
  **关掉了 alt-ref**:alpha 路和颜色路是两个独立的编码器,隐藏帧的位置不保证一样,而核心要求每个包两路同时有或同时没有输出帧;带 alpha 的 alt-ref 没试过。
- 输出先写成 `<out>.part`,`verify` 通过才改名并写 sidecar;`verify` 不通过则保留 `.part`、不写 sidecar、退出码 1。ffmpeg 失败则删掉 `.part`。
- sidecar 里除 schema 1 的字段外多写一个 `generator`("alphaclip encode, core …"),核心忽略未知键。
- WebM 输入里 ffmpeg 自带的 VP9 解码器会丢 alpha:`ffprobe` 看到 `alpha_mode=1` 的标签时,`encode` 给输入加 `-c:v libvpx-vp9`。`ffprobe` 把标签名打印成大写(`TAG:ALPHA_MODE`),比较时要转小写(踩过)。

## `alphaclip inspect <clip.webm> [--meta <json> | --no-meta]`

一行一个 `key: value`,好解析:`file`、`file_size`、`codec`、`video_size`、`frame_rate`(注明来自 DefaultDuration 还是测出来的)、`duration_us`、`packets`、`clusters`、
`keyframes`(颜色 / alpha / 两路都是)、`seekable_at_packets`(前 12 个)、`seek_granularity_frames`(seek 点之间最长的间隔)、`cues`、`alpha_stream`、`sidecar`、`metadata_schema`,
然后是核心实际怎么读它:`playable: yes`(或 `no (原因)`)以及 `alpha_mode`、`content_size`、`alpha_origin`、`premultiplied`、`color_range`、`matrix`、`loop_hint`。
sidecar 默认取同名的 `<clip>.acmeta.json`(有就用)。文件读不成 WebM → 退出码 1;核心拒绝播放不算 inspect 失败(看 `playable`)。

## `alphaclip verify <clip.webm> [--meta <json> | --no-meta] [--ffmpeg <路径> | --no-ffmpeg]`

每项检查一行 `[ ok ]` / `[warn]` / `[FAIL]` / `[skip]`,最后 `result: PASS (n warnings)` 或 `FAIL`;退出码 0(没有 FAIL)或 1。

| 检查 | 规则 |
|---|---|
| `container` | 是 WebM、V_VP9、有包 |
| `timestamps` | 严格递增(FAIL);帧间隔的最大值超过最小值的 1.5 倍 → warn(可变帧率,循环按固定帧时长算) |
| `keyframes` | 第一个包两路都是关键帧(FAIL);有的包只在一路是关键帧 → warn(核心只能 seek 到两路都是的地方),并给出最长间隔 |
| `cues` | 有视频轨的 Cues(没有 → **FAIL**,R9);每个 Cue 的簇位置真的是簇的起点、CueTime 对得上簇里的包、CueRelativePosition 对得上包的位置、那个包两路都是关键帧(R10),否则 FAIL |
| `sidecar` | 没有 sidecar → warn("auto" 模式,绑定用自己的矩阵和范围);sidecar 没写 `color_range` / `matrix` → warn |
| `open` | 核心用这个 sidecar 打开(拒绝 → FAIL,带核心的原话) |
| `decode` | 整片解码无错(FAIL 时写"after N frames") |
| `alpha` | 报告 alpha 的取值范围;alpha 到处都一样 → warn("alpha 丢了?");sidecar 写了预乘而透明像素下的颜色多数不是黑的(黑的占比 < 95%)→ warn("标志像是写反了");**没写预乘而透明像素下 ≥ 95% 是黑的 → warn("暗圈")**。只在透明像素 ≥ 1000 个时判;"透明"= alpha ≤ 2,packed 的 alpha 按 range 换算 |
| `seek` | 每个 seek 点、每个间隔的中点、片头之前、片尾之后,seek 后的第一帧和顺序解码的同一帧逐字节(FNV-1a 哈希)相同 |
| `ffmpeg` | 核心的每一帧和 ffmpeg(`-c:v libvpx-vp9 -fps_mode passthrough`)的解码逐字节相同;帧数不同、ffmpeg 报错也算 FAIL |

没有 sidecar 时核心不知道颜色是预乘的,所以一个预乘的片子在没有 sidecar 时会多一条 `alpha` 的 warn,这是对的(它会被当直通色显示)。

## 量到的:预乘 / 直通 / 透明处填黑(关掉 M4 留下的缺口)

M4 没测"直通色、透明处填黑"的素材。`test_authoring.py` 的 E 部分在 CPU 上量了(解码结果按 sidecar 还原成 RGBA,叠在白、灰、青三种背景上对精确真值;
"放大 2×"是把存储的各个平面双线性放大后再合成,对照"把预乘的真值双线性放大"):

| 素材 | 存法 | 1:1 软边带符号均值(绝对值) | 放大 2×(带符号) |
|---|---|---|---|
| 柔边(M4 的形状,14 / 10 像素的 alpha 斜坡),颜色延续 / 填黑 / 预乘 | 直通 / 直通 / 预乘 | 0.03 / 0.02 / 0.22 | −0.01 / −0.02 / −0.20 |
| 硬边(1.5 像素的斜坡),颜色延续 | 直通 | 0.05 | **0.00** |
| 硬边,颜色延续或填黑 | 预乘(两者完全一样) | 0.99 | **+1.54**(偏亮) |
| 硬边,透明处填黑 | 直通 | 0.73 | **−2.46**(暗圈) |

- 柔边素材(像 M4 的形状)怎么存都干净。硬边素材上:**直通且颜色延续到透明区域下面最准**;预乘有 1 到 1.5 个灰度级的偏亮(颜色乘 alpha 后取整、再经 4:2:0),但不会变暗;
  **直通且透明处是黑的**,放大后双线性滤波把黑色混进边缘,出暗圈。所以默认选预乘(不依赖素材的透明处是什么颜色),而素材能导出"颜色延续"的直通 alpha 时用 `--straight` 最准——
  `encode` 不会自己造这种颜色(没做扩边),只会保持素材原有的。
- 这些数都很小(最大 2.5 个灰度级,取的是软边像素的平均值),没有在 GPU 上量过放大,只是 CPU 上同样公式的结果。

## 测试方法(`core/tests/test_authoring.py`,需要 ffmpeg 和 numpy,什么都不下载)

- A 素材:把 M4 的形状(两个软边圆盘,颜色处处有定义)生成 RGBA 的 PNG 序列(同一份 `samples/m4/truth_rgba_3f.raw` 真值逐字节相同)、ProRes 4444 的 MOV、带 VP9 alpha 的 WebM、奇数尺寸序列、不透明序列。
- B `encode` 的七种(原生 alpha / packed 右 / packed 下 × 预乘 / 直通,加 auto):退出码、文件、sidecar 内容、`inspect`、工具自己的 verify、单独的 verify、
  **画面本身**(解码结果在 CPU 上还原成 RGBA,叠三种背景对真值:全图平均绝对误差 ≤ 1.0,软边带符号均值 ≤ 1.0),并有两个负向对照:把 `premultiplied` 标志写反(边缘均值 19 / 53,至少要 3)、把矩阵按 BT.709 读(平均误差 1.74,要 ≥ 1.0 且 > 4 倍正确值)。
- C 输入和选项:MOV、WebM-alpha、不透明序列、`--mode opaque` 丢 alpha、`--gop`、`--fps`(视频重采样和序列输入帧率)、`--matrix bt709 --range full`(sidecar、画面、packed 的 alpha 图亮度范围)、
  `--loop`、非 ASCII 的输入和输出路径、`--force`、`--no-verify`,以及十几种拒绝(没有 `--out`、输入不存在、序列没 `--fps`、模式 / 矩阵 / crf / gop 的坏值、没有 alpha 却要 alpha 模式、packed 奇数尺寸、找不到 ffmpeg、读不了的输入)和奇数尺寸(321×181)带 alpha 的编码。
- D `verify` / `inspect` 对**手工拼的 WebM**(测试里有一个很小的封装器:一路颜色 + 一路 alpha 的 VP9 包,按 BlockAdditions 写进去,可以任意放关键帧、Cues、时间戳):
  对齐的流干净通过(同时验证封装器本身:ffmpeg 能解出它拼的 alpha);**颜色每 30 帧、alpha 每 20 帧一个关键帧(R10 的真实样本)**:Cue 指向的包只在颜色流是关键帧 → FAIL,
  核心照样能解码、seek 退回到两路都是关键帧的点、结果仍和 ffmpeg 一致;没有 Cues、CueRelativePosition 偏一、CueClusterPosition 偏一、Cue 指向非关键帧、两个包同一个时间戳、被截断的包、alpha 恒为 255、
  预乘标志和画面矛盾(两个方向)、sidecar 错误(`keyed`)、没有 sidecar、sidecar 缺矩阵和范围、乱码文件、找不到文件、`--no-ffmpeg`、`--ffmpeg` 起不来、PATH 里没有 ffmpeg、环境变量 `ALPHACLIP_FFMPEG`。
- E 上面那张"量到的"表。
- Unity:把 `encode` 生成的四个片子(名字和 M4 的一样:`native_straight` `native_premul` `packed_right_premul` `packed_bottom_straight`,`--crf 10 --gop 30 --loop`)交给 M4 的编辑模式测试矩阵。

## 踩过的坑

- Windows 的头文件里 `ReplaceFile` 是宏、`ReadFile` / `OpenFile` 是函数:同名的自己的函数会被改名或解析错(链接时找不到 `cli::ReplaceFile`)。改名成 `MoveReplace`,管道读取显式写 `::ReadFile`。
- `std::filesystem::rename` 在 Windows 上不保证覆盖已有文件,所以那里直接用 `MoveFileExW(MOVEFILE_REPLACE_EXISTING)`。
- Windows 上 `main` 收到的是 ANSI 代码页的参数,中文路径会坏:`Utf8Args` 用 `CommandLineToArgvW(GetCommandLineW())`,文件用 `_wfopen`,子进程用 `CreateProcessW`(参数按 CRT 规则加引号)。核心的 `ac_open_file` 本来就收 UTF-8。
- Unity 测试里"对其他矩阵 / 范围 / 色度位置的参考比较"只对 Pairs 里**第一个**片子做。第一个片子是颜色很饱和的红橙色时,按错误矩阵(BT.709、全范围)解出的值越界,着色器和 CPU 参考的夹取方式在 Linear 下对不上(平均差 1.4、最大 28):
  用 M4 原版的 `native_straight.webm` 打头出现同样的两个失败,与 `encode` 无关。验收时用 `samples/m0/opaque.webm` 打头。这是那两项测试对素材内容敏感,不是着色器改了。
- 在本地没有 `numpy` 的机器上 `test_authoring.py` 跑不了(其他测试不需要它)。

## 没覆盖到的

见 [m5-conclusion.md](m5-conclusion.md)。
