# M6 笔记

M6 = 预览页:本地 WebGL 预览,和 Unity 渲染对比(设计文档 §3.3 的 TR-4、§11),完成标准"预览与引擎结果肉眼一致"。
结论见 [m6-conclusion.md](m6-conclusion.md)(定稿)。本文记录 M6 里做的设计决定、用法、接口、测试方法和踩过的坑。
改 `tools/preview/`、`shaders/`、`tools/cli/{preview,http_server}.*` 之前先读它;更早的决定见 [m1-notes.md](m1-notes.md) 到 [m5-notes.md](m5-notes.md)。

## 代码布局(M6 新增和改动)

```
shaders/alphaclip_yuv_to_rgba.frag.glsl   着色器主源(GLSL ES 3.0):YUV(A) 平面 -> 预乘 RGBA。预览页原样使用
tools/preview/www/index.html, style.css   预览页
tools/preview/www/app.js                  界面:播放、拖入文件、背景、缩放、sidecar 编辑
tools/preview/www/renderer.js             WebGL2 的两步渲染(和引擎绑定做的一样),可读回像素
tools/preview/www/api.js                  对本地服务的调用、帧的二进制格式
tools/preview/www/autotest.{html,js}      测试用:按任务清单渲染,把像素交回测试脚本
tools/preview/www/uitest.{html,js}        测试用:在 iframe 里操作真正的页面
tools/cli/preview.cpp                     `alphaclip preview`:片子会话、接口、静态文件
tools/cli/http_server.{h,cpp}             只监听 127.0.0.1 的小型 HTTP/1.1 服务(Windows 套接字和 POSIX 两套)
tools/cli/preview_assets.h, embed_assets.cmake   构建时把页面文件生成成 C++ 数组,嵌进 exe
tools/cli/inspect.cpp                     `inspect` 的报告改成可以写进字符串(InspectReport),页面显示的就是它
tools/cli/util.{h,cpp}                    OpenInBrowser(Windows 上 ShellExecuteW)
core/tests/test_preview.py                177 项:服务、着色器对 CPU 参考、合成对真值、对 Unity、页面本身
bindings/unity/TestProject/Assets/Editor/AlphaClipEditModeAlphaTests.cs   新增 -acDumpDir:把画出来的东西写成原始 RGBA
bindings/unity/scripts/run_editmode_tests.ps1                             新增 -DumpDir
```

核心库(`core/`)、C 接口、`alphaclip.dll` / `libalphaclip.so` **没有改动**;`ac_version` 仍是 `0.1.0-m4`。

## 用法

```bash
build/core-win64/bin/alphaclip.exe preview out/clip.webm      # 启动本地服务并打开浏览器;同名的 clip.acmeta.json 会被用上
build/core-win64/bin/alphaclip.exe preview                    # 不带片子:打开空页面,把文件拖进去
```

| 选项 | 作用 |
|---|---|
| `--meta <文件>` / `--no-meta` | 指定 sidecar / 不用 sidecar(和 `inspect`、`verify` 一样) |
| `--port N` | 监听的端口;`0` = 任意空闲端口。不给时用 47800,被占用就自动换一个空闲的 |
| `--no-open` | 不启动浏览器;地址打印成 `url: http://127.0.0.1:N/` |
| `--dev-root <仓库目录>` | 页面文件从仓库里读,而不是用嵌在 exe 里的那份(改页面时不用重新编译) |

按 Ctrl+C 结束。服务最多同时保留 4 个片子(最近最少用的让位),不往磁盘上写任何东西。

页面上能做的:播放 / 暂停 / 逐帧 / 拖时间轴 / 循环 / 速度;拖入 `.webm`(可以连同 `.acmeta.json`,也可以单独拖一个 sidecar 换掉当前的);
背景(棋盘格、任意颜色、几个预设);缩放(适应窗口、25%–800%,滚轮,拖动平移,双线性 / 最近点);只看 alpha / 只看颜色;
色彩空间 Gamma / Linear;混合方式(预乘,或错误的直通 alpha 作对照);编辑 sidecar 的字段并复制 / 下载结果;sidecar 没写时的默认矩阵、范围、色度位置;
鼠标下像素的着色器输出值;`alphaclip inspect` 的报告。界面语言跟随浏览器(中文 / 英文),`?lang=en` / `?lang=zh` 可以强制。

## 决定,以及和设计文档的差异

1. **解码放在本地服务里(你选的方案),不是 WebAssembly,也不是浏览器自带的解码器。** `alphaclip.exe` 用和引擎加载的同一份原生核心解码,把 Y/U/V[/A] 平面交给页面,
   页面只负责上传纹理和画。好处:不用下载任何工具链;解码结果就是引擎里的那一份(逐字节);`encode` 之后直接 `preview` 是一个工具。
   代价:预览时要开着 exe;页面不是一个可以单独发给别人的文件。页面里"帧从哪来"只在 `api.js` 一处,以后换成 WebAssembly 核心不必动渲染代码。
2. **着色器主源落地了,生成脚本没有做。** 设计 §8.3 说"主源用 GLSL ES 3.0 写,生成各引擎版本"。M6 写了主源 `shaders/alphaclip_yuv_to_rgba.frag.glsl`,
   预览页(WebGL2 就是 GLSL ES 3.0)原样使用它,所以"预览版本"不需要生成。Unity 的 `YuvToRgb.shader` **仍是 M4 手写的那份,没有动**;
   两份现在靠 `test_preview.py` 的 C 部分对齐(同一块显卡上输出逐字节相同)。生成脚本留到做 Godot / Cocos 之前。
3. **页面按引擎的两步来画,而不是一步合成。** 第一步"output":平面 → 预乘 RGBA,尺寸是画面尺寸(= Unity 的 `AlphaClipPlayer.Output`);
   第二步"scene":先画背景,再把 output 当成一张贴图用 `One / OneMinusSrcAlpha` 混合上去(= 场景里的四边形或 UI);第三步原样拷到画布。
   这样两步都能单独读回、单独和 Unity 比。
4. **Linear 用真正的 sRGB 帧缓冲模拟,不是在着色器里手算。** Linear 模式下两个目标都是 `SRGB8_ALPHA8`:着色器输出线性值,硬件写入时编码、
   采样时解码、在线性光下混合——和 Unity 的 Linear 工程走的是同一条硬件路径,放大时的滤波也在线性值上做。最后拷到画布时(画布不是 sRGB 目标)在着色器里再编码一次。
5. **参数由核心说了算。** 页面改 sidecar 字段后,把新的 sidecar 文本交给服务重新 `ac_open_memory`;核心接受就换,核心拒绝(422,带 `AC_ERR_*` 和核心的原话)
   页面就显示原因并保留原来的画面。页面自己不实现任何校验规则,所以它和引擎不会各说各话。
6. **取帧接口是无状态的。** `at_us=T`:T 时刻该显示的那一帧(`ac_seek` + `ac_decode_next`);`after_us=P`:pts 为 P 的那一帧之后的一帧,到片尾返回 204。
   服务记着解码器停在哪一帧之后:顺序播放时 `after_us` 正好接上,只解一帧;乱序、重试、多个标签页也都得到正确的帧,只是要多 seek 一次。
   不用帧号:有隐藏帧的片子"包数 ≠ 显示帧数",时间才是核心的寻址方式。
7. **页面文件嵌在 exe 里。** `tools/cli/CMakeLists.txt` 里列出文件(地址 | 仓库里的路径 | 类型),`embed_assets.cmake` 在构建时生成 C++ 数组;
   服务只认这张表里的地址,没有"把地址当路径去读文件"的代码,所以不存在路径穿越。嵌进去的文件一共约 73 KB(exe 现在是 2,884,608 字节)。
8. **测试页也嵌在 exe 里**(`autotest.html`、`uitest.html`):它们必须和接口同源才能取帧。它们只把结果发往 `http://127.0.0.1:<端口>` 的收集器
   (脚本里用正则卡死),页面的内容安全策略也只允许连本机。

## 本地服务

- **只监听 `127.0.0.1`**(`INADDR_LOOPBACK`),别的机器连不上。Windows 上用 `SO_EXCLUSIVEADDRUSE`,别的程序不能抢同一个端口。
- **`Host` 必须是自己**(`127.0.0.1:端口` 或 `localhost:端口`),否则 403:防 DNS 重绑定(别的网站用一个解析到 127.0.0.1 的域名来访问)。
- **带 `Origin` 且不是自己的请求 403**;`/api/` 下的请求如果浏览器标了 `Sec-Fetch-Site: cross-site / same-site` 也 403:别的网站不能让浏览器替它调接口。
- **不写磁盘。** 上传的片子只在内存里。导出 sidecar 是浏览器端的"下载",不经过服务。
- 一个请求一条连接(`Connection: close`);每条连接一个线程读请求,**处理函数串行执行**(一把锁),所以片子会话不需要自己的锁。
  每条连接单独一个线程是为了不让浏览器的空闲预连接卡住别的请求。最初的版本是单线程的,手动试用时没有出过问题;改成多线程是按推理做的
  (浏览器会开一些不发请求的连接,单线程会被它卡到超时),没有实际观测到单线程版本被卡住。现在的版本用 6 条空闲连接测过,不会卡。最多 64 条连接。
- 上限:请求头 16 KiB(431)、请求体 1 GiB(413,和 CLI 读文件的上限一样)、sidecar 请求 1 MiB(核心自己的上限是 64 KiB)、收发超时 15 秒。
  不支持分块上传(501);支持 `Expect: 100-continue`;没有 `Content-Length` 的请求按"没有请求体"处理。
- 每个回复都带 `Cache-Control: no-store` 和 `X-Content-Type-Options: nosniff`;HTML 带内容安全策略(只加载自己的东西,只连本机)。

| 地址 | 说明 |
|---|---|
| `GET /` 等 | 页面文件(只有清单里的那几个) |
| `GET /api/state` | `{"version", "clip": 最近用过的片子或 null}` |
| `POST /api/clips?name=文件名` | 请求体是 `.webm` 的字节 → `{"id","name","size"}`;这时还没有打开 |
| `POST /api/clips/<id>/open` | 请求体是 sidecar 文本,空 = 不用 sidecar → 片子的 `{"id","name","size","meta","info","inspect","error"}`;核心拒绝时 422 `{"error","status"}`,片子保持原样 |
| `GET /api/clips/<id>/frame?at_us=T` 或 `?after_us=P` | 一帧:32 字节小端头(`ACF1`、宽、高、标志(第 0 位 = 有 A 平面)、`pts_us` int64、8 字节保留)+ 紧凑排列的 Y、U、V[、A];到片尾 204 |
| `POST /api/quit` | 结束服务(测试用) |

`info` 就是 `ac_info` 的字段(`alpha_mode` 另带一个名字)。`inspect` 是 `alphaclip inspect` 的报告原文。

## 测试(`core/tests/test_preview.py`,177 项)

```bash
python -I core/tests/test_preview.py --cli build/core-win64/bin/alphaclip.exe --workdir <空目录> \
    --browser "C:/Program Files/Google/Chrome/Application/chrome.exe" --browser-arg=--force_high_performance_gpu \
    --unity-gamma build/m6-unity/dump-gamma --unity-linear build/m6-unity/dump-linear
```

- **A(74 项,不需要浏览器):** 嵌入的文件逐字节等于仓库里的;10 个样本片,服务交出的每一帧逐字节等于 `alphaclip decode`(顺序走完、随机 seek、乱序要"下一帧"、
  片头之前、同一帧要两次);换 sidecar、7 种被核心拒绝的 sidecar(片子保持原样)、不是 WebM 的文件、截断的片子、带路径和中文的文件名、超过 4 个片子;
  命令行(不存在的文件、端口被占、坏参数、被拒绝的 sidecar 也能启动、`--no-meta`);HTTP:别的 Host / Origin / 跨站标记被拒、14 种畸形请求 400、
  头过大 431、体过大 413、分块 501、半截请求不回复、6 条空闲连接不卡住别的请求、**300 例变异请求之后服务还在**。
- **B(53 项,无头浏览器):** `autotest.html` 用页面自己的 `renderer.js` 跑 144 个任务,把读回的像素发给脚本。
  ① 着色器输出对 CPU 参考(脚本里用 numpy 按同样的公式实现):72 张,四种透明模式、两种矩阵、两种范围、两种色度位置、奇数尺寸,Gamma 和 Linear,最大差 1。
  ② 合成对"输出的 CPU 混合":140 张。③ M4 的四个片子对**精确真值**,用 M4 的判据(全图平均绝对误差 ≤ 1.0、软边带符号均值在 ±1.0 以内),
  加两个必须失败的对照(sidecar 的 `premultiplied` 写反、用直通 alpha 混合)。④ 放大 2×:最近点 = 每个像素重复两次;双线性 = 每个像素落在相邻像素之间。
- **C(12 项):** Unity 的编辑模式透明测试加了 `-acDumpDir`,把它画的每一张图(着色器输出、三种背景上的合成、两个对照)写成原始 RGBA;
  脚本拿同名的预览页结果逐像素比,并写出"Unity | 预览 | 差 ×64"的对比图(`<workdir>/compare/*.png`)。
- **D(38 项):** `uitest.html` 在 iframe 里打开真正的页面并像人一样操作:自动播放、每一帧都按顺序出现、速度对、暂停 / 逐帧 / 拖时间轴 / 片尾停住 / 循环、
  背景、缩放、滚轮、拖动、像素读数、改 sidecar(被拒绝时显示原因并保留画面)、拖入片子 / 单独拖入 sidecar / 拖入坏文件;页面屏幕上的画面等于 CPU 参考。

Unity 的导出:

```powershell
powershell -File bindings/unity/scripts/run_editmode_tests.ps1 -ColorSpace Linear -Pairs "samples\m0\opaque.webm|opaque.raw" `
    -AlphaClips "a.webm;b.webm;c.webm;d.webm" -Truth samples\m4\truth_rgba_3f.raw -DumpDir build\m6-unity\dump-linear -OfflineGuard
```

**"和 Unity 一样"的判据是事先定的:任何像素最多差 2 级、平均不超过 0.1 级。** 同一块显卡上(Unity 和浏览器都用 RTX 4060)实测远在其内(输出 0,合成最多 1)。
浏览器用另一块显卡时,Gamma 仍在判据内;Linear 下有个别值差到 3,这条判据因此在看到数据之后另加了一档——"换了显卡的 Linear:最多 4 级,超过 2 级的值不超过万分之一"——
原因见下面第一条坑。这一档是事后定的,请按这个身份看待它。

## 踩过的坑

- **Linear 下显卡之间在接近黑色的地方互相差 2–3 级。** sRGB 曲线在暗部很陡(线性光的一小步是好几个灰度级),硬件"解码 → 混合 → 编码"的精度各家不同。
  实测:蓝色通道存的是 2、alpha 254、叠在白色上——按公式算是 14,Intel UHD 和 SwiftShader 与公式相差不超过 1,RTX 4060 给 16;每张图最多 3 个这样的值
  (全部 1620 万个值里 71 个)。Unity 在 RTX 4060 上同样的像素也是 16(查过它导出的图),所以这不是预览页和 Unity 的差别,而是显卡之间的差别。
  M4 笔记里"Linear 下单点误差最大 38、原因没查"很可能是同一类现象(那是对真值比,还叠加了编码误差),这里没有去证实。
- **无头 Chrome 默认用集成显卡**(这台机器是 Intel UHD),Unity 用的是 RTX 4060。要在同一块卡上比,给浏览器加 `--force_high_performance_gpu`。
  软件渲染是 `--use-angle=swiftshader --enable-unsafe-swiftshader`。
- **Edge 启动后会换一个进程**:脚本启动的那个 `msedge.exe` 把活交给自己的另一个进程然后退出,`taskkill /PID <启动的那个> /T` 什么也杀不到,
  无头 Edge 就一直留在后台(发现时已经攒了几十个进程;只清掉了带测试目录的那些)。现在按"命令行里带着这次启动专用的 profile 目录"来结束进程,不会碰到用户自己的浏览器;
  每次启动用一个新的 profile 目录(刚被强行结束的 profile 可能还锁着)。Chrome 没有这个问题。
- **不让测试浏览器联网:** 启动参数里给一个不存在的代理(`--proxy-server=http://127.0.0.1:9`);Chromium 对回环地址默认不走代理,所以本机的服务照常能连。
  不要加 `--proxy-bypass-list=<-loopback>`,那是把"回环不走代理"取消掉。
- `curl -X POST` 不带 `-d` 时不发 `Content-Length`。一开始服务回 411,后来改成按规范当作没有请求体。
- CMake 的列表用分号分隔,`text/html; charset=utf-8` 这样的值会被拆开:清单里只写 `text/html`,字符集在 C++ 里补。
- 用脚本派发的 `PointerEvent` 调 `setPointerCapture` 会抛异常(浏览器不认识这个指针),页面里包了一层 try。
- Python 标准库没有 WebSocket,所以没有走"用调试协议驱动浏览器"的路:测试页自己把结果 POST 给脚本开的一个小收集器(跨源,收集器回 CORS 头)。
- 读回 `SRGB8_ALPHA8` 帧缓冲得到的是**编码后的字节**(存的是什么就是什么),不会被线性化:Unity 的 `ReadPixels` 也是这样,所以两边可以直接比字节。

## 没有测的,写进来免得被当成测过

- **浏览器:** 只有 Chromium 系(Chrome 156、Edge 157,都是 Windows、无头模式)。Firefox、Safari 没有测(本机没有装 Firefox)。
- **和 Unity 的对比范围:** 只有 2022.3.62f2、内置管线、编辑模式的场景路径(相机 + 四边形);M4 的四个合成片(320×180)的第 0、20、40 帧、三种背景、1:1。
  URP、UI 路径(RawImage)、Unity 6、播放器构建、Pixel 6a、真实素材、大分辨率、放大后的画面都没有和 Unity 比过。
- **"肉眼":** 测试比的是帧缓冲里的字节,不是屏幕上的光。浏览器和 Unity 窗口各自的显示色彩管理不在测试范围内。
- **性能:** 没有量。每一帧是一次本机 HTTP 请求,1080p 一帧约 3–5 MB;大分辨率能不能实时播没有试过。
- **POSIX:** `http_server.cpp` 的 POSIX 分支和 `OpenInBrowser` 的 `xdg-open` / `open` 分支只在 Android arm64 的交叉编译里编译过,没有运行过。
- **消毒器:** 新的 C++ 代码(HTTP 服务、`preview`)没有在 ASan / UBSan 下跑;只有 300 例变异请求和手写的畸形请求。
- `keyed`、边缘后处理(M7)页面上还没有;"调参数"目前只有 sidecar 的字段和默认值。
- 着色器生成脚本;Unity 的着色器没有改成从主源生成。
