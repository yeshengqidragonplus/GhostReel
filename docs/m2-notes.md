# M2 笔记

M2 = Unity 编辑器原型:Windows 编辑器里播放不透明视频,完成标准"画面正确、能循环、不泄漏"(设计文档 §11)。
结论见 [m2-conclusion.md](m2-conclusion.md)(草稿)。本文记录 M2 里做的设计决定,以及它们相对设计文档 v0.2 的差异。

## 代码布局

```
core/CMakeLists.txt, core/src/api.cpp   新增共享库目标 alphaclip_shared(alphaclip.dll,只导出 14 个 ac_* 函数)和 ABI 布局的 static_assert
core/tests/test_shared_lib.py           从外面(ctypes)调用 DLL 的测试,78 项
bindings/unity/package/                 UPM 包 com.alphaclip.video
  Runtime/Native/AcNative.cs            P/Invoke 声明和带检查的包装;结构体布局与 C++ 的 static_assert 对应,运行时再校验一次
  Runtime/AlphaClipPlayer.cs            播放组件
  Runtime/AlphaClipRendererBinder.cs    把 Output 挂到 Renderer 上
  Runtime/Resources/AlphaClip/YuvToRgb.shader
  Editor/AlphaClipPluginSettings.cs     导入 alphaclip.dll 时设成 Editor + Win64 插件
bindings/unity/TestProject/             测试和演示工程(Assets/Editor 的批处理测试、Assets/Scripts 的播放模式测试驱动、演示场景、
                                        Assets/Demo/UrpPipeline.asset 和 UrpRenderer.asset:切换到 URP 时用的管线资源;工程默认是内置管线 + Gamma)
bindings/unity/scripts/                 sync_plugin_win64.sh、run_editmode_tests.ps1
```

## 决定与差异(需要你知道)

1. **Unity 版本:** 开发基线 2022.3.62f2(满足 Unity 官方说的 16 KB 页版本门槛 2022.3.56f1);用户实际在用 Unity 6000,所以同时在
   本机的 6000.3.2f1 上验证升级兼容。验证用的是 `bindings/unity` 的**副本**:直接用 6000 打开仓库里的工程会就地升级工程文件。
2. **画面输出是 RenderTexture,不是"每个渲染器自己画三个平面"。** 组件把 Y/U/V 传成 R8 纹理,用一个着色器 `Graphics.Blit` 到一张
   RGBA 的 `RenderTexture`(`Output`),任何材质、相机、UI 把它当普通贴图。好处:着色器不依赖具体渲染管线和各种 UI 着色器;
   以后 alpha(预乘)可以直接写进 Output 的 alpha 通道。代价:多一张 w×h×4 字节的贴图(640×360 是 0.9 MB,1080p 是 8 MB)和每个新帧一次全屏绘制。
   设计文档 §4/§9 写的是"用着色器画出来",这里偏离了;若 M3 的基线显示这一遍绘制或这张贴图成问题,再改。
3. **着色器是手写的 Unity 版本。** 设计要求"一份 GLSL ES 3.0 主源 + 生成脚本,各引擎版本由它生成"(§8.3);生成脚本还没有,M2 的 Unity 着色器就是将来要被生成的第一个变体,
   文件头里记着这点。
4. **纹理方向和色彩:**
   - 平面按"第一行在上"上传,而 Unity 纹理的第一行是下边,所以着色器把 V 翻转(用相机渲染并与参考比对验证过方向)。
   - Y/U/V 是 `R8`、线性(非 sRGB)、不带 mipmap;Y 点采样,U/V 双线性。
   - **色度位置:** 默认"左对齐"(MPEG-2 / H.26x / VP9 的惯例,色度横向与偶数列亮度同位),在着色器里把色度查找右移 0.25 个色度像素;
     也可选"居中"(朴素双线性假设的位置)。这是因为第一版用居中采样时,与 ffmpeg(左对齐)比较发现色度偏了半个亮度像素。
   - **矩阵/范围:** BT.601 / BT.709、limited / full 现在是组件上的设置;码流里的色彩信息是"未指定"(M0 已确认),M4 起由 sidecar 提供。
   - **Gamma / Linear 色彩空间:** 着色器有一个 `_LinearOutput` 参数,由 C# 按 `QualitySettings.activeColorSpace` 设置;Linear 下着色器输出线性值,
     由硬件在写 sRGB 渲染目标时编码(`GL.sRGBWrite` 默认开启,文档也建议不要去切换它,而是把渲染目标建成正确的 sRGB/线性类型)。
     sRGB→线性的转换自己写(Unity 文档没有确认 `UNITY_COLORSPACE_GAMMA` 这个宏,所以不依赖它)。
5. **原生插件:** `alphaclip.dll` 由 llvm-mingw 构建,静态 libc++/libgcc/winpthreads,只依赖 KERNEL32 和 UCRT 的 api-ms-win-crt-*,
   只导出 14 个 `ac_*`。**不提交**(构建产物),由 `sync_plugin_win64.sh` 拷进包;所以不提交它的 `.meta`,导入设置(仅 Editor + Win64)由
   `AlphaClipPluginSettings` 在导入时用代码设置。结构体(`ac_info` 48、`ac_frame` 72、`ac_stats` 40 字节)的布局用 C++ `static_assert` 锁死,
   C# 侧运行时再校验。
6. **打开方式:** `OpenFile(path)`(编辑器和独立播放器)、`Open(byte[])`(拷进原生内存,核心不拷贝所以组件持有它直到 `ac_close` 之后才释放)、
   `Open(TextAsset)`(`.bytes` 资源,资源包里的原始数据就是这么走的)。Android 的 StreamingAssets 在 APK 里,要用 UnityWebRequest 读字节再 `Open(byte[])`——留给 M3。
7. **生命周期:** `OnDestroy` → `Close()`:先 `ac_close`(会 join 解码线程),再释放字节拷贝,再销毁贴图、材质、渲染纹理。`OnDisable` 不做事:Update 不再被调用,
   核心的队列满了解码线程就会停下,CPU 占用接近零,所以没有单独做"后台暂停"(设计 §9.4 里要求的"应用进后台时暂停解码"留给真机阶段确认)。
8. **时钟:** `Time.unscaledTimeAsDouble`。批处理模式的播放模式里,Unity 自己的两种时间不一致(`unscaledTimeAsDouble` 比 `realtimeSinceStartup` 快约 30%;Unity 6 上量到 1.6 倍,帧率每秒几千帧时更多),
   所以播放模式测试按引擎时钟验证节奏,不按墙上时钟。时钟从 `Play()`/`Seek()` 之后的**下一次 `Update`** 开始计,见第 9 条末尾。
9. **URP(补测):**
   - **包:** URP、Core RP、Shader Graph、universal-config 都是编辑器自带的内置包(`BuiltInPackages`),不需要下载。公开注册表里 URP 的版本列表只到 10.10.1
     (14.x / 17.x 只随编辑器提供),所以不能靠查注册表找它们。2022.3 上它们依赖的 burst / mathematics / searcher,本机缓存里有更高的版本
     (1.8.21 / 1.2.6 / 4.9.2),测试工程的清单里直接指定这几个版本;用"死代理"保险(`-OfflineGuard`)启动编辑器确认了没有联网。
   - **6000.3.2f1 + URP 17.3.0 要下载**(用户同意后下载了,只下这五个)mathematics 1.3.2、collections 2.4.3、searcher 4.9.3、nuget.mono-cecil 1.11.4、
     test-framework.performance 3.0.3,共 3.75 MB(来源 `download.packages.unity.com`,逐个核对了注册表给的大小和 SHA-1)。它们只放在临时目录和 Unity 6 的工程副本里
     (清单里用 `file:` 指向本地 tgz),不进全局缓存也不进仓库。
   - **Unity 6 的包管理器不复用旧的"已解压全局缓存":** 同样的 burst 1.8.21 在 2022.3 上直接用缓存,在 Unity 6 上却还是尝试联网下载(被 `-OfflineGuard` 拦下)。
     解决办法是把缓存里的 `com.unity.burst-1.8.21` 文件夹复制出来,清单里写 `file:` 指向这个文件夹(1.2 GB 的本地拷贝,没有下载;不能指向 tgz,因为缓存里只有解压后的目录)。除 burst 之外 URP 17 依赖链上缺的包就是上面那五个。
   - **清单必须不带 BOM:** Windows PowerShell 5.1 的 `Set-Content -Encoding UTF8` 会写 BOM,Unity 的清单 JSON 解析器拒收;用 Python 写。
   - **`Graphics.Blit` 在 SRP 里的调用位置:** Unity 文档建议在 `RenderPipelineManager.endContextRendering` 之类的回调里调用。实测在 URP 14.0.12 上,
     组件在 `Update()` 里直接调用(编辑模式测试里在渲染循环之外调用、播放模式里由 `Update` 调用)工作正常,画面与内置管线逐值一致;所以没有改成回调。
     Unity 6000.3.2f1 + URP 17.3.0(默认 Render Graph)上同样的测试也全部通过,画面与参考最大差 1,所以仍然没有改成回调。HDRP 没有验证,仍是已知风险。
   - **材质属性:** URP 的着色器用 `_BaseMap`,内置用 `_MainTex`;`AlphaClipRendererBinder` 默认自动选(材质有 `_BaseMap` 就用它)。内置的 `Unlit/Texture` 在 URP 里也能画(没有 LightMode 的 pass 会被 URP 当 SRPDefaultUnlit 渲染)。
   - **测试:** 运行脚本多了 `-Pipeline Builtin|URP`(按需切换工程并重启一次编辑器)和 `-OfflineGuard`;测试会校验当前管线;相机渲染 `Output` 的测试在 URP 下用两种材质(`Universal Render Pipeline/Unlit` 和内置 `Unlit/Texture`)。
   - **测试里踩的两个坑:** 播放模式的"节奏"检查原先比较总耗时,结果切换色彩空间后的第一次启动因为着色器编译卡了 1.2 秒而失败一次,同样配置重跑两次都通过;
     改成逐帧看"画面落后时钟多少"的中位数和 95 分位(17 ms / 32 ms),不受一次性卡顿影响。另外,运行脚本里局部变量 `$method` 覆盖了 `-Method` 参数(PowerShell 变量不区分大小写)。
   - **Unity 6 + URP 17 上这条检查又失败了,这次查出来的是组件的真实问题,不是测试的抖动:** 每次运行第一张画面都落后时钟约 1.3 秒(统计量是"落后多少",
     按帧数取 95 分位,批处理模式里每秒两千多帧,启动时追赶的那 80 帧正好压在 95 分位的边缘,所以时好时坏)。原因:`Play()` 在某一帧中间被调用,时钟从调用那一刻开始计,
     而这一帧剩下的部分(渲染、URP 17 的首次着色器编译、场景加载)可能很长,下一帧 `Update` 看到时钟已经走了 1.3 秒,于是剪辑开头被快进。
     修法:`Play()` 和 `Seek()` 只记下"待锚定",时钟从**下一次 `Update`** 开始计(`m_anchorPending`);顺手让 `Play()` 在已经播放时直接返回(原先重复调用会把时钟倒回上一次的基准)。
     新增一项检查:第一张画面落后时钟不超过 500 ms(修之前每次 1.3 秒,修之后 140–220 ms,这是解码线程出第一帧的耗时,Windows 纯 C 构建上的数,真机另量)。
     修后落后量的中位数 16 ms、95 分位 31 ms(之前 17 ms / 63–678 ms)。
     **遗留:** 第一张画面前时钟仍在走(要等解码线程出第一帧),所以起始处最多会丢掉约这个耗时那么长的一段;要做到"等第一帧再开始计时"需要核心提供"第一帧 pts"的查询,M3 量过真机上的首帧耗时再决定。
10. **验证方法的几条教训**(也写进了 CLAUDE.md):
   - 编辑模式不发 `OnDestroy`,所以编辑模式测试显式 `Close()`,`OnDestroy` 路径由播放模式测试覆盖。
   - `Process.PrivateMemorySize64` 在 Mono 里不动,用 Win32 `GetProcessMemoryInfo`;进程私有字节会受显卡驱动的延迟释放影响,单个 200 周期窗口里能摆动 ±150 MB 而没有趋势,
     所以泄漏判据用精确的计数器(打开/关闭次数、Texture/RT/Material 对象数、Unity 分配器和图形驱动内存),进程私有字节只作粗略上限并取两个窗口里小的那个;
     核心自己的堆由 `ctypes` 测试在 Unity 之外按周期检查(每周期不到 1 KB)。每条判据都有一个故意泄漏的对照来证明它会失败。
   - `WaitForEndOfFrame` 在批处理模式里不会恢复。
   - Windows PowerShell 5.1 在参数默认值里拿不到 `$PSScriptRoot`。
