# M3 笔记

M3 = Android 真机:同一套代码在 Pixel 6a 上正确播放,并记录性能基线(设计文档 §11)。
结论见 [m3-conclusion.md](m3-conclusion.md)(定稿)。本文记录 M3 里做的设计决定、相对设计文档 v0.2 的差异和方法。
M2 的笔记见 [m2-notes.md](m2-notes.md),改 `bindings/unity/` 之前先读它。

## 代码布局(M3 新增和改动)

```
core/CMakeLists.txt                              共享库目标所有平台都叫 alphaclip(Android 上是 libalphaclip.so);if(ANDROID) 加链接选项
core/scripts/build_core_android_arm64.sh         构建后检查 libalphaclip.so(LOAD 对齐、NEEDED、导出符号),生成 libalphaclip.stripped.so
bindings/unity/package/Editor/AlphaClipPluginSettings.cs   也认 libalphaclip.so:设成仅 Android、CPU = ARM64
bindings/unity/package/Runtime/AlphaClipPlayer.cs          UnityWebRequest 加载(OpenUriAsync 等)、Present 耗时计数
bindings/unity/package/package.json              新增依赖 com.unity.modules.unitywebrequest
bindings/unity/TestProject/Assets/Scripts/AlphaClipBenchmark.cs   真机基准(测试代码,包里不带)和 SamplePicture 取样函数
bindings/unity/TestProject/Assets/Editor/AndroidBuild.cs          CreateBenchScene、批处理 Build(IL2CPP / ARM64 / 开发构建)
bindings/unity/scripts/sync_plugin_android_arm64.sh     把 stripped .so 拷进包的 Plugins/Android/arm64-v8a/
bindings/unity/scripts/run_android_build.ps1     在 build/unity-android(工程副本)里构建 APK
bindings/unity/scripts/run_android_bench.ps1     安装 APK、跑基准、取日志和只读设备状态、卸载
bindings/unity/scripts/compare_picture_samples.py        真机取样对 Windows 编辑器取样
```

## 决定与差异(需要你知道)

1. **Unity 版本:仍是 2022.3.62f2。** 它的 Android 模块已装,不需要下载编辑器。M2 留下的"M3 要不要以 6000 为准"这个问题**没有得到回答**
   (问你的对话框被中断了),所以按默认走;6000 的 Android 模块没装,要用得另外下载(会先问你)。Android 上的 Unity 6 没有验证过。
2. **`libalphaclip.so`(arm64-v8a,NDK r30,静态 libc++,API 24 只是冒烟值):**
   - 链接选项 `-Wl,--exclude-libs,ALL` 把 libvpx / libwebm 的符号藏起来,只导出 14 个 `ac_*`;`-z max-page-size=16384` 显式写出 16 KB 对齐
     (NDK r30 默认也是,显式写是为了换 NDK 版本时不悄悄变),`--build-id=sha1`。
   - 构建脚本会检查并打印:所有 LOAD 段对齐 0x4000、NEEDED 只有 libm / libdl / libc、导出符号数 14 且没有非 `ac_` 的名字。
   - 带符号的 .so 是 5,486,792 字节,**strip 后 680,520 字节(0.65 MiB;设计 NFR-3 的目标是核心库 < 2 MB)**,拷进包里的是 `libalphaclip.stripped.so`(包里叫 `libalphaclip.so`)。
     和 DLL 一样**不提交**(构建产物),所以导入设置由代码设置。
3. **插件导入设置:** 用 `PluginImporter.SetPlatformData(BuildTarget.Android, "CPU", "ARM64")`。这个键 Unity 文档没有列出,
   是靠 Unity 自己写出的 `.meta` 反查确认的(`Android: CPU: ARM64`,而且 Unity 写了 `Is16KbAligned: true`,说明它自己也认出了对齐)。升级 Unity 之后这个键要再核对一次。
4. **Android 上读 StreamingAssets:** 它们在 APK 里(`jar:file://...!/assets/...`),不能用文件 API。播放器新增 `OpenUriAsync(uri)` /
   `OpenStreamingAssetsAsync(path)`:用 `UnityWebRequest` 读出字节,再走 `Open(byte[])`;解析出的来源含 `://` 时 `OnEnable` 自动走这条路。
   加载中销毁物体、加载失败(`LoadError`)、不存在的文件都测过。Windows 上用 `file://` 地址走同一段代码(Windows 的 play-mode 测试多了一节 UnityWebRequest 的测试);
   真机上基准程序正是从 APK 里这样读两个片子的,所以这条路径在设备上也实际跑过。**代价:** 整个文件进内存(和 M2 的 `Open(byte[])` 一样),大文件不适合,留给后面决定。
5. **进后台暂停(设计 §9.4):** 还是没做,也没在真机上验证:`OnDisable` 现在只清掉"加载中"标记,依旧靠"队列满了解码线程自己停"。
   (M2 笔记第 7 条写"`OnDisable` 不做事",现在不完全对,已在那里加了更新说明。)
6. **性能计数:** `AlphaClipPlayer.GetPresentTimes()` / `ResetPresentTimes()` 累计 `Present` 里取帧、三个平面上传、绘制提交的**主线程 CPU 耗时**(`Stopwatch`)。
   注意是 **CPU 侧提交时间,不是 GPU 时间**:GPU 侧耗时没有量。这是包里的公开 API;每次 `Present` 因此多几次 `Stopwatch.GetTimestamp`,这点开销**没有单独量过**,
   如果以后在意,可以用条件编译去掉。
7. **基准程序(`AlphaClipBenchmark`):** 每个片子放在 Unity 场景的一个四边形上循环播放,2 秒预热后量 10 秒,每个片子 3 轮(opaque / alpha 交替)。`vSyncCount = 0`、`targetFrameRate = 60`
   (手机默认会限 30 fps,会盖住主线程的真实开销)。字段含义:
   - `decode_ms_per_frame`:核心解码线程自己的耗时,即 `ac_get_stats` 的 `decode_time_us` 增量 / 解出的帧数(**和 M1 的真机表是同一个计数器**,含彩色、alpha、平面拷贝,不含等待)。
   - `acquire_us_per_call`、`upload_ms_per_picture`、`draw_ms_per_picture`、`main_present_ms_per_unity_frame`:上面第 6 条的主线程 CPU 耗时。
   - `frame_ms_*`:Unity 每帧的 `unscaledDeltaTime`,看主线程有没有卡顿。
   - `lag_ms_*`:`CurrentClockUs - LastPtsUs`,每个 Unity 帧一个样本,即画面落后播放器时钟多少。**这个数对 30 fps 的片子天生是 0–33 ms 均匀分布**
     (取"pts ≤ 时钟的最新一帧"),中位数理论值 16.7 ms、95 分位 31.7 ms,实测 16.1–17.0 和 31.3–32.2,正好是这个分布,不是额外的延迟。
   - `first_picture_ms`:从物体 `SetActive(true)` 到第一张画面呈现,**含从 APK 里读文件、打开、解第一帧**。
   - `pictures` / `decoded` / `dropped`:呈现的画面数 / 解出的帧数 / 核心丢弃的帧数(落后时钟被丢掉的)。
   - 内存:`mono_used_mb`、`unity_alloc_mb`、`gfx_driver_mb`(Unity 的精确计数器)、`rss_mb` / `rss_peak_mb`(读 `/proc/self/status`,粗)。
8. **基准是"没固定核"的:** 应用里的线程不能从 `adb shell` 用 `taskset` 随便固定(别的 uid)。所以这是**应用在安卓调度器下实际得到的数**,
   不是 M1 那种固定到某一类核的数。M1 结论写的"M3 基线要固定核类型、多次重复、记录频率和温度"这一条,只做到了多次重复(3 轮)和记录电池温度、
   前后的 CPU 频率快照;**固定核类型没做**(见 m3-conclusion 的"没覆盖到的")。
9. **画面正确性怎么验的:** 基准程序在设备上把每个片子 seek 到 0、0.5、1 秒,`Present` 之后从 `Output` 这张 RenderTexture 读回,在 8×6 个固定位置取样(每处 RGB,共 144 个通道值)和均值,
   写进 logcat;`compare_picture_samples.py` 把它和**同样的取样在 Windows 编辑器里得到的结果**逐通道比。Windows 的画面在 M2 里已经和独立 CPU 参考比过(最大差 1),
   所以这等于验证"Android 的 .so 解码 + 上传 + Mali 上的 YUV→RGB"和 Windows 一致。容限 3,实测最大差 **1**。
   **读回的是 `Output`,不是屏幕:** 四边形、相机、交换链这最后一段在设备上没有读回,也没有人看过手机屏幕。
10. **构建 APK:**
    - `run_android_build.ps1` 先把 `bindings/unity` 拷到 `build/unity-android`(git 忽略),在副本里构建,所以 Android 的导入和玩家设置不会碰仓库里的 `TestProject`。
      批处理入口 `AlphaClip.Tests.AndroidBuild.Build`:IL2CPP、ARM64、minSdk API 24、包名 `com.ghostreel.m3test`、`BuildOptions.Development`(开发构建,APK 带 debuggable 标志;
      这次只通过 logcat 读结果,没有用 `run-as` 之类的方式去碰应用)。
    - **Gradle 要下载**(Unity 2022.3.62f2 自带 Gradle 7.5.1 和 AGP 7.4.2 的模板,仓库是 gradlePluginPortal / google() / mavenCentral(),本机缓存里没有):
      你同意后下载,放进 `E:\Tools\gradle-home`(通过 `GRADLE_USER_HOME`),**实际 277.5 MB、988 个文件**(我事先估的是 100–250 MB,低估了)。
      之前的离线试跑(`-OfflineGuard`:`org.gradle.offline=true` 加死代理)停在"解析 AGP 失败",确认了这是唯一要联网的东西。
    - Unity 构建报告:耗时 343 秒、0 个错误 0 个警告、总大小 267.8 MB(各输出文件按展开后算);APK 文件本身 **20.0 MB**(21,010,279 字节,我事先估的 50–100 MB 偏大)。
11. **APK 里的检查:** `lib/arm64-v8a/` 有 5 个库(`libalphaclip.so`、`libil2cpp.so`、`libunity.so`、`libmain.so`、`lib_burst_generated.so`),**全部 LOAD 段对齐 0x4000**;
    APK 里的 `libalphaclip.so` 与构建输出的 stripped 文件逐字节相同;`zipalign -c -P 16` 通过。
    **注意:** 这个开发 APK 里的 `.so` 是压缩存放的(`stored=False`),安装时被解出来,所以 zip 级别的 16 KB 对齐对它们本来就不起作用,
    "验证成功"这一句在这里意义有限;真正要紧的是 ELF 的 LOAD 对齐(已查)。谷歌商店用的 AAB / 不压缩存放的 `.so`、以及 Play Console 的检查**没有做**(CLAUDE.md 里说要在真机阶段用真实 AAB 验证,留作遗留项)。
12. **踩的小坑:**
    - 通过 PowerShell 的 `adb shell '...| grep ...'` 引号会出问题(报 `Display: inaccessible or not found`):把输出取回来在本地筛选。
    - 设备状态快照里**内核温区读不到**(`/sys/class/thermal` 从 shell 读出来是空的),只有电池温度(`dumpsys battery`)。电池温度开跑前 29.3 °C,跑完 29.2 °C,
      电量 100%(`status: 5` = 充满,手机接着线)。
    - "跑完之后"的 CPU 频率快照是应用退出后的空闲值(小核 1.3–1.8 GHz、中核 0.4 GHz、大核 0.85 GHz),"开始前"的快照是安装刚结束的值(大多在最高频),
      **两者都不代表基准运行期间**。所以这次没有记录到"运行时解码线程在哪类核、什么频率"。
    - Unity 报的刷新率(`Screen.currentResolution.refreshRateRatio`)是 30,而主线程循环以 57.8 fps 跑(`targetFrameRate = 60`、`vSyncCount = 0`);没有深究。
    - 用户自己的 Unity 编辑器(MergeX2 工程)当时开着,构建用的是另一个工程副本,互不影响;构建时间可能因此偏长。
