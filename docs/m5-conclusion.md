# M5 结论

状态:**草稿,等用户定稿**(2026-11-06 写;定稿前不当作最终结论)· 对应设计文档 §11 的 M5:制作工具 `encode` / `inspect` / `verify`,完成标准"一条命令把素材变成可播放的包"。
设计决定、命令的用法和规则、量到的数据见 [m5-notes.md](m5-notes.md)。

## 一句话结论

**M5 的完成标准在 Windows 上达成:`alphaclip encode` 一条命令把素材(PNG 序列、带 alpha 的 ProRes MOV、带 VP9 alpha 的 WebM、不透明素材)变成 `clip.webm` + `clip.acmeta.json`,
并在写出之前用 `verify` 检查;生成的包被 M4 的 Unity 测试矩阵原样接受。** `inspect` 打印文件里有什么和核心怎么读它;`verify` 检查时间戳、颜色流和 alpha 流的关键帧是否对齐(R10)、
Cues 是否存在且每个 Cue 指向两路都是关键帧的包(R9)、整片能解码、seek 自洽、alpha 合理,并与 ffmpeg 的独立解码逐字节比对。
编码用的是**外部 ffmpeg**(不链接、不随包发行);颜色默认**预乘**。**没有的:** Pixel 6a 上跑这些命令和用工具生成的片子、Unity 6 / URP / HDRP 上用工具生成的片子、真实素材、1080p 和编码速度、
对 `encode` 新代码的消毒器运行、自带编码器、扩边,详见"没覆盖到的"。

## 证据

| 检查 | 范围 | 结果 |
|---|---|---|
| `core/tests/test_authoring.py`(素材;七种 encode:退出码、文件、sidecar、inspect、verify、**画面对精确真值**;输入和选项;十几种拒绝;手工拼的坏文件;透明处填黑的量化) | Windows,ffmpeg 9.0.2 | **181/181** |
| 同上的负向对照 | | 把 `premultiplied` 写反:边缘均值 19 / 53(要 ≥ 3);矩阵按 BT.709 读:平均误差 1.74(要 ≥ 1.0 且 > 4 倍正确值);坏文件各自触发了对应的 FAIL / warn |
| M4 的 Unity 编辑模式测试矩阵,换成 **`encode` 生成的四个片子**(`--crf 10 --gop 30 --loop`) | 2022.3.62f2,内置管线,Linear 和 Gamma,`-OfflineGuard` | **151/151 × 2**;对真值的合成:全图平均绝对误差 0.12–0.34,软边带符号均值 −0.25 … +0.07(Linear)/ −0.17 … +0.01(Gamma),和 M4 原片一样 |
| 对照:用 M4 原版 `native_straight.webm` 打头 | 同上,Linear | 同样的两项失败(见下) |
| `fuzz_core.py --command authoring`(对 `inspect` 和 `verify --no-ffmpeg` 变异:截断、字节翻转、区域、Cues 等元素的大小字段) | Windows,3 个片子 3 个种子 | **3600 例**:0 崩溃、0 卡死 |
| `test_vs_ffmpeg.py`(核心对 ffmpeg 逐字节) | 原来的 6 个片子 | **281/281**(解析器改了回调,没有退化) |
| `test_shared_lib.py`、`test_meta.py` | Windows | **78/78**、**103/103** |
| `verify` 对全部 10 个已有样本(M0、M1、M4) | Windows | 全部 PASS(含隐藏帧的超帧片、奇数尺寸片、场景切换片;ffmpeg 逐字节一致);没有 sidecar 的 6 个各有一条预期的 warn |
| Android arm64 交叉编译(CLI 和 `.so`) | NDK r30 | 通过;14 个导出、LOAD 段 0x4000;POSIX 的进程代码因此编译过 |

- **坏文件的结果**(手工拼的 320×180、60 帧,颜色每 30 帧一个关键帧):**alpha 每 20 帧一个关键帧时(R10 的真实样本),只有第 0 帧两路都是关键帧**——`verify` 报 `keyframes` warn 和 `cues` FAIL(Cue 指向"颜色流是关键帧而 alpha 流不是"的包);
  核心照样解码正确,seek 退回到第 0 帧,和 ffmpeg 一致。所以 R10 的风险是真实的、`verify` 看得见,而核心在这种文件上不会出错,只是 seek 变慢(这里要从头解码)。
- **Unity 那两项"失败"与 `encode` 无关:** 编辑模式测试对 Pairs 里第一个片子做"BT.709 / 全范围"的参考比较,第一个片子颜色饱和时,错误矩阵解出的值越界,着色器和参考在 Linear 下的夹取对不上(平均差 1.4、最大 28);
  用 M4 的原片打头同样出现。验收时用 `samples/m0/opaque.webm` 打头,151/151。
- 工具生成的片子和 M4 手写脚本生成的大小相近(`native_premul`:39,042 对 40,088 字节),`verify` 全部 PASS,ffmpeg 逐字节一致。

## 数字(CPU 上的量化,不是 GPU)

| 素材 | 存法 | 1:1 软边带符号均值 | 放大 2× |
|---|---|---|---|
| 柔边(M4 的形状) | 直通 / 直通(透明处填黑)/ 预乘 | 0.03 / 0.02 / 0.22 | −0.01 / −0.02 / −0.20 |
| 硬边(1.5 像素的斜坡),颜色延续 | 直通 | 0.05 | 0.00 |
| 硬边,颜色延续或填黑 | 预乘 | 0.99 | +1.54(偏亮) |
| 硬边,透明处填黑 | 直通 | 0.73 | −2.46(暗圈) |

柔边素材怎么存都干净;硬边上直通且颜色延续最准,预乘略偏亮但不变暗,直通且透明处填黑放大后变暗。**默认预乘**(不依赖素材透明处的颜色),`--straight` 在素材的颜色延续时最准。
M4 缺口"直通色、透明处填黑"因此有了(CPU 层面的)数:1:1 没问题,放大才有暗圈,且只在硬边上明显。`verify` 对这种素材给 warn。

## 没覆盖到的(请不要把上面的结论外推到这些)

- **Pixel 6a:** 没有在手机上跑 `inspect` / `verify`(不需要 ffmpeg 的部分可以跑,没做);没有用工具生成的片子重建 APK、装机。包里的 Android `.so` 和 Windows `alphaclip.dll` 仍是 M4 同步的那份,**M5 没有重新同步**。
- **Unity 6、URP、HDRP:** 工具生成的片子只在 2022.3.62f2 的内置管线上跑过(Linear 和 Gamma);播放模式和 UI 路径没有用它们重跑。
- **真实素材、规格:** 所有测试片都是自己合成的 320×180、60 帧;没有 1080p、没有长片;**编码速度、内存没有量**;默认 `--crf 24` 只是占位。输入素材的颜色矩阵只试了 ffmpeg 自己标记的(RGB、ProRes 的默认);
  没标记的 BT.709 YUV 输入会被按 BT.601 读。
- **编码器的行为:** 只试了 ffmpeg 9.0.2(gyan.dev 的 full build,libvpx-vp9)。alt-ref 关掉了,带 alpha 的 alt-ref 没试;两遍编码没做;关键帧对齐靠固定的 GOP(`--gop`),`verify` 会检查,但没有对"场景切换让编码器自己插关键帧"的素材验证
  (固定 `-keyint_min` 之后 libvpx 应该不会,但没有构造这样的素材去试)。
- **POSIX 上没有跑过:** `tools/cli/util.cpp` 里启动子进程的 POSIX 分支(`fork` / `execvp` / 管道)只在 Android arm64 的交叉编译里编译过,没有在任何系统上运行;`encode` / `verify` 的所有测试都是 Windows 上跑的。
- **新代码没有消毒器运行:** Cues 解析和 `analyze` 只做了 3600 例确定性变异(Windows),没有在真机的 ASan + UBSan 构建上跑(要 adb,要你另外同意),没有覆盖率引导的模糊测试。
- **GPU 上的放大:** "放大 2×"的暗圈只在 CPU 上用同样的公式量了,没有在 Unity 里渲染一个放大的四边形去量。
- **没做:** 自带 libvpx 编码器和 libwebm 封装器(需要读 PNG 的新依赖,要下载);扩边(把颜色延伸到透明区域下面);对源素材的 PSNR;`keyed`(M7);音频;
  `encode` 不处理多个视频流、不裁剪或缩放(packed 模式要偶数尺寸,奇数就拒绝)。

## 给后续里程碑的交接

| 事项 | 归属 |
|---|---|
| 用工具生成的片子在 Pixel 6a 上重建 APK 并跑(同时把 M4 的设备验证补上);重新同步包里的二进制 | 补做,要你同意装机 |
| 扩边(颜色延续到透明区域下面),让 `--straight` 在任何素材上都最准 | 看真实素材再定 |
| 自带编码器 / 封装器(去掉对外部 ffmpeg 的依赖) | 看需要 |
| 真实素材上的 `--crf` 默认值、分辨率、帧率 | §13.2 的规格定下来之后 |
| M6 预览页(本地 WebGL,和 Unity 渲染对比):`inspect` 的 `key: value` 输出、sidecar 的 `generator` 可以直接用 | M6 |
| `keyed` 的参数和查找表生成器(TR-5) | M7 |

## 产物

- 代码:`tools/cli/{util,inspect,verify,encode}.*`、`commands.h`、`main.cpp` 的分发;`core/src/analyze.{h,cpp}`;`core/src/demuxer.*`(可选的 Cues 收集);`core/src/meta.*`(`Meta.schema`)。
- 测试:`core/tests/test_authoring.py`(181 项,需要 ffmpeg 和 numpy);`core/tests/fuzz_core.py` 的 `--command authoring`。
- 文档:[m5-notes.md](m5-notes.md)(本阶段的决定、命令参考、量到的数据、踩过的坑)。
- 提交:见 `git log`;都在本地,没有 push。
