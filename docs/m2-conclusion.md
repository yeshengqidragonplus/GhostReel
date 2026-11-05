# M2 结论

状态:**草稿,待确认**(2026-11-05)· 对应设计文档 §11 的 M2:Unity 编辑器原型,Windows 编辑器里播放不透明视频,完成标准"画面正确、能循环、不泄漏"。
设计决定和相对设计文档的差异见 [m2-notes.md](m2-notes.md);用法见 [bindings/unity/README.md](../bindings/unity/README.md)。

## 一句话结论

**M2 的完成标准达成,范围是 Unity 编辑器 + Windows。** 核心库现在有共享库(`alphaclip.dll`,1.61 MB,只导出 14 个 `ac_*` 函数,只依赖 KERNEL32 和 UCRT),
Unity 包 `com.alphaclip.video` 通过 P/Invoke 加载它,把解码出的 Y/U/V 传成纹理,用着色器转成 RGB 画进一张 `RenderTexture`。
渲染出的画面与独立的 CPU 参考**最大只差 1 个灰度级**(Gamma 和 Linear 两种工程色彩空间下都是),循环时钟连续,创建销毁不泄漏;
同一套测试在 **2022.3.62f2**(开发基线)和 **6000.3.2f1**(升级兼容验证,在工程副本上)上都通过。
**没有的:** URP/HDRP、Windows 播放器构建、Android、alpha、sidecar、真实素材。

## 证据

| 检查 | 范围 | 结果 |
|---|---|---|
| `core/tests/test_shared_lib.py`(ctypes 从外面调用 DLL:结构体布局、打开/解码/线程化/循环/错误路径、150 次打开关闭) | Windows,4 个片子 | **78/78** |
| 编辑模式测试(画面对 CPU 参考、seek、色彩参数、奇数分辨率、字节与 TextAsset 来源、相机渲染、泄漏) | 2022.3.62f2,Gamma 和 Linear | **62/62 × 2** |
| 同上 | 6000.3.2f1,Gamma 和 Linear | **62/62 × 2** |
| 播放模式测试(真实组件生命周期:`OnEnable` 自动播放、`Update` 驱动、循环、Pause/Seek、150 次 `Destroy`) | 2022.3.62f2 与 6000.3.2f1,Linear(不透明片)/ Gamma(alpha 片) | **17/17**(每种配置) |
| 演示场景(重建、播放、相机里的画面不是纯色、存成 PNG) | 2022.3.62f2 与 6000.3.2f1 | **6/6** |

(以上编辑模式 62 项是最终版测试;2022.3 与 6000 上更早的 56 项版本在代码相同的情况下也全部通过。)

**画面正确性:**
- 渲染出的 `Output` 读回后,与"解码出的 YUV 在 CPU 上按同样采样方式转成 RGB"的独立参考逐通道比较:最大差 **1**,均值 0.003–0.1,
  大于 2 的为 0 个(每个片子约 4100 万个通道值,60 帧)。覆盖不透明、alpha 片(只取彩色)、奇数分辨率 321×181、BT.601 / BT.709、limited / full range、色度左对齐 / 居中。
- 用相机和内置 `Unlit/Texture` 材质把 `Output` 当普通贴图再渲染一遍,结果同样最大差 1。这也走过了 Linear 工程里的 sRGB 读写转换。
- 我自己看了相机截图:方向正立,计时文字可读。
- **和 ffmpeg 自己的转换交叉验证**(同一帧,PSNR):双线性、左对齐色度 **40.5 dB**;居中色度 37.6 dB;错误矩阵(BT.709)25.5 dB。
  未到无穷大是因为 swscale 的定点精度和取整不同;ffmpeg 默认的双三次色度上采样在锐利色块边缘只有 28.7 dB,和朴素双线性本来就不可比。
  这个比较还促成了一个修正:第一版用居中采样,色度横向偏了半个亮度像素,现在默认按视频惯例"左对齐",并保留"居中"选项。
- 色彩空间:着色器按 `QualitySettings.activeColorSpace` 自动处理 Gamma / Linear,两种工程下读回的字节都与参考一致。

**循环、seek、释放:**
- 播放模式里循环跨过循环点:pts 严格递增、没有大于 250 ms 的画面空档(最大 34 ms)。非循环片播到最后一帧并结束。
- seek:每个片子 5 个目标(含片外),显示的是目标时刻那一帧,与参考一致。Pause 时画面不变。
- 不泄漏,判据都用精确的东西:打开/关闭次数相等(编辑模式 400 个周期、播放模式 152 个)、Texture2D / RenderTexture / Material 对象数不增长、
  Unity 分配器和图形驱动内存增长小于 8 MB(实测 0.4–0.7 MB 和 0.0 MB)。核心自己的堆由 `ctypes` 测试在 Unity 之外按周期量:每个打开/关闭周期不到 1 KB。
- 每条泄漏判据都有一个**故意泄漏的对照**会让它失败(40 个不关闭的播放器:对象数、图形驱动内存 +48 MB、Unity 分配器 +14 MB、私有字节 +270 MB 全部报警)。
  一个插曲:第一版用 `Process.PrivateMemorySize64` 量内存,对照暴露出它在 Mono 里根本不动,等于什么也没测,换成了 Win32 `GetProcessMemoryInfo`。
  换了之后进程私有字节在 200 周期窗口里会摆动 ±150 MB 而没有趋势(1600 个周期合计 −111 MB),是显卡驱动的延迟释放,所以它只作粗略上限。

## 数字

- **`Present()` 的 CPU 侧耗时**(取帧、三个平面上传、YUV→RGB 绘制提交):640×360 约 **0.17 ms/帧**,321×181 约 0.1 ms,在 RTX 4060 笔记本显卡上。
  这是桌面显卡,**不代表手机**;它是 NFR-1 里"上传 ms/帧"的一个桌面参照,M3 要在 Pixel 6a 上重测。GPU 侧耗时没有量。
- 核心 DLL 1.61 MB(设计 NFR-3 的目标是核心库 < 2 MB,待实测:Windows x86_64 纯 C 的 libvpx 构建下是这个数)。

## 没覆盖到的(请不要把上面的结论外推到这些)

- **URP / HDRP:** 只验证了内置渲染管线。URP 要装包(需要下载,会先问你)。Unity 文档说在 SRP 里 `Graphics.Blit` 应在 `RenderPipelineManager.endContextRendering`
  之类的回调里调用,目前 `Update()` 里直接调用在 SRP 下**没有验证过,可能要改**。"着色器不依赖管线"只对产出 `Output` 的这一步成立,而且还没在 URP 里实测。
- **播放器构建:** 没有做 Windows 独立播放器构建(IL2CPP/Mono),只在编辑器里。插件设置里勾了 Win64 但没有验证过打包。
- **其他平台:** Android(M3:`libalphaclip.so`、`-Wl,--exclude-libs`、16 KB zip 对齐、StreamingAssets 在 APK 里)、macOS、Linux 编辑器。
- **功能:** alpha(M4)、sidecar 元数据(M4,所以矩阵/范围/色度位置现在是组件设置)、音频、全局实例数上限与降级(FR-10)。
- **素材与规模:** 只有合成片,640×360 和 321×181;没有真实素材,没有 1080p,没有多个播放器同时播放,没有 1 小时浸泡。
- **编辑器场景:** 脚本重新编译(域重载)时如果有打开的播放器会怎样没测;重新构建 DLL 要先关闭 Unity(它锁着已加载的插件)。
- **GPU 侧耗时、内存峰值**没量。

## 给后续里程碑的交接

| 事项 | 归属 |
|---|---|
| Android 共享库 `libalphaclip.so`(隐藏 libvpx/libc++ 符号、静态 libc++、16 KB 对齐)和 Unity 的 Android 插件设置 | M3 |
| Android 上 StreamingAssets 的读取(UnityWebRequest → `Open(byte[])`) | M3 |
| 性能基线(见 M1 结论的方法)加上 Unity 侧的上传和绘制耗时;是否要去掉平面拷贝、是否保留 RenderTexture 这一遍 | M3 |
| Unity 6:Android 模块没装(要下载,会先问你);你说在用 6000,M3 可能应当直接以 6000 为准 | M3 前决定 |
| URP(/HDRP)验证和 `Graphics.Blit` 在 SRP 里的调用位置 | 需要装包时 |
| alpha:`Output` 的 alpha 通道、预乘、混合 | M4 |
| 着色器单一主源 + 生成脚本(现在的 Unity 着色器是第一个变体) | 设计里的后续阶段 |

## 产物

- 核心:`core/CMakeLists.txt`(共享库目标)、`core/src/api.cpp`(ABI 布局的 `static_assert`)、`core/tests/test_shared_lib.py`。
- Unity:`bindings/unity/package/`、`bindings/unity/TestProject/`、`bindings/unity/scripts/`、`bindings/unity/README.md`。
- 样本:演示场景的 `Assets/Scenes/Demo.unity` 与 `Assets/Demo/DemoUnlit.mat`(示例片子由菜单从 `samples/m0` 拷进 `StreamingAssets`,不提交)。
- 提交:见 `git log`;都在本地,没有 push。
