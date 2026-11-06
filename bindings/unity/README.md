# AlphaClip 的 Unity 绑定(原型,M4)

("AlphaClip"只是占位名,见设计文档 §13.2。)

标准 VP9/WebM 视频由 C++ 核心(`core/`)在自己的线程里软解,Unity 这一层只做三件事:把字节交给核心、把 Y/U/V 三个平面传成纹理、
用着色器画成 RGB。画面输出在一个普通的 `RenderTexture`(`AlphaClipPlayer.Output`)里,任何材质、相机、UI 都可以当普通贴图用。
M4 起支持**透明**:WebM 原生 alpha(`native_alpha`)和画面旁边并排放 alpha 灰度图的 `packed_right` / `packed_bottom`,由同名的 sidecar
(`clip.acmeta.json`)说明;输出是**预乘 alpha**,要用预乘混合的材质画(见下面"透明"一节)。`keyed`(绿幕抠图)和边缘后处理还没有。
平台:**Unity 编辑器 + Windows x86_64**(M2、M4)和 **Android arm64**(M3:Pixel 6a 上跑过 2022.3.62f2 的开发构建;
Unity 6 上的 Android、AAB / 商店的 16 KB 检查,以及 **M4 在设备上**都还没做,见 `docs/m3-conclusion.md`、`docs/m4-conclusion.md`)。

## 目录

```
package/        UPM 包 com.alphaclip.video:Runtime(P/Invoke、播放组件、着色器)、Editor(插件导入设置)
TestProject/    用来测试和演示的 Unity 工程(2022.3.62f2),通过 file: 依赖引用 package/
scripts/        sync_plugin_win64.sh / sync_plugin_android_arm64.sh(把核心 DLL / .so 拷进包)、run_editmode_tests.ps1(批处理测试)、
                run_android_build.ps1 / run_android_bench.ps1 / compare_picture_samples.py(Android 构建、真机基准、画面对比)
```

## 第一次使用

1. 构建核心(Git Bash,仓库根目录):`sh core/scripts/build_core_win64.sh` → `build/core-win64/bin/alphaclip.dll`
2. **关掉 Unity**(它会锁住已加载的插件),拷进包里:`sh bindings/unity/scripts/sync_plugin_win64.sh`
   (DLL 是构建产物,不提交;`package/Runtime/Plugins/` 在 `.gitignore` 里)
3. 用 **2022.3.62f2** 打开 `bindings/unity/TestProject`。菜单 **AlphaClip > Create Demo Scene** 会把示例片子拷进 `StreamingAssets`
   并生成 `Assets/Scenes/Demo.unity`,打开后点 Play 就能看到视频。

> 不要用 Unity 6 直接打开仓库里的 `TestProject`:它会就地升级工程文件。想验证 6000,先把 `bindings/unity` 复制到别处(见 `docs/m2-conclusion.md`)。

## 在自己的工程里用

`Packages/manifest.json` 里加 `"com.alphaclip.video": "file:<路径>/bindings/unity/package"`,并把 `alphaclip.dll` 放进包的
`Runtime/Plugins/x86_64/`。然后:

```csharp
var player = gameObject.AddComponent<AlphaClipPlayer>();
player.Loop = true;
player.Matrix = ColorMatrix.BT709;            // 取决于素材,见下
player.OpenFile(path);                        // 或 player.Open(byte[]) / player.Open(TextAsset)
player.Play();
renderer.material.mainTexture = player.Output; // 或者给物体加 AlphaClipRendererBinder
```

也可以把组件拖到物体上,填 `Streaming Assets Path`(或 `Clip Asset`),它在 `OnEnable` 时自己打开并播放;`AlphaClipRendererBinder`
把 `Output` 通过 MaterialPropertyBlock 挂到一个 Renderer 上,不改材质资源。想用别的时钟(音频、时间线),自己调
`Present(long clockUs)`,不要开 `Play()`。销毁物体(`OnDestroy`)或调 `Close()` 会停掉解码线程并释放所有原生和 GPU 资源。

## Android(M3)

包里的 `Runtime/Plugins/Android/arm64-v8a/libalphaclip.so` 由 `AlphaClipPluginSettings` 在导入时设成仅 Android、CPU = ARM64(只有 arm64-v8a;和 DLL 一样是构建产物,不提交)。
StreamingAssets 在 APK 里,要用 `UnityWebRequest` 读:组件会自动做到(来源里含 `://` 就走 `OpenUriAsync`),也可以手动调 `OpenStreamingAssetsAsync(path)` / `OpenUriAsync(uri)`
(协程,`IsLoading`、`LoadError` 看状态)。包因此依赖 `com.unity.modules.unitywebrequest`。

```bash
sh core/scripts/build_core_android_arm64.sh        # NDK r30;检查 LOAD 对齐 / 导出符号,生成 build/core-android-arm64/libalphaclip.stripped.so
sh bindings/unity/scripts/sync_plugin_android_arm64.sh
```

```powershell
# 在 build\unity-android(工程副本)里构建开发 APK;-OfflineGuard = 不许联网(Gradle 没有缓存会失败而不是下载)
powershell -File bindings/unity/scripts/run_android_build.ps1 -OfflineGuard
# 真正构建:Gradle 要下载 Android Gradle Plugin 7.4.2 及依赖(约 280 MB,先问用户),放到 E: 上
powershell -File bindings/unity/scripts/run_android_build.ps1 -GradleHome E:\Tools\gradle-home
# 真机基准:会在手机上安装 APK、运行约 1.5 分钟、卸载(先问用户)。结果在 -OutDir 的 results.txt
powershell -File bindings/unity/scripts/run_android_bench.ps1 -Apk build\android\m3test.apk -OutDir build\m3-bench
python -I bindings/unity/scripts/compare_picture_samples.py <Windows 取样> build\m3-bench\results.txt
```

APK 里的 `.so` 必须是 16 KB 对齐的(构建脚本和 `llvm-readelf -lW` 检查);Unity 2022.3 官方只支持它自带的 NDK r23b,
但这里 Unity 只打包我们预先用 NDK r30 构建好的 `.so`,不会用 NDK 编译它。Android 版的 Unity 6 没有验证过。

## 渲染管线

`Output` 是普通的 `RenderTexture`,内置管线和 URP 都能用:

- 内置管线:`Unlit/Texture`、`Standard` 等都用 `_MainTex`。
- URP:`Universal Render Pipeline/Unlit`、`Lit` 用 `_BaseMap`;内置的 `Unlit/Texture` 在 URP 里也能画。
- `AlphaClipRendererBinder` 默认自动找属性(材质有 `_BaseMap` 就用它,否则 `_MainTex`);自定义着色器可以在 `Texture Property` 里写名字。

已在 **2022.3.62f2 + URP 14.0.12** 和 **6000.3.2f1 + URP 17.3.0** 验证(见 `docs/m2-conclusion.md`);HDRP 还没有。
测试工程里 `run_editmode_tests.ps1 -Pipeline URP` 会把工程切到 URP(`-Pipeline Builtin` 切回),`-OfflineGuard` 让编辑器在一个死代理后面启动,
这样 Package Manager 想联网就会失败而不是悄悄下载。URP、Core RP、Shader Graph 是编辑器自带的内置包,不需要下载;
2022.3 上它们依赖的 burst / mathematics / searcher 用的是本机缓存里的版本(测试工程清单里直接指定了版本)。
Unity 6 上 URP 17 依赖的几个包(mathematics、collections、searcher、mono-cecil、test-framework.performance,共约 3.75 MB)本机没有,
而且 Unity 6 的包管理器不复用旧的全局缓存(burst 也要从 `file:` 本地文件夹引用),细节见 `docs/m2-notes.md` 第 9 条;
`bindings/unity/TestProject` 的清单保持 2022.3 的版本,Unity 6 + URP 17 只在工程副本里验证过。

## 透明(M4)

**sidecar:** 和片子同名、扩展名 `.acmeta.json` 的 JSON 文件,说明透明怎么存。`OpenFile`、`Streaming Assets Path`、`OpenUriAsync` 会自动找它(Android 的 APK 里也行);
`Open(byte[])` / `Open(TextAsset)` 用组件的 `Meta Asset`,或者直接把 sidecar 文本作为参数传进去(`OpenFile(path, json)`;传 `""` 表示不用)。
没有 sidecar 时,带 alpha 流的文件按 `native_alpha` 播,其余按不透明。字段表和规则见 `docs/m4-notes.md`,最小的例子:

```json
{ "schema": 1, "alpha_mode": "packed_right", "premultiplied": true, "color_range": "limited", "matrix": "bt601", "content_size": [320, 180] }
```

- `alpha_mode`:`opaque`(忽略文件里的 alpha 流,且不解码它,省约 39% 的解码时间)、`native_alpha`、`packed_right` / `packed_bottom`(画面在左/上,alpha 灰度图在右/下,存成亮度,和颜色用同一个 `color_range`;`content_size` 是画面尺寸,必须是偶数)。
- `premultiplied`:视频里的颜色是否**已经乘过 alpha**。写错会出现暗圈(把预乘的当直通)或亮圈(把直通的当预乘)。
- sidecar 写了 `matrix` / `color_range` 就以它为准(`AlphaClipPlayer.EffectiveMatrix` / `EffectiveFullRange`),没写才用组件上的设置。
- 不合法的 sidecar(JSON 错、字段错、与文件矛盾)抛 `AlphaClipException`(`ErrFormat`),不认识的 schema、`keyed`、非零 `edge` 抛 `ErrUnsupported`,播放器保持关闭。

**画出来:** `Output`(尺寸是画面尺寸,不是编码帧尺寸)是**预乘**的,必须用 `Blend One OneMinusSrcAlpha` 的材质。直通 alpha 的着色器(`Unlit/Transparent`、`UI/Default`、精灵着色器)会再乘一次 alpha,软边出现暗圈。包里有两个现成的:

```csharp
var scene = AlphaClipMaterials.CreateUnlit();   // 网格 / 四边形 / 精灵:着色器 AlphaClip/UnlitPremultiplied(内置管线和 URP)
scene.mainTexture = player.Output;              // 或 AlphaClipRendererBinder
var ui = AlphaClipMaterials.CreateUI();         // UGUI:AlphaClip/UIPremultiplied,Mask / RectMask2D 照常
rawImage.material = ui;
rawImage.texture = player.Output;
```

- Linear 项目:预乘在线性空间里做,在线性帧缓冲里混合(物理上正确);同一个形状无论按直通还是预乘存,出来的结果一致。
- 制作要求(M5 的 `encode` 应当照做):**直通存法的颜色要延伸到透明区域下面**("扩边"),不要在透明处填黑——4:2:0 和滤波会把黑色抹进边缘;或者干脆存预乘的。测试里的两种存法都是这样做的,"直通 + 透明处填黑"没有测过。

## 色彩设置

核心不从码流里读色彩信息(码流里是"未指定"),所以 sidecar 没写时由组件的三个设置决定:

- `Matrix`:`BT601` 或 `BT709`。ffmpeg 从 RGB 源编码、没有显式指定时是 BT.601(仓库里的样本片就是);高清素材通常是 BT.709。
- `Full Range`:一般关(limited 16–235)。
- `Chroma Siting`:`Left`(默认,MPEG-2 / H.26x / VP9 的惯例,色度与偶数列亮度同位)或 `Center`。选错会让色块边缘有半个像素的色边。

项目的色彩空间(Gamma / Linear)不用管:着色器按 `QualitySettings.activeColorSpace` 自动处理,两种下读回的画面都与参考一致。

## 测试

全部在批处理模式下跑,退出码 0 表示通过。先用核心命令行生成参考的原始 YUV:
`build/core-win64/bin/alphaclip.exe decode samples/m0/opaque.webm --out opaque.raw`。

```powershell
# 编辑模式:画面与独立 CPU 参考逐像素比对、seek、色彩参数、奇数分辨率、用相机和 Unlit/Texture 渲染、泄漏
powershell -File bindings/unity/scripts/run_editmode_tests.ps1 -ColorSpace Linear -Pairs "clip.webm|clip.raw;clip2.webm|clip2.raw"
# 播放模式:组件的真实生命周期(OnEnable 自动播放、Update 驱动、循环、Pause/Seek、OnDestroy 释放)
powershell -File bindings/unity/scripts/run_editmode_tests.ps1 -ColorSpace Linear -PlayClip samples\m0\opaque.webm
# 演示场景:重建并播放,检查相机里的画面,-SnapshotDir 会把画面存成 PNG
powershell -File bindings/unity/scripts/run_editmode_tests.ps1 -Demo -SnapshotDir <目录>
# 透明(M4):samples\m4 的四个片子叠在三种背景上,和 RGBA 真值比较,并带负向对照;编辑模式走相机 + 四边形,播放模式走 RawImage(UI)
powershell -File bindings/unity/scripts/run_editmode_tests.ps1 -Pairs "..." -AlphaClips "a.webm;b.webm;..." -Truth samples\m4\truth_rgba_3f.raw
powershell -File bindings/unity/scripts/run_editmode_tests.ps1 -PlayClip samples\m0\opaque.webm -AlphaClips "a.webm;b.webm;..." -Truth samples\m4\truth_rgba_3f.raw
```

脚本会按需切换工程的色彩空间和渲染管线(`-Pipeline Builtin|URP`,各要重启一次编辑器)。`-Unity` / `-Project` 可以指向别的编辑器和工程副本。
核心 DLL 从外面调用的测试(不经过 Unity):`python -I core/tests/test_shared_lib.py ...`。

## 已知限制

- 验证过的只有 Windows 编辑器和 Pixel 6a 上的 Android 开发构建;没有 Windows 播放器构建、macOS、Android 的 release / AAB / 16 KB 页设备(见 `docs/m3-conclusion.md`)。
- 渲染管线:内置管线、2022.3 上的 URP 14.0.12、Unity 6000.3.2f1 上的 URP 17.3.0(Render Graph)验证过。Unity 文档说在 SRP 里 `Graphics.Blit` 应在
  `RenderPipelineManager.endContextRendering` 之类的回调里调用;组件现在是在 `Update()` 里直接调用,在上面这些配置里实测正常
  (编辑模式、播放模式和演示场景),但 HDRP 没有验证,这条仍是已知风险。
- 时钟从 `Play()` / `Seek()` 之后的下一次 `Update` 开始计,但第一张画面要等解码线程出第一帧;这段时间(Windows 纯 C 构建上 140–220 ms)时钟已经在走,
  起始处最多丢掉这么长一段。Pixel 6a 上量到 33–35 ms(含读 APK 和打开),在 640×360 的片子上不需要处理。
- 没有 `keyed`(绿幕抠图)和边缘后处理(M7)、没有音频。没有 UI 绑定组件(`RawImage` 的用法见上面"透明")。
- 从 StreamingAssets 读的文件整个进内存(`Open(byte[])`),大文件不适用;进后台暂停解码没有在设备上验证。
