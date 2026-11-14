# 预览页

`alphaclip preview` 用的网页:把片子按引擎里的方式画在浏览器里(同一份原生核心解码,同一个着色器,同样的两步渲染)。

```bash
build/core-win64/bin/alphaclip.exe preview out/clip.webm    # 启动只监听本机的小服务并打开浏览器
build/core-win64/bin/alphaclip.exe preview                  # 空页面,把 .webm(和 .acmeta.json)拖进去
```

- `www/` 里的文件和 `shaders/alphaclip_yuv_to_rgba.frag.glsl` 在构建时被嵌进 `alphaclip.exe`(清单在 `tools/cli/CMakeLists.txt`),
  所以改了之后要重新编译;或者用 `--dev-root <仓库目录>` 让服务直接读仓库里的文件。
- `index.html` / `style.css` / `app.js` 是界面,`renderer.js` 是 WebGL2 渲染,`api.js` 是对本地服务的调用。
- `autotest.*` 和 `uitest.*` 只给 `core/tests/test_preview.py` 用。
- 页面离不开 exe:解码在 exe 里,页面只拿到 Y/U/V/A 平面。

设计决定、接口、测试方法见 [docs/m6-notes.md](../../docs/m6-notes.md),结论见 [docs/m6-conclusion.md](../../docs/m6-conclusion.md)。
