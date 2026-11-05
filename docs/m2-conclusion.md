# M2 结论

状态:**草稿,待确认**(2026-11-05)· 对应设计文档 §11 的 M2:Unity 编辑器原型,Windows 编辑器里播放不透明视频,完成标准"画面正确、能循环、不泄漏"。
设计决定和相对设计文档的差异见 [m2-notes.md](m2-notes.md);用法见 [bindings/unity/README.md](../bindings/unity/README.md)。

## 一句话结论

**M2 的完成标准达成,范围是 Unity 编辑器 + Windows。** 核心库现在有共享库(`alphaclip.dll`,1.61 MB,只导出 14 个 `ac_*` 函数,只依赖 KERNEL32 和 UCRT),
Unity 包 `com.alphaclip.video` 通过 P/Invoke 加载它,把解码出的 Y/U/V 传成纹理,用着色器转成 RGB 画进一张 `RenderTexture`。
渲染出的画面与独立的 CPU 参考**最大只差 1 个灰度级**(Gamma 和 Linear 两种工程色彩空间下都是),循环时钟连续,创建销毁不泄漏;
同一套测试在 **2022.3.62f2**(开发基线)和 **6000.3.2f1**(升级兼容验证,在工程副本上)的内置管线上都通过;
**URP 也补测了:2022.3.62f2 + URP 14.0.12 和 6000.3.2f1 + URP 17.3.0(Render Graph)全部通过**。
Unity 6 + URP 17 需要下载五个小包(共 3.75 MB,你同意之后下的,只放在工程副本里);2022.3 + URP 14 没有下载任何东西。
补测 URP 17 时播放模式测试查出了一个**组件的真实问题**(时钟从 `Play()` 那一刻起算,会把这一帧剩下的卡顿算进去,剪辑开头被快进约 1.3 秒),已修,见"证据"末尾。
**没有的:** HDRP、Windows 播放器构建、Android、alpha、sidecar、真实素材。

## 证据

| 检查 | 范围 | 结果 |
|---|---|---|
| `core/tests/test_shared_lib.py`(ctypes 从外面调用 DLL:结构体布局、打开/解码/线程化/循环/错误路径、150 次打开关闭) | Windows,4 个片子 | **78/78** |
| 编辑模式测试(画面对 CPU 参考、seek、色彩参数、奇数分辨率、字节与 TextAsset 来源、相机渲染、泄漏) | 2022.3.62f2,**内置管线**,Gamma 和 Linear | **64/64 × 2** |
| 同上 | 6000.3.2f1(工程副本),内置管线,Gamma 和 Linear | **64/64 × 2** |
| 同上 | 2022.3.62f2,**URP 14.0.12**,Gamma 和 Linear | **66/66 × 2**(多了用 URP/Unlit 渲染 `Output` 的一项加管线校验) |
| 同上 | 6000.3.2f1(工程副本),**URP 17.3.0**(默认 Render Graph),Gamma 和 Linear | **66/66 × 2** |
| 播放模式测试(真实组件生命周期:`OnEnable` 自动播放、`Update` 驱动、循环、Pause/Seek、150 次 `Destroy`) | 上面四种配置(2022.3 / 6000 × 内置 / URP);各跑 Linear(不透明片)和 Gamma(alpha 片) | **19/19**(八次运行) |
| 演示场景(重建、播放、相机里的画面不是纯色、存成 PNG) | 同上四种配置,Gamma | **7/7**(四次运行) |

上表是**组件修好之后**在最终代码上整体重跑的结果(共 20 次 Unity 运行,全部退出码 0)。
所有 Unity 运行都带 `-OfflineGuard`(编辑器在死代理后面启动),Package Manager 没有、也不可能联网下载;Unity 6 + URP 17 用的五个包是事先下好、核对过 SHA-1、
再用 `file:` 本地引用进来的,burst 1.8.21 是从本机缓存里复制出来的文件夹。
**画面正确性:**
- 渲染出的 `Output` 读回后,与"解码出的 YUV 在 CPU 上按同样采样方式转成 RGB"的独立参考逐通道比较:最大差 **1**,均值 0.003–0.1,
  大于 2 的为 0 个(每个片子约 4100 万个通道值,60 帧)。覆盖不透明、alpha 片(只取彩色)、奇数分辨率 321×181、BT.601 / BT.709、limited / full range、色度左对齐 / 居中。
- 用相机和内置 `Unlit/Texture` 材质把 `Output` 当普通贴图再渲染一遍,结果同样最大差 1。这也走过了 Linear 工程里的 sRGB 读写转换。
- 我自己看了相机截图(内置管线和 URP 各一张):方向正立,计时文字可读,两者看不出差别。
- **和 ffmpeg 自己的转换交叉验证**(同一帧,PSNR):双线性、左对齐色度 **40.5 dB**;居中色度 37.6 dB;错误矩阵(BT.709)25.5 dB。
  未到无穷大是因为 swscale 的定点精度和取整不同;ffmpeg 默认的双三次色度上采样在锐利色块边缘只有 28.7 dB,和朴素双线性本来就不可比。
  这个比较还促成了一个修正:第一版用居中采样,色度横向偏了半个亮度像素,现在默认按视频惯例"左对齐",并保留"居中"选项。
- 色彩空间:着色器按 `QualitySettings.activeColorSpace` 自动处理 Gamma / Linear,两种工程下读回的字节都与参考一致。

**循环、seek、释放:**
- 播放模式里循环跨过循环点:pts 严格递增、没有大于 250 ms 的画面空档(最大 34 ms)。非循环片播到最后一帧并结束。
- seek:每个片子 5 个目标(含片外),显示的是目标时刻那一帧,与参考一致。Pause 时画面不变。
- 不泄漏,判据都用精确的东西:打开/关闭次数相等(编辑模式 400 个周期、播放模式 152 个)、Texture2D / RenderTexture / Material 对象数不增长、
  Unity 分配器和图形驱动内存增长小于 8 MB(实测 0.4–0.7 MB 和 0.0 MB)。核心自己的堆由 `ctypes` 测试在 Unity 之外按周期量:每个打开/关闭周期不到 1 KB。
- **起始与节奏(补测 URP 17 时发现并修掉的问题):** 在 Unity 6 + URP 17 上"画面落后时钟"的检查时好时坏,逐帧打出来发现每次运行第一张画面都落后约 1.3 秒:
  组件的时钟从 `Play()` 被调用的那一刻起算,而那一帧剩下的部分(渲染、URP 17 的首次着色器编译、场景加载)很长,下一帧 `Update` 看到的时钟已经走了 1.3 秒,
  剪辑开头被快进。现在时钟从 `Play()` / `Seek()` 之后的**下一次 `Update`** 开始计,并新增检查"第一张画面落后时钟不超过 500 ms"。
  修后第一张画面落后 31–52 ms(2022.3)和 177–219 ms(6000,这段是解码线程出第一帧加上 Unity 6 批处理模式里时钟比墙钟快 1.6 倍),
  之后落后量的中位数 16 ms、95 分位 31 ms(修前 17 ms 和 63–678 ms,不稳定)。**遗留:** 第一张画面出来之前时钟仍在走,起始处最多丢掉解码线程出第一帧那么长的一段;
  真机上的首帧耗时和是否要"等第一帧再起算"留给 M3。
- 每条泄漏判据都有一个**故意泄漏的对照**会让它失败(40 个不关闭的播放器:对象数、图形驱动内存 +48 MB、Unity 分配器 +14 MB、私有字节 +270 MB 全部报警)。
  一个插曲:第一版用 `Process.PrivateMemorySize64` 量内存,对照暴露出它在 Mono 里根本不动,等于什么也没测,换成了 Win32 `GetProcessMemoryInfo`。
  换了之后进程私有字节在 200 周期窗口里会摆动 ±150 MB 而没有趋势(1600 个周期合计 −111 MB),是显卡驱动的延迟释放,所以它只作粗略上限。

## 数字

- **`Present()` 的 CPU 侧耗时**(取帧、三个平面上传、YUV→RGB 绘制提交):640×360 约 **0.17 ms/帧**,321×181 约 0.1 ms,在 RTX 4060 笔记本显卡上。
  这是桌面显卡,**不代表手机**;它是 NFR-1 里"上传 ms/帧"的一个桌面参照,M3 要在 Pixel 6a 上重测。GPU 侧耗时没有量。
- 核心 DLL 1.61 MB(设计 NFR-3 的目标是核心库 < 2 MB,待实测:Windows x86_64 纯 C 的 libvpx 构建下是这个数)。

## 没覆盖到的(请不要把上面的结论外推到这些)

- **HDRP:** 没有试。URP 14.0.12(2022.3)和 URP 17.3.0(6000.3.2f1)验证过(Gamma 和 Linear,编辑模式、播放模式、演示场景,用 `Universal Render Pipeline/Unlit` 和内置 `Unlit/Texture`
  两种材质渲染 `Output`,与参考最大差 1)。Unity 文档建议 SRP 里 `Graphics.Blit` 在 `RenderPipelineManager.endContextRendering` 之类的回调里调用,而组件是在 `Update()` 里直接调用;
  这在 URP 14 和 URP 17 上实测正常,HDRP 上仍是已知风险。
- **Unity 6 的清单:** `bindings/unity/TestProject` 的清单仍是 2022.3 的版本;Unity 6 + URP 17 只在工程副本里验证,副本的清单里有五个本地 tgz 引用和一个本地 burst 文件夹引用
  (Unity 6 的包管理器不复用旧的全局缓存,见 m2-notes 第 9 条)。以 6000 为准做 M3 时需要决定这些包怎么管理(提交 tgz?私有注册表?还是只在 CI 里下载)。
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
| HDRP;`Graphics.Blit` 在 HDRP 里的调用位置(URP 14 / 17 已验证) | 需要时再做 |
| 起始处时钟:要不要"等第一帧再起算"(需要核心提供第一帧 pts 的查询)和真机上的首帧耗时 | M3 |
| Unity 6 工程里 URP 17 依赖包的管理方式(见"没覆盖到的") | M3 前决定 |
| alpha:`Output` 的 alpha 通道、预乘、混合 | M4 |
| 着色器单一主源 + 生成脚本(现在的 Unity 着色器是第一个变体) | 设计里的后续阶段 |

## 产物

- 核心:`core/CMakeLists.txt`(共享库目标)、`core/src/api.cpp`(ABI 布局的 `static_assert`)、`core/tests/test_shared_lib.py`。
- Unity:`bindings/unity/package/`、`bindings/unity/TestProject/`、`bindings/unity/scripts/`、`bindings/unity/README.md`。
- 样本:演示场景的 `Assets/Scenes/Demo.unity` 与 `Assets/Demo/DemoUnlit.mat`(示例片子由菜单从 `samples/m0` 拷进 `StreamingAssets`,不提交)。
- 提交:见 `git log`;都在本地,没有 push。
