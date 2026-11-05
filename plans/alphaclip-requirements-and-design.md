# AlphaClip(暂定名)— 跨引擎透明视频插件:需求与设计方案

- 状态:**草案 v0.2**(v0.1 于 2026-11-04;v0.2 于 2026-11-05 按 [M0 结论](../docs/m0-conclusion.md)修订,改动处都标了「M0」)
- 用途:作为新项目的起点文档。新项目里把本文件放进 `docs/`,让 Claude 先读它再动手。
- 开发节奏:**小步慢做,每个里程碑独立可验证、可随时暂停**(见第 11 节)。
- 范围:一套 C++ 核心 + Unity / Godot 4 / Cocos Creator 3.x 三个薄绑定 + 离线制作工具。

> 关于来源:本方案受一款商业手游里的视频插件(Leviathan)启发。我们只借鉴"用标准视频加透明数据、在 GPU 里还原透明"这一思路,
> **不复刻它的文件格式、代码和资源**。第 12 节说明了合规边界,附录 A 记录当时观察到的事实。

---

## 1. 背景与问题

游戏里的特效和演出常常需要"带透明的视频"(循环背景光效、建筑建造动画、活动出场演出等)。现有方案各有短板:

| 引擎 | 现状 | 短板 |
|---|---|---|
| Unity | 自带 `VideoPlayer` | Android 上黑屏、机型差异大(编码档次、解码器数量限制等);逐像素透明只文档化支持 ProRes 4444 和 WebM/VP8,Android 上官方建议转码并勾选 Keep Alpha 才稳定;VP9 不支持导入(Unity 6 文档,2026-11 核对) |
| Godot 4 | 自带 `VideoStreamPlayer` | 据我所知只支持 Theora(.ogv),没有 VP9/H.264,更没有透明 |
| Cocos Creator 3.x | 自带 `VideoPlayer` 组件 | 各平台走各自原生播放器,行为不一致,不支持透明 |

目标是**一套统一的格式、统一的解码、统一的渲染效果**,在三个引擎里表现一致,并且不依赖设备自带的硬件解码器。

## 2. 目标与非目标

### 2.1 目标

1. 播放带透明的短视频(典型 1~10 秒、循环或单次),三个引擎效果一致。
2. 使用标准、开放的视频编码(首选 VP9,WebM 容器),普通播放器也能打开原始画面。
3. 透明通道支持多种来源(第 5 节的四种模式),制作方可按素材选择。
4. 软解为主,行为可预测,不受机型硬解差异影响。
5. 有配套离线工具:编码、校验、预览、调参。
6. 每个引擎的接入代码尽量薄,核心逻辑只写一份。

### 2.2 非目标(v1 明确不做)

- 通用视频播放器、流媒体、直播、DRM、字幕、章节。
- H.264 / HEVC(专利和授权成本);AV1 作为后期可选项。
- 长视频(几分钟以上)和 4K。软解扛不住,且不是目标场景。
- 视频编辑、剪辑、特效合成。
- 首期不做 iOS、macOS、WebGL(放在里程碑后段,见第 11 节)。

## 3. 需求

### 3.1 功能需求(FR)

| 编号 | 需求 | 优先级 |
|---|---|---|
| FR-1 | 打开:从内存字节(`byte[]`/TextAsset)和文件路径两种来源 | P0 |
| FR-2 | 异步 Prepare,带回调:已就绪、播放结束、出错 | P0 |
| FR-3 | 播放控制:Play / Pause / Stop / Seek / Loop | P0 |
| FR-4 | 不透明视频播放(YUV→RGB) | P0 |
| FR-5 | 透明模式:`packed_right`、`packed_bottom`(并排拼接) | P0 |
| FR-6 | 透明模式:`native_alpha`(WebM 原生 alpha,yuva420p) | P1 |
| FR-7 | 透明模式:`keyed`(绿幕 + 参数/查找表抠图) | P2 |
| FR-8 | 边缘处理:收缩、羽化、去噪(可选后处理) | P2 |
| FR-9 | 输出预乘 alpha,便于直接叠加到 UI 和场景 | P0 |
| FR-10 | 多实例同时播放,有总数上限和降级策略 | P1 |
| FR-11 | 音频(可选):单独一个音轨解码并交给引擎音频系统 | P3 |
| FR-12 | 无缝循环(循环点不闪、不卡) | P1 |
| FR-13 | 引擎内资源导入:编辑器里直接拖入即可预览 | P1 |

### 3.2 非功能需求(NFR)

| 编号 | 需求 | 说明 |
|---|---|---|
| NFR-1 | 性能 | 解码 + 上传的耗时要有可测的预算。**初始目标待基线测量后确定**(M3 之后校准),先定义指标:解码 ms/帧、上传 ms/帧、主线程占用、掉帧率 |
| NFR-2 | 内存 | 帧队列长度可配置(默认 4);单实例常驻内存可查询 |
| NFR-3 | 包体 | 核心库(仅 libvpx + libwebm,arm64)目标 < 2 MB,待实测 |
| NFR-4 | 稳定性 | C 接口不抛异常,统一返回错误码;任何格式错误都不能崩溃 |
| NFR-5 | 线程安全 | 解码线程与主线程之间只通过有界队列通信 |
| NFR-6 | 可测试 | 解码结果可与 ffmpeg 参考输出逐帧对比 |
| NFR-7 | 合规 | 只用宽松许可的依赖(BSD / MIT / Apache);FFmpeg 仅在需要时动态链接 |
| NFR-8 | Android 兼容 | 原生库要满足 Google Play 对 16 KB 页大小的要求(要求细节在做 M3 时按官方最新文档核对) |

### 3.3 工具需求

| 编号 | 需求 |
|---|---|
| TR-1 | 命令行 `alphaclip encode`:把素材编成 VP9 + 透明数据,输出标准文件 + 元数据 |
| TR-2 | 命令行 `alphaclip inspect`:打印编码、分辨率、帧率、透明模式、元数据版本 |
| TR-3 | 命令行 `alphaclip verify`:用核心库解码,与参考输出逐帧比对 |
| TR-4 | 本地预览页:拖入文件、切换背景色、调参数,所见即所得 |
| TR-5 | `keyed` 模式的参数/查找表生成器(P2,研究性质) |

## 4. 总体架构

```
 制作端                                    运行端
┌──────────────────────┐   文件包    ┌────────────────────────────────────────────┐
│ alphaclip CLI        │ ─────────▶ │  引擎绑定 (Unity / Godot / Cocos)           │
│  encode/inspect/verify│            │   ├─ 播放组件 (控制、事件、资源加载)         │
│ 预览页 (WebGL)        │            │   ├─ 纹理上传 (Y/U/V[/A] 平面)              │
└──────────────────────┘            │   └─ 着色器 (YUV→RGB、透明还原、边缘)         │
                                    │            ▲                                │
                                    │            │ C ABI                          │
                                    │  C++ 核心 (alphaclip core)                  │
                                    │   ├─ 解封装 (webm_parser)                   │
                                    │   ├─ 解码线程 (libvpx → YUV[A] 帧)          │
                                    │   └─ 有界帧队列 + 时钟                       │
                                    └────────────────────────────────────────────┘
```

**分层原则**

1. 核心只做"字节进、YUV(A) 帧出",**不碰任何图形 API**。
2. 绑定只负责三件事:把字节交给核心、把平面数据传成纹理、用着色器画出来。
3. 着色器逻辑**写一份源,生成各引擎版本**(第 7 节),避免三套各自漂移。

## 5. 透明模式设计

| 模式 | 视频里放什么 | 透明数据在哪 | 优点 | 代价 |
|---|---|---|---|---|
| `opaque` | 普通画面 | 无 | 最简单 | 无透明 |
| `packed_right` / `packed_bottom` | 画面 + 灰度透明通道并排 | 同一个视频 | 一个文件,实现简单 | 有效分辨率减半或解码量加倍 |
| `native_alpha` | yuva420p | WebM 的附加块(BlockAdditional) | 全分辨率,透明精度最高 | 制作端需要真透明素材;**M0 已验证可行**(Windows 与 Pixel 6a 上与 ffmpeg 逐字节一致)。代价:每个实例要**两个解码器**(彩色、alpha 各一个),alpha 路额外约 +32%~+45%(Pixel 6a)/ +69%(Windows 纯 C)的解码时间;两路关键帧必须对齐 |
| `keyed` | 绿幕(或其他纯色)背景画面 | 一个小的参数/查找表文件 | 素材只需绿幕,不需要真透明 | 抠图质量依赖算法与调参;研究成本最高 |

> `keyed` 的思路来自对那款游戏的观察(附录 A):视频本体不带 alpha,另存参数加一张颜色查找表,运行时在着色器里还原颜色和透明。
> 这里只借鉴思路,算法和文件格式自行设计。

**选择建议**:有真透明素材 → `native_alpha`;只有绿幕素材 → `keyed`;最简单可用 → `packed_*`。

## 6. 文件与元数据设计(自研,v0.1)

### 6.1 原则

- **画面文件始终是标准 WebM**,任何播放器都能打开(此时看不到透明)。
- 透明相关参数放在**同名 sidecar 元数据**里,必要时再附加查找表图片。
- 元数据带版本号,核心库对未知版本拒绝加载并返回明确错误。
- 后期可选:把 WebM + 元数据 + 查找表打成一个单文件,方便资源包管理(P3)。

### 6.2 文件组成

```
clip.webm              标准 WebM (VP9)
clip.acmeta.json       元数据(必需,即使是 opaque 也带,方便统一流程)
clip.aclut.png         查找表(仅 keyed 模式)
```

### 6.3 元数据字段草案

```jsonc
{
  "schema": 1,
  "alpha_mode": "opaque | packed_right | packed_bottom | native_alpha | keyed",
  "premultiplied": true,            // 输出是否预乘(着色器按此处理)
  "color_range": "limited | full",  // YUV 取值范围
  "matrix": "bt601 | bt709",
  "content_size": [986, 572],       // 有效画面尺寸(packed 模式下和视频尺寸不同)
  "loop_hint": true,
  "edge": { "shrink": 0.0, "feather": 0.0, "denoise": 0.0 },
  "keyed": {                        // 仅 keyed 模式
    "lut": "clip.aclut.png",
    "lut_size": 32,
    "key_color": [0, 254, 0],
    "floor": 0.0, "ceil": 1.0, "gamma": 1.0
  }
}
```

字段名和含义在 M1 之后可能调整,**以核心库的 schema 校验为准**。

## 7. 核心库设计(C++17,C ABI)

### 7.1 依赖

| 依赖 | 用途 | 许可 |
|---|---|---|
| libvpx | VP8/VP9 解码(自带 NEON 优化)。**M0:** `native_alpha` 要开两个解码器实例,alpha 流的 Y 平面就是全范围 0–255 的 alpha | BSD-3 + 专利授权 |
| libwebm | WebM 解封装。**M0:** 用事件回调式的 `webm_parser`,**不是** `mkvparser`(后者读不到 BlockAdditions);上游默认不编它,需直接编译其源码(CMake 选项 `ENABLE_WEBM_PARSER`) | BSD-3 |
| dav1d(可选,后期) | AV1 解码 | BSD-2 |
| FFmpeg(可选) | 仅当必须支持其他容器/编码时,动态链接 | LGPL(取决于构建选项) |

### 7.2 C 接口草案

```c
typedef struct ac_player ac_player;

typedef enum {
  AC_OK = 0, AC_AGAIN,           // AGAIN:当前没有新帧
  AC_ERR_IO, AC_ERR_FORMAT, AC_ERR_UNSUPPORTED,
  AC_ERR_DECODE, AC_ERR_STATE, AC_ERR_OOM
} ac_status;

typedef struct {
  int32_t width, height;         // 视频尺寸
  int32_t content_w, content_h;  // 有效画面尺寸
  int64_t duration_us;
  float   fps;
  int32_t has_alpha_plane;       // 是否有第 4 个 A 平面
} ac_info;

typedef struct {
  const uint8_t* plane[4];       // Y, U, V, A(可为空)
  int32_t stride[4];
  int32_t width, height;
  int64_t pts_us;
  uint32_t flags;
} ac_frame;

ac_status ac_open_memory(const uint8_t* data, size_t len, const char* meta_json,
                         ac_player** out);          // data 在播放器存活期间必须有效
ac_status ac_open_file(const char* path, const char* meta_json, ac_player** out);
ac_status ac_get_info(ac_player*, ac_info*);
ac_status ac_start(ac_player*);                      // 启动解码线程
ac_status ac_seek(ac_player*, int64_t pts_us);
ac_status ac_acquire_frame(ac_player*, int64_t clock_us, ac_frame* out);
ac_status ac_release_frame(ac_player*, ac_frame*);
const char* ac_last_error(ac_player*);
void ac_close(ac_player*);
```

### 7.3 线程与时钟

- 一个解码线程:解封装 → 解码 → 写入有界环形队列(默认 4 帧)。
- 主线程每帧调用 `ac_acquire_frame(clock)`:取出 `pts <= clock` 的最新一帧,更早的直接丢弃。
- 时钟由绑定提供(默认用引擎的游戏时间;有音频时以音频为主时钟,P3)。
- 循环:到达末尾后 `seek(0)` 并给后续帧加上 `pts` 偏移,保证时钟连续。
- **M0:** `native_alpha` 里,同一个数据包的彩色数据和 BlockAdditional(BlockAddID 1)分别交给两个解码器。
  seek 只能落在 Cues 指向的、**两路都是关键帧**的位置(从非关键帧起解码会返回错误,不会崩溃)。
  `webm_parser` 是流式解析器,容器层 seek 要自己维护 Cues 表,并在跳转后调用 `WebmParser::DidSeek()`。
- **M0:** 码流里的色彩空间和范围信息是"未指定",`matrix` / `color_range` 必须来自 sidecar 元数据,不能依赖码流。

### 7.4 错误处理

- 所有函数返回 `ac_status`,不抛异常;`ac_last_error` 给出人能读的原因。
- 对非法输入(损坏的 WebM、超大分辨率、元数据不合法)一律返回错误,**不允许崩溃**(后期加模糊测试)。

## 8. 渲染设计

### 8.1 纹理

- Y、U、V(以及可选的 A)各一张单通道 8 位纹理(`R8`)。U、V 为半分辨率。
- 纹理必须是**非 sRGB**,否则颜色会被错误地转换。

### 8.2 着色器职责

1. YUV → RGB(按元数据的 `matrix` 与 `color_range`)。
2. 透明还原:`packed_*` 读另一半画面的灰度;`native_alpha` 读 A 平面;`keyed` 读查找表。
3. 输出预乘 alpha(混合模式为 `One OneMinusSrcAlpha`)。
4. 可选边缘后处理(收缩、羽化、去噪),放在单独一遍里,默认关闭。

### 8.3 一份源,多端生成

- 主源用 GLSL ES 3.0 写,放在 `shaders/`。
- 用一个小生成脚本产出:Unity ShaderLab(HLSL)、Godot Shading Language、Cocos `.effect`、预览页的 WebGL 版本。
- 每个引擎里都有"渲染一张固定测试图并和参考图比对"的黄金图测试(第 10 节),防止漂移。

### 8.4 `keyed` 模式的查找表采样

- 查找表是 32×32×32 的 3D 表,排成 1024×32 的横条图。
- 2D 横条采样需要手动做三线性:先在两个相邻切片内做双线性,再在切片间插值。
- 注意:**Unity 项目若使用 Linear 色彩空间**,需要在着色器里补上 sRGB/线性转换,否则抠边颜色会偏(已列入风险)。

## 9. 引擎绑定设计

> 下面各引擎的具体 API 名称,在实现阶段必须按**对应版本的官方文档**核对,本文只定结构。

### 9.1 Unity

- 形态:UPM 包 `com.<你的域名>.alphaclip`。
- 结构:
  - `Runtime/`:`AlphaClipPlayer`(MonoBehaviour,控制与事件)、`AlphaClipGraphic`(UGUI)、`AlphaClipRenderer`(世界空间)。
  - `Plugins/`:`Android/arm64-v8a/libalphaclip.so`、`Windows/x86_64/alphaclip.dll`、(后期)macOS、iOS。
  - `Shaders/`:生成的 ShaderLab。
  - `Editor/`:导入器(`.webm` + `.acmeta.json` 识别为一个资源)、预览 Inspector。
- 数据来源:
  - 内存:`TextAsset.bytes` / `byte[]`(固定内存地址传给核心)。这也是那款游戏的做法:视频存成 TextAsset 随 AssetBundle 下发。
  - 文件:`StreamingAssets` 在 Android 上位于 APK 内,不能直接当文件路径用,需要先读出字节或拷到可读目录。
- 纹理上传:
  - v1:`Texture2D.LoadRawTextureData(IntPtr, size)` + `Apply(false)`,实现最简单。
  - v2(优化):原生渲染插件 + `GL.IssuePluginEvent`,在渲染线程用 `glTexSubImage2D` / Vulkan 暂存缓冲上传,减少主线程停顿。

### 9.2 Godot 4

- 形态:GDExtension(`godot-cpp`),注意 `godot-cpp` 版本需与引擎小版本匹配。
- 类:`AlphaClipStream : VideoStream`、`AlphaClipStreamPlayback : VideoStreamPlayback`,这样可以直接用现成的 `VideoStreamPlayer` 节点。
- 纹理:用 `ImageTexture` / `RenderingServer` 更新平面纹理;着色器用 Godot Shading Language。
- 透明合成:把着色器挂在 `CanvasItem` 材质上;3D 场景另做 `ShaderMaterial` 版本。

### 9.3 Cocos Creator 3.x

- 原生平台:原生引擎扩展(C++ + JSB 绑定),TypeScript 组件 `AlphaClipPlayer`。
- 纹理:通过引擎提供的纹理更新接口上传平面数据;材质用自定义 `.effect`。
- Web 平台:**不走原生库**,单独做一个 TypeScript 实现(浏览器自带 WebCodecs 或 WASM 版 libvpx),放在后期里程碑,先不承诺。

### 9.4 绑定层共同行为

| 行为 | 约定 |
|---|---|
| 生命周期 | 组件销毁时必须 `ac_close`;热重载/切场景不泄漏线程 |
| 并发上限 | 全局最多 N 个实例(可配置),超出时返回错误或降级为静态首帧 |
| 暂停 | 应用切后台时暂停解码线程,回到前台后恢复 |
| 日志 | 统一前缀,错误带 `ac_last_error` 的文本 |

## 10. 测试与质量

| 类别 | 做法 |
|---|---|
| 解码正确性 | 同一段视频,核心库输出的 YUV 与 ffmpeg 参考输出逐帧比较(MD5 / PSNR) |
| 着色器黄金图 | 每个引擎渲染固定测试画面,与参考图比对(允许小容差) |
| 透明质量 | 用已知真值(带 alpha 的渲染)合成绿幕,再抠回,计算 alpha 误差 |
| 性能基线 | 解码 ms/帧、上传 ms/帧、主线程占用、内存;在 Pixel 6a 上先测,后续补低端机 |
| 稳定性 | 循环播放 1 小时浸泡;反复创建销毁 1000 次;损坏文件模糊测试 |
| 测试素材 | **只用自己生成的测试片**,不使用任何第三方游戏的素材 |

## 11. 里程碑(小步慢做)

每一步都应该**能独立验证、做完就能停**。没有硬性时间表。

| 阶段 | 内容 | 完成标准 |
|---|---|---|
| **M0 工具链** ✅ | 装好 CMake、Ninja、Windows 编译器、对应版本的 Android NDK;拉取 libvpx、libwebm;验证 libvpx 读 WebM alpha 的可行性 | 能编译出 Windows 和 Android arm64 的空库;alpha 可行性有结论。**已完成(2026-11-05),结论见 `docs/m0-conclusion.md`** |
| **M1 核心解码** | 核心库 + 命令行小工具:WebM → 逐帧输出 YUV/PNG。含 M0 遗留的容器层 seek(Cues + `DidSeek`)和循环的时钟逻辑 | 与 ffmpeg 参考输出逐帧一致(alpha 的参考输出要用 `-c:v libvpx-vp9`) |
| **M2 Unity 编辑器原型** | Windows 编辑器里播放不透明视频 | 画面正确、能循环、不泄漏 |
| **M3 Android 真机** | 同一套代码在 Pixel 6a 上播放;记录性能基线 | 真机播放正确,基线数据入库 |
| **M4 透明(基础)** | `packed_right`/`packed_bottom` + `native_alpha`,预乘输出 | 在 UI 和场景里叠加无黑边 |
| **M5 制作工具** | `encode` / `inspect` / `verify` 命令行 | 一条命令把素材变成可播放的包 |
| **M6 预览页** | 本地 WebGL 预览,和 Unity 渲染对比 | 预览与引擎结果肉眼一致 |
| **M7 边缘与 keyed** | 边缘后处理;绿幕 + 查找表抠图与生成器 | 质量指标达标,有对比图 |
| **M8 Godot** | GDExtension,`VideoStreamPlayer` 可用 | 示例场景里播放透明视频 |
| **M9 Cocos** | 原生扩展 + TS 组件 | 示例工程里播放透明视频 |
| **M10 扩展平台** | iOS / macOS / Cocos Web / WebGL(按需) | 每个平台单独评估 |
| **可选** | 音频、AV1、原生硬解通道(MediaCodec) | 按需求单独立项 |

## 12. 许可与合规

1. **不复刻**那款游戏的文件格式、代码、着色器或任何资源;本方案只借鉴公开可知的通用思路(绿幕抠图、查找表、并排拼接透明)。
2. 本机 `E:\MergeIsland\exported_videos\` 里是从那款游戏导出的第三方视频,**不要放进新项目仓库**,也不要当测试素材。测试素材自己生成。
3. 依赖只选宽松许可;若引入 FFmpeg,必须动态链接并按其许可要求附带声明。
4. VP9 / AV1 相对 H.264 / HEVC 的授权成本更低,但**法务结论不是我给的**,正式发布前请让公司法务确认。
5. 本项目自身的许可证由你决定(见第 13 节待定项)。

## 13. 风险与待定事项

### 13.1 风险

| 编号 | 风险 | 缓解 |
|---|---|---|
| R1 | 软解 VP9 在低端 Android 上吃不消 | M3 建立基线;限制分辨率和并发;后期加 MediaCodec 通道。**M0 数据点:** Pixel 6a、640×360、单线程,彩色加 alpha 合计约 0.83 ms/帧(大核)到 3.4 ms/帧(小核);耗时随调度到的核类型跳动,基线必须固定核;目标规格未定,不要外推 |
| R2 | ~~libvpx 读不到 WebM 原生 alpha,或行为不稳定~~ | **已关闭(M0,2026-11-05):** `native_alpha` 可行,见 `docs/m0-conclusion.md`。残留的关键帧对齐问题见 R10 |
| R3 | Unity Linear 色彩空间下抠边偏色 | 着色器里显式处理转换;黄金图覆盖两种色彩空间 |
| R4 | 三个引擎的纹理上传接口细节和版本差异 | 每个引擎只做薄层;API 名称实现时核对官方文档 |
| R5 | Android 16 KB 页大小等平台新要求 | M3 时按 Google 官方最新文档核对并用对应 NDK 构建。**M0:** NDK r30 默认 16 KB 对齐(已验证 ELF 段对齐);APK 内 zip 对齐和 Play 检查留到 M3;Unity 2022.3 需 ≥ 2022.3.56f1 且 Burst ≥ 1.8.21 |
| R6 | `keyed` 抠图质量上限不明 | 放在 M7,先用并排/原生 alpha 打通主链路 |
| R7 | 着色器多端生成产生漂移 | 黄金图测试 + 单一主源 |
| R8 | 多实例同时解码导致卡顿 | 全局并发上限与降级策略(FR-10) |
| R9 | WebM 无 Cues 时 seek 慢或失败 | 制作工具强制写入 Cues;`verify` 检查 |
| R10 | `native_alpha` 的彩色流与 alpha 流关键帧不对齐(如场景切换),seek 后 alpha 无法解码(M0 新增) | M5 的 `verify` 检查"每个 Cues 指向的块,两路都是关键帧";M0 样本是固定 GOP,错位情况未测 |

### 13.2 待你决定的事项

- [ ] 项目正式名称(目前 AlphaClip 只是占位)
- [ ] 项目许可证
- [ ] 你们的素材主要是"真透明"还是"绿幕"?(决定 `keyed` 的优先级)
- [ ] 典型分辨率、帧率、同时播放数量的上限
- [ ] 是否需要音频
- [ ] 目标 Unity 的最低版本(M0 核对:本机有 2022.3.20f1c1、2022.3.62f2、6000.2.10f1、6000.3.2f1;2022.3 要 ≥ 2022.3.56f1 才支持 16 KB 页)
- [ ] 是否确定只做 VP9,不做 H.264
- [ ] 首期平台:Android arm64 + Windows 编辑器,是否同意

---

## 附录 A:对那款游戏 Leviathan 的观察(事实 + 推断)

**事实(直接从安装包里看到的)**

- 视频存放在 AssetBundle 里,以 TextAsset 字节形式存放:54 个文件。
  - 40 个是 WebM(VP9),2 个是 IVF(VP9),12 个是 `_alpha` 私有格式的透明数据文件。
  - 画面多为 986×572(地图建筑演出)和整屏 UI 背景;帧率以 24 为主,少量 25 和 30。
- 随包有精简的 FFmpeg(Android arm64):avcodec、avformat、avutil。构建配置里关闭了 swscale、avfilter、swresample、avdevice 和命令行程序。
- 有 13 个着色器,命名里能看到 `YUV420P`、`Alpha_Right`、`Alpha_Bottom`、`AlphaPro`、`ChromaKey`、`3D` 变体,以及边缘滤波和一个带锐化参数的着色器。
- `_alpha` 文件由"参数 JSON + 一张 1024×32 的 PNG(32 级 RGBA 查找表)"组成。JSON 里有绿幕背景色、抠图阈值、alpha 上下限与伽马、边缘去噪/收缩/羽化等字段。

**推断(没有验证)**

- 视频本体是绿幕画面,透明数据由离线工具生成,运行时在着色器里按查找表还原。
- 不走 swscale,YUV 平面直接上传 GPU 转 RGB。
- 选择自带 FFmpeg 的原因可能是机型差异和透明合成需求。

**没法判断的**

- 抠图的实际质量(没解码播放过)、离线工具的形态、业务代码实现(在加密的程序集里,未触碰)。

## 附录 B:建议的新项目目录结构

```
alphaclip/
├─ docs/                     本文档与后续设计记录
├─ core/                     C++17 + CMake
│  ├─ include/alphaclip.h
│  ├─ src/
│  ├─ third_party/           libvpx、libwebm
│  └─ tests/
├─ shaders/                  GLSL ES 3.0 主源 + 生成脚本
├─ bindings/
│  ├─ unity/                 UPM 包
│  ├─ godot/                 GDExtension
│  └─ cocos/                 原生扩展 + TS 组件
├─ tools/
│  ├─ cli/                   alphaclip 命令行
│  └─ preview/               本地预览页
└─ samples/                  自己生成的测试片和示例工程
```

## 附录 C:本机环境(2026-11-04 初稿,2026-11-05 按 M0 更新)

| 项 | 状态 |
|---|---|
| Unity | 已装,编辑器在 `D:\Program Files\Unity\Hub\Editor`:2022.3.20f1c1、2022.3.62f2(都自带 NDK r23b)、6000.2.10f1、6000.3.2f1(后两者没装 Android 模块) |
| Cocos Creator | 3.8.8,`C:\ProgramData\cocos\editors\Creator\3.8.8` |
| Godot | 4.7,`D:\Tools\godot-4.7` |
| Android SDK | `E:\AndroidSDK`;`ndk-bundle` 是空壳。M0 装了 NDK r30:`E:\AndroidSDK\ndk\30.0.16248370` |
| 真机 | Pixel 6a(adb 已连接;M0 在上面跑通过,Android 15,4 KB 页) |
| 已有运行时 | Java 11、Python 3.12、Node、.NET |
| M0 新装(均不在 PATH,装在 `E:\Tools`) | CMake 4.4.4、Ninja 1.13.2、llvm-mingw(clang);另有 ffmpeg 9.0.2,仅作外部工具。Windows 上暂无 nasm,libvpx 为纯 C 构建 |
| 你自己的项目 | 已使用 YooAsset(`com.tuyoogame.yooasset`) |

## 附录 D:新项目首次对话建议提示词

> 请先阅读 `docs/alphaclip-requirements-and-design.md`。
> 我们现在做 **M0**:只做工具链准备和 libvpx 读 WebM alpha 的可行性验证,不要写引擎绑定。
> 下载任何东西前先告诉我文件名、来源和大小,等我确认。
> 完成后给我一份 M0 结论,再决定 M1。
