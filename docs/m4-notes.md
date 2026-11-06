# M4 笔记

M4 = 透明(基础):`packed_right` / `packed_bottom` + `native_alpha`,预乘输出;完成标准"在 UI 和场景里叠加无黑边"(设计文档 §11)。
结论见 [m4-conclusion.md](m4-conclusion.md)(定稿)。本文记录 M4 里做的设计决定、相对设计文档 v0.2 的差异和方法。
改核心的元数据代码(`core/src/{json,meta}.cpp`)或 `bindings/unity/` 的着色器和播放器之前先读它;更早的决定见 [m1-notes.md](m1-notes.md)、[m2-notes.md](m2-notes.md)、[m3-notes.md](m3-notes.md)。

## 代码布局(M4 新增和改动)

```
core/src/json.{h,cpp}                      严格的 JSON 读取器(RFC 8259),自己写,没有依赖
core/src/meta.{h,cpp}                      sidecar(schema 1)的解析、校验、布局推导
core/include/alphaclip.h, core/src/api.cpp ac_info 扩到 80 字节(static_assert 锁死);ac_open_* 接受 meta_json;ac_version 0.1.0-m4
core/src/clip.{h,cpp}                      Clip::Open(..., ignore_alpha):opaque 不解码 alpha 流
tools/cli/main.cpp                         info / decode / play 都加了 --meta <文件>;info 多打印 alpha 模式、内容尺寸等
core/tests/test_meta.py                    sidecar 的 ctypes 测试(103 项,含 3000 例变异)
core/tests/test_shared_lib.py              ac_info 布局改为 80 字节
samples/m4/                                gen_clips.ps1、4 个片子、4 个 sidecar、3 帧 RGBA 真值
bindings/unity/package/Runtime/            AcNative(AcInfo 80 字节、meta 参数)、AlphaClipPlayer(sidecar 来源、alpha 平面、内容尺寸)、
                                           AlphaClipMaterials、Resources/AlphaClip/{YuvToRgb,Unlit,UI}.shader
bindings/unity/TestProject/Assets/         Scripts/AlphaClipTruth.cs(真值和判据,两套测试共用)、Editor/AlphaClipEditModeAlphaTests.cs、
                                           Scripts/PlayModeTestDriver.cs(UI 路径)、Editor/AlphaClipEditModeTests.cs(参考实现支持 alpha)
bindings/unity/scripts/run_editmode_tests.ps1   新增 -AlphaClips 和 -Truth
```

## sidecar(`clip.acmeta.json`,schema 1)

| 字段 | 必填 | 取值 | 说明 |
|---|---|---|---|
| `schema` | 是 | 整数 `1` | 其他整数 → `AC_ERR_UNSUPPORTED`;不是整数(`"1"`、`1.0`、`1e0`、布尔)→ `AC_ERR_FORMAT` |
| `alpha_mode` | 是 | `opaque` `packed_right` `packed_bottom` `native_alpha` `keyed` | `keyed` → `AC_ERR_UNSUPPORTED`(M7);未知值、大小写不符 → `AC_ERR_FORMAT` |
| `premultiplied` | 否,默认 `false` | 布尔 | **视频里的颜色是否已经乘过 alpha**(设计文档草案里写的是"输出是否预乘",含糊,这里定为这个意思) |
| `color_range` | 否,默认"未指定" | `limited` `full` | 未指定 = 绑定自己的设置;packed 的 alpha 图也按这个 range 解码 |
| `matrix` | 否,默认"未指定" | `bt601` `bt709` | 同上 |
| `content_size` | 否 | `[宽, 高]`,两个 1..8192 的整数 | 画面尺寸;`opaque`/`native_alpha` 下必须等于视频尺寸;packed 下可省略(由视频尺寸减半推出) |
| `loop_hint` | 否,默认 `false` | 布尔 | 只是提示,组件不据此循环 |
| `edge` | 否 | `{shrink, feather, denoise}`,数字 ≥ 0 | 全 0 才接受;**非零 → `AC_ERR_UNSUPPORTED`**(边缘后处理要到 M7,宁可拒绝也不悄悄忽略) |
| `keyed` | 否 | 对象 | 只有 `alpha_mode` 是 `keyed` 时才有意义(M7);其他模式下忽略 |
| 其他键 | — | 任意 | 忽略(向前兼容);同一对象里**重复的键**拒绝 |

- **没有 sidecar**(传 `NULL` 或 `""`):"auto"——文件带 alpha 流就按 `native_alpha`,否则 `opaque`;颜色范围和矩阵"未指定"。这保持了 M1 到 M3 的行为(那时没有 sidecar)。设计文档说 sidecar"必需",这里放宽为可选,因为工具(M5)总会写它,而核心的测试和老素材不需要。
- **一致性检查**(打开文件时,sidecar 与文件对不上 → `AC_ERR_FORMAT`):`native_alpha` 但文件没有 alpha 流;`packed_*` 但文件同时带 alpha 流(自相矛盾);`opaque` 带了不等于视频尺寸的 `content_size`;packed 布局放不下(`packed_right` 要宽 ≥ 2×内容宽、高 ≥ 内容高;`packed_bottom` 要宽 ≥ 内容宽、高 ≥ 2×内容高)。
- **`opaque` + 文件里有 alpha 流:合法**,alpha 流被忽略且**不解码**(Windows 纯 C 构建、640×360、60 帧:解码线程 104.3–104.9 ms → 62.6–63.9 ms,省约 39%;测量时 Unity 测试也在跑,只作参考)。
- sidecar 在读文件**之前**检查(`ac_open_file` 不会为一个坏 sidecar 去读一个大文件)。大小上限 64 KiB(超出 → `AC_ERR_OOM`),嵌套深度 ≤ 8,字符串 ≤ 4096 字节,数组和对象 ≤ 64 项。UTF-8 严格校验(过长编码、代理对、U+10FFFF 以上都拒绝);开头的 UTF-8 BOM 接受(Windows 工具会写);`\u0000` 拒绝。
- 数字不用 `strtod`(跟随区域设置),自己解析。

## packed 布局

- `packed_right`:画面在左,alpha 图在右;`packed_bottom`:画面在上,alpha 图在下。画面左上角在编码帧的 (0,0),alpha 图的左上角是 `ac_info.alpha_x/alpha_y`(核心算好,绑定不必各自重复这条规则):`packed_right` 为 (内容宽, 0),`packed_bottom` 为 (0, 内容高)。视频比"画面 + alpha 图"大时,多出来的部分忽略。
- **内容宽高必须是偶数**(且 ≥ 2):4:2:0 的色度样本这样就不会跨在画面和 alpha 图的边界上。
- **alpha 图存成亮度**:alpha 0 → Y 16,alpha 255 → Y 235(limited),或 0..255(full),**和颜色用同一个 `color_range`**;它的色度被忽略(制作工具应写 128)。这是"一张灰度图变成视频"最自然的结果。`native_alpha` 的 alpha 流则固定是全范围 0..255(M0 已确认)。
- **核心不拷贝像素:** `ac_frame` 还是整个编码帧(`width`/`height` = 编码尺寸),裁剪是着色器的事(`ac_info` 给内容尺寸和 alpha 图位置)。这样保持"字节进、YUV(A) 帧出",也没有多一次内存拷贝。
- **色度夹紧:** 着色器把色度查找限制在画面自己的色度样本内。没有它,`packed_right` 画面最右一列的双线性滤波会读到右边灰色 alpha 图的色度(128),在颜色边缘产生一圈色偏。alpha 用点采样,不会跨区域。

## 预乘和色彩空间

- `Output` 是**预乘 RGBA**,要用 `Blend One OneMinusSrcAlpha` 画。直通 alpha 的着色器(Unity 的 `Unlit/Transparent`、`UI/Default`、精灵着色器)会把颜色再乘一次 alpha,软边出现暗圈——测试里把它作为负向对照,暗圈的带符号误差 ≈ −19 个灰度级。
- **Gamma 项目:** 直通色 `rgb × a`;已预乘的色原样输出但限制在 `≤ a`(一个比自己的 alpha 还大的颜色会在透明处"加光",编码噪声和色度渗漏会造成这种情况)。
- **Linear 项目:** 在线性值上预乘,硬件写 sRGB 渲染纹理时再编码。直通色:`linear(rgb) × a`;已预乘的色:先除掉 alpha、转线性、再乘回去(`a` 很小时用 `max(a, 1e-4)` 保护;乘回去之后误差被 `a` 压住)。这样**同一个形状无论按哪种方式存,输出一致**,并且在线性帧缓冲里混合是物理上正确的。代价:这和作者在 Gamma 空间合成时看到的样子不完全一样(Unity Linear 工作流的通用现象)。
- `Output` 的 alpha 通道总是线性的、不编码。

## ABI 变化

- `ac_info` 从 48 字节扩到 **80 字节**,在末尾追加:`alpha_mode`、`premultiplied`、`color_range`、`matrix`、`loop_hint`、`alpha_x`、`alpha_y`、`reserved`(都是 `int32`)。`content_w/h` 现在是真值。`has_alpha_plane` 仅在 `native_alpha` 为 1。
- C++ 的 `static_assert`、C# 的 `AcInfo` 和 `CheckLayout()`(80/72/40)、ctypes 的 `Info` 都已同步。Android 的 `libalphaclip.so` 也已重新构建(NDK r30,711,984 字节)并用 `sync_plugin_android_arm64.sh` 同步进 Unity 包(2026-11-06);
  M3 的旧 `.so` 与新 ABI 不兼容(`CheckLayout` 会报错),所以每次改 `ac_info` 都要重新构建并同步它。**M4 还没有在设备上跑过**:没有重建 APK、没有装到手机上。

## Unity 绑定

- `Output` 的尺寸是**画面尺寸**,不是编码帧尺寸(packed 下不同)。`AlphaClipInfo` 新增 `ContentWidth/Height`、`AlphaMode`、`Premultiplied`、`HasAlphaPlane`、`ColorRange`、`Matrix`、`LoopHint`;`HasAlpha` 的含义改为"`Output` 带透明"(`HasAlphaPlane` 才是"有第四个平面")。
- **sidecar 来源:** `OpenFile(path, metaJson = null)`(`null` 时找同目录同名的 `.acmeta.json`,`""` 强制不用)、`Open(byte[]/TextAsset, metaJson = null)`(`null` 时用组件的 `MetaAsset`)、`OpenUriAsync(uri, metaJson = null)`(`null` 时用 `UnityWebRequest` 取同位置的 `.acmeta.json`,读不到就当没有——Android 的 `jar:file://` 同样适用)。`AlphaClipPlayer.SidecarUri` 是公开的小函数,方便测试。
- **sidecar 的矩阵和范围优先于组件设置**(`EffectiveMatrix`、`EffectiveFullRange`),没写才用组件的。
- **材质:** `AlphaClipMaterials.CreateUnlit()`(场景,`AlphaClip/UnlitPremultiplied`)和 `CreateUI()`(UGUI,`AlphaClip/UIPremultiplied`,是 `UI/Default` 的预乘版,Mask / RectMask2D 照常)。着色器放在 `Resources/AlphaClip`,所以一定进玩家构建。**没有做 UI 绑定组件**(`RawImage` 在 ugui 包里,加依赖会碰 Unity 6 的版本号问题);用法是 `raw.texture = player.Output; raw.material = AlphaClipMaterials.CreateUI();`。
- 着色器里对 alpha 的取样用 `tex2Dlod`(分支里不能用需要导数的 `tex2D`)。

## 素材和判据

- `samples/m4/gen_clips.ps1`(只用 ffmpeg lavfi 的自产内容)生成同一个形状(两个柔边圆盘:大的半径 62、14 像素渐变,小的半径 22、10 像素渐变;颜色饱和、随空间和时间变化,**在整个画面里都有定义**)的四种存法:`native_straight`(直通)、`native_premul`(预乘)、`packed_right_premul`(640×180,预乘)、`packed_bottom_straight`(320×360,直通),CRF 10 以压低编码噪声;外加 320×180 的第 0、20、40 帧的 RGBA 真值(未经色度下采样和编码)。
- **判据:** 把一个片子的第 0、20、40 帧叠在三种背景(白、灰、青)上,和解析合成 `truth.rgb × a + 背景 × (1 − a)`(Linear 项目在线性光下)逐像素比较。关键数字是**半透明边缘(真值 alpha 在 5%..95%)的带符号平均误差**:暗圈为负、亮圈为正;另有全图平均绝对误差。限值:平均绝对误差 ≤ 1.0,边缘带符号均值 |.| ≤ 1.0。
- **负向对照**(必须被判为失败,否则判据是瞎的):sidecar 的 `premultiplied` 标志写反(两个方向,各用对应的片子)、用直通 alpha 的场景着色器(`Unlit/Transparent`)、用 Unity 默认的 UI 材质画 RawImage。正确管线的数字在 0.13–0.28(平均绝对误差)和 ±0.06(边缘均值)之内,对照在 ±19–21,限值落在两组之间(对照至少要 10)。
- 两层测试各有分工:**对 CPU 参考**(解码出的 YUV(A) 平面在 CPU 上按同样采样方式转成预乘 RGBA,验证着色器逐字节正确,含 alpha 通道和 packed 裁剪),与 **对真值**(验证整条链——解码、着色器、混合方式——在视觉上没有暗圈)。
- 编辑模式用相机 + 四边形 + `AlphaClip/UnlitPremultiplied`;播放模式用世界空间 Canvas + `RawImage` + `AlphaClip/UIPremultiplied`,另有 `RectMask2D` 压住左半边的检查(右半边必须是纯背景,左半边仍等于真值)。

## 没有测的,写进来免得被当成测过

- **直通色且透明处是黑色的素材**(很多工具导出的 RGBA 就是这样):4:2:0 和滤波会把黑色抹进边缘。`native_straight` 的颜色在透明处是延续的("扩边"过),所以没有覆盖这种情况;怎么给制作工具(M5)提要求,见 m4-conclusion 的交接表。
- 设备(Pixel 6a)上的 M4、Unity 6、HDRP、`keyed`、边缘后处理、真实素材、大分辨率、多个播放器同时播放。
