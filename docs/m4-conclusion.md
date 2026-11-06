# M4 结论

状态:**定稿**(2026-11-06,用户确认)· 对应设计文档 §11 的 M4:透明(基础),`packed_right` / `packed_bottom` + `native_alpha`,预乘输出,完成标准"在 UI 和场景里叠加无黑边"。
设计决定、sidecar 字段表和规则见 [m4-notes.md](m4-notes.md);用法见 [bindings/unity/README.md](../bindings/unity/README.md)。

## 一句话结论

**M4 的完成标准在 Windows 上达成:Unity 2022.3.62f2 的编辑器,内置管线和 URP,Gamma 和 Linear 两种色彩空间,场景(相机 + 四边形)和 UI(RawImage)两条路径。**
核心现在会读 sidecar(`clip.acmeta.json`,schema 1),支持 `opaque`、`packed_right`、`packed_bottom`、`native_alpha` 四种模式;着色器输出预乘 RGBA,
把同一个已知形状的四种存法(原生 alpha / packed 右 / packed 下,直通和预乘两种颜色)叠在白、灰、青三种背景上,与精确的 RGBA 真值合成比较:
**半透明边缘的带符号平均误差在 ±0.15 个灰度级以内,没有暗圈**(Gamma 下 ±0.06);三个负向对照(把 `premultiplied` 标志写反、用直通 alpha 的着色器、用 Unity 默认 UI 材质)
分别显出 −19 到 +21(Gamma)和 −6 到 +14(Linear)的暗圈或亮圈,所以这个判据确实看得见黑边。
**没有的:** Pixel 6a 上的 M4(包里的 Android `.so` 还是 M3 的)、Unity 6、HDRP、`keyed`、边缘后处理、"直通色但透明处填黑"的素材、真实素材,详见"没覆盖到的"。

## 证据

| 检查 | 范围 | 结果 |
|---|---|---|
| `core/tests/test_meta.py`(ctypes 调用 DLL:auto 模式、每个字段、alpha 流的处理、packed 布局和它的拒绝、六十多种非法/不支持的 sidecar 及其状态码、sidecar 先于文件检查、变异) | Windows | **103/103**;外加 **10 万例**和 **100 万例**变异(种子 7、11):无崩溃,每个结果都是有文档的状态码,合法的(2253、22565 例)`ac_info` 都自洽 |
| 同一套测试,把两个样本片对调(负向对照) | Windows | 大量 FAIL(测试能失败) |
| `core/tests/test_vs_ffmpeg.py`(对 ffmpeg 逐字节) | 原来的 6 个片子 | **281/281**(没有退化) |
| 同上 | 新的 4 个 M4 片子(自动模式 = 普通文件) | **187/187** |
| `core/tests/test_shared_lib.py`(`ac_info` 现为 80 字节) | Windows,4 个片子 | **78/78** |
| Android arm64 交叉编译 | NDK r30 | 通过、无警告;14 个导出、LOAD 段 0x4000、只依赖 libm / libdl / libc;strip 后 711,984 字节(M3 是 680,520,多的是 JSON 和 sidecar 代码) |
| 编辑模式测试(画面对 CPU 参考、seek、色彩参数、奇数尺寸、相机渲染、泄漏;**M4:** 4 个新片的画面对参考、对真值的合成、对照、sidecar 的错误路径) | 2022.3.62f2,**内置管线**,Gamma 和 Linear | **178/178 × 2** |
| 同上 | 2022.3.62f2,**URP 14.0.12**,Gamma 和 Linear | **180/180 × 2**(多了用 URP/Unlit 渲染的两项) |
| 播放模式测试(组件生命周期;**M4:** sidecar 经 UnityWebRequest、UGUI RawImage + `AlphaClip/UIPremultiplied` 对真值、默认 UI 材质对照、RectMask2D) | 上面四种配置 | **90/90 × 4** |

最后一轮是在最终代码上整体重跑的结果(播放模式 4 个配置、Linear 的 2 个配置的编辑模式;Gamma 的 2 个编辑模式配置在把"对照下限"改成随色彩空间走之后补跑)。所有 Unity 运行都带 `-OfflineGuard`,没有下载任何东西。

**画面正确性(着色器对独立 CPU 参考):** 解码出的 YUV(A) 在 CPU 上按同样的采样方式转成预乘 RGBA,和 `Output` 读回逐通道比较,所有模式(原生 alpha、packed 右、packed 下;直通、预乘;
Gamma、Linear)**最大只差 1 个灰度级**,包括 alpha 通道;原来的片子(含 321×181 奇数尺寸、BT.709、全范围、色度居中)同样最大差 1。
参考实现把色度查找限制在画面自己的色度样本内,与着色器一致;packed 画面的右边缘因此不会读到灰色 alpha 图的色度。

**合成对真值(判据本身):** 同一个形状存成四种方式,叠在三种背景上,与解析合成 `truth.rgb × a + 背景 × (1 − a)`(Linear 项目在线性光下)比较。

| | Gamma | Linear |
|---|---|---|
| 正确管线:全图平均绝对误差 | 0.13 – 0.28 | 0.12 – 0.31 |
| 正确管线:软边缘带符号平均误差 | −0.03 … +0.06 | +0.04 … +0.15 |
| 正确管线:单点最大误差 | 12 | 38(原因没查,大概是 sRGB 曲线在近黑处很陡;所以不用最大值判) |
| 对照:直通 alpha 的着色器 / 默认 UI 材质 | −19.1 … −19.2 | −6.0 |
| 对照:`premultiplied` 写反(直通片按预乘 / 预乘片按直通) | +21.2 / −19.2 | +13.8 / −9.7 |
| 限值(平均绝对误差 / 边缘均值 / 对照至少) | 1.0 / ±1.0 / 10 | 1.0 / ±1.0 / 3 |

内置管线和 URP 的数字几乎一样。Linear 的对照幅度小,是因为误差在线性光里产生、再 sRGB 编码成字节,同样的"颜色多乘一次 alpha"在字节上移动得少;限值因此随色彩空间走。
UI 路径(RawImage + 预乘 UI 着色器)与场景路径的数字相同;`RectMask2D` 压住左半边时,右半边与纯背景逐像素一致(0 个像素不同),左半边仍等于真值(平均误差 0.03)。

## 数字

- **`opaque` 的 sidecar 让核心不解码文件里的 alpha 流**(Windows 纯 C、640×360、60 帧,解码线程耗时 104.3–104.9 ms → 62.6–63.9 ms,省约 39%;测量时机器上还有 Unity 测试在跑,仅作参考)。
- **`Present()` 的 CPU 侧耗时**(桌面 RTX 4060 笔记本;不代表手机):同一个 320×180 的画面,原生 alpha 约 0.09–0.10 ms/帧,packed 约 0.16–0.18 ms/帧(packed 的编码帧是画面的两倍大,要上传两倍的像素);640×360 的不透明片约 0.18–0.20 ms。单次测量、有抖动,只作参考。
- 核心 DLL 现在 1,733,632 字节(M2 时是 1.61 MB,多出来的是 JSON 和 sidecar 代码),仍然只导出 14 个 `ac_*`。

## 没覆盖到的(请不要把上面的结论外推到这些)

- **Pixel 6a:** M4 在设备上没有跑。包里的 `Plugins/Android/arm64-v8a/libalphaclip.so` 已经换成 M4 的构建(711,984 字节,已同步),但没有用它重建过 APK,更没有装到手机上
  (M3 的旧 `.so` 与新的 80 字节 `ac_info` 不兼容)。重建 APK 只需要主机(Gradle 缓存已在 `E:\Tools\gradle-home`,可离线构建),装到手机上要你另外同意。alpha 在手机上的耗时(A 平面的上传、packed 的双倍像素)没有量。
- **Unity 6:** M2 时的工程副本是一次性的,已经不在了;重建要再下载五个 URP 17 的依赖包(共 3.75 MB,要先问你)。M4 的着色器和播放器在 6000 上没验证过。
- **HDRP**、`keyed`(M7)、边缘后处理(M7)。
- **直通色、透明处填黑的素材**(很多工具导出的 RGBA 就是这样):4:2:0 和滤波会把黑色抹进边缘。测试里的两种直通/预乘存法都没有这个问题(直通片的颜色延伸到透明区域下面),所以"直通 + 透明处填黑"会不会出暗圈、有多重,**没有量过**;
  `README` 和 `m4-notes` 里写了制作要求,M5 的 `encode` 要照做,并且最好用一个对照片量出来。
- **别的编码器的 alpha 写法**、真实素材(头发、粒子)、1080p、多个播放器同时播放、长时间浸泡。
- **UI 绑定组件**没有做(`RawImage` 的用法写在 README 里);`loop_hint` 只是读出来,组件不据此循环。
- 数字只在这一台机器上(RTX 4060 笔记本、Windows)量过,不同显卡的色度滤波可能差一两个灰度级。

## 给后续的交接

| 事项 | 建议归属 |
|---|---|
| Pixel 6a 上的 M4:基准程序加上 alpha 片(含 A 平面上传、packed 的双倍像素)、重建 APK、装到手机上跑、画面对 Windows 的取样(含 alpha) | 你同意设备操作后做(M4 的剩余项;`.so` 已同步) |
| Unity 6 上的回归(重建工程副本,下载五个小包) | 你决定;可和 Android 的 Unity 6 问题一起处理 |
| 制作工具 `encode` 的要求:直通色要向透明区域延伸(扩边)或存预乘;packed 的 alpha 写成亮度、内容尺寸取偶数、色度写 128;`verify` 检查 sidecar 与文件一致、彩色与 alpha 两路关键帧对齐;用"直通 + 透明处填黑"的对照片量出暗圈有多重 | M5 |
| 哪种透明模式对手机最划算(原生 alpha 多一路解码;packed 解码和上传像素翻倍):用设备数字填设计 §5 的比较 | M4 剩余项 / M5 之后 |
| `keyed` 模式、边缘后处理(sidecar 里 `edge` 非零现在返回 `AC_ERR_UNSUPPORTED`) | M7 |
| HDRP;UI 绑定组件(如果需要) | 需要时再做 |
| 着色器单一主源 + 生成脚本(现在有三个手写的 Unity 着色器) | 设计里的后续阶段(Godot / Cocos 开始前) |

## 产物

- 核心:`core/src/{json,meta}.{h,cpp}`、`core/include/alphaclip.h`(`ac_info` 80 字节、枚举)、`core/src/{api,clip}.cpp`、`tools/cli/main.cpp`(`--meta`)、`core/tests/test_meta.py`、`core/tests/test_shared_lib.py`。
- 样本:`samples/m4/`(`gen_clips.ps1`、4 个片子、4 个 sidecar、`truth_rgba_3f.raw`)。
- Unity:`bindings/unity/package/Runtime/`(`AlphaClipPlayer`、`AcNative`、`AlphaClipMaterials`、`Resources/AlphaClip/{YuvToRgb,Unlit,UI}.shader`)、
  `bindings/unity/TestProject/Assets/{Scripts/AlphaClipTruth.cs,Scripts/PlayModeTestDriver.cs,Editor/AlphaClipEditModeTests.cs,Editor/AlphaClipEditModeAlphaTests.cs}`、`bindings/unity/scripts/run_editmode_tests.ps1`。
- 提交:见 `git log`;都在本地,没有 push。
- 剩余项(Pixel 6a 上的 M4、Unity 6)你选择按现状定稿,没有先补做;它们记在"给后续的交接"里,不阻塞 M5,也不要当作已经做过。

> **M5 之后的处理结果(2026-11-06 加注,上面的正文是 M4 定稿时的原样):** "直通色、透明处填黑"的素材 M5 在 CPU 上量过了(见 [m5-notes.md](m5-notes.md)):柔边素材怎么存都干净;1.5 像素的硬边上,放大 2× 后直通且透明处填黑的软边均值 −2.5 个灰度级(暗圈),
> 预乘 +1.5,直通且颜色延续的 0.0;GPU 上的放大没量。Pixel 6a 上的 M4 和 Unity 6 仍然没有做(M5 也没有补)。M5 的 `encode` 生成的四个片子过了本阶段的 Unity 矩阵(2022.3.62f2、内置管线、Gamma 和 Linear,151/151 × 2)。
