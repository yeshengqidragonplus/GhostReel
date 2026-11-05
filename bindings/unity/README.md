# AlphaClip 的 Unity 绑定(M2 原型)

("AlphaClip"只是占位名,见设计文档 §13.2。)

标准 VP9/WebM 视频由 C++ 核心(`core/`)在自己的线程里软解,Unity 这一层只做三件事:把字节交给核心、把 Y/U/V 三个平面传成纹理、
用着色器画成 RGB。画面输出在一个普通的 `RenderTexture`(`AlphaClipPlayer.Output`)里,任何材质、相机、UI 都可以当普通贴图用。
M2 只做**不透明**视频;alpha 是 M4。目前只有 **Unity 编辑器 + Windows x86_64**;Android 是 M3。

## 目录

```
package/        UPM 包 com.alphaclip.video:Runtime(P/Invoke、播放组件、着色器)、Editor(插件导入设置)
TestProject/    用来测试和演示的 Unity 工程(2022.3.62f2),通过 file: 依赖引用 package/
scripts/        sync_plugin_win64.sh(把核心 DLL 拷进包)、run_editmode_tests.ps1(批处理测试)
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

## 色彩设置(M4 起由 sidecar 提供)

核心不从码流里读色彩信息(码流里是"未指定"),所以现在由组件的三个设置决定:

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
```

脚本会按需切换工程的色彩空间和渲染管线(`-Pipeline Builtin|URP`,各要重启一次编辑器)。`-Unity` / `-Project` 可以指向别的编辑器和工程副本。
核心 DLL 从外面调用的测试(不经过 Unity):`python -I core/tests/test_shared_lib.py ...`。

## 已知限制

- 只有 Windows 编辑器;没有 Windows 播放器构建、Android、macOS(见设计文档里程碑)。
- 渲染管线:内置管线、2022.3 上的 URP 14.0.12、Unity 6000.3.2f1 上的 URP 17.3.0(Render Graph)验证过。Unity 文档说在 SRP 里 `Graphics.Blit` 应在
  `RenderPipelineManager.endContextRendering` 之类的回调里调用;组件现在是在 `Update()` 里直接调用,在上面这些配置里实测正常
  (编辑模式、播放模式和演示场景),但 HDRP 没有验证,这条仍是已知风险。
- 时钟从 `Play()` / `Seek()` 之后的下一次 `Update` 开始计,但第一张画面要等解码线程出第一帧;这段时间(Windows 纯 C 构建上 140–220 ms)时钟已经在走,
  起始处最多丢掉这么长一段。真机上的首帧耗时 M3 再量。
- 没有 alpha(M4)、没有音频、没有 sidecar 元数据(M4)。
- StreamingAssets 路径用的是普通文件路径;Android 上 StreamingAssets 在 APK 里,要先用 UnityWebRequest 读出字节再 `Open(byte[])`(M3)。
