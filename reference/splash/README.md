# 关闭锁屏时冷启动后 splash 展开异常

[English](#english)

**归类：当前 LineageOS 23.2 镜像已知问题，加速层不做规避，不修改系统组件。** 实机已复现。已确认是 Quickstep 在首次导航栏 insets 回调前把默认值 0 当作任务动画的裁剪底边。用户反馈：Waydroid 冷启动后从桌面启动设置，splash 透明；展开一次通知栏后恢复。不需要先进入负一屏。

**锁屏对照（用户实测）：** 原复现在关闭锁屏时；同一镜像开启滑动锁屏，Waydroid 冷启动后正常上滑解锁，首次 splash 动画即正常，无需先拉通知栏。此补充描述的是观察到的触发条件，未采集解锁过程的 Binder 跟踪，不据此断定是哪次回调恢复了状态。

本问题按实际的 splash 图层、父层裁剪和导航栏回调归因；SystemUI 进程级的 CPU 回退计数不能说明某一次 splash 使用了历史帧。
## 同一会话的通知栏前后对照

保持 Waydroid、Weston 和系统服务运行。每次先回桌面，强停设置并确认进程退出，再点同一图标，三次最终均进入设置的 NetworkDashboardActivity。A、B 不操作通知栏；B 后展开并收起一次通知栏，再做 C。Windows 侧录 Weston 窗口，960×540、60 fps 恒定帧率，每段 264 帧；视频帧号从 0 起，不等同于 Android 实际提交的帧数。

- [A：首次冷启](boot-shade.li8tzFsW/A.mp4)、[B：再次冷启](boot-shade.li8tzFsW/B.mp4)：展开时只见桌面上一条越来越扁的亮带，到视频帧 121、122 才分别跳到完整的齿轮启动画面。
- [C：通知栏之后](boot-shade.li8tzFsW/C.mp4)：从同一图标开始出现带齿轮的圆角窗口，向全屏扩展。[三次逐帧对照图](boot-shade.li8tzFsW/comparison.jpg)按画面分析区浅色面积首次超过 80% 对齐，而非按点击时间对齐。
- B 和 C 的 SurfaceFlinger 活动图层快照均捕获到 1920×1080 的 splash buffer，展开时图层可见、alpha=1、curr_frame=1。**异常发生在父级 Task 的 transition-leash（任务动画控制图层）的 crop**：B 的首个动画裁剪纵向是 −89 到 90，随后收缩到接近 0，导致子图层在屏幕上的高度从约 34 像素缩成约 1 像素；C 从 417 到 597 开始，向正常内容高度展开。可对照 [B 父层](boot-shade.li8tzFsW/B-leash.csv)、[C 父层](boot-shade.li8tzFsW/C-leash.csv)、[B 子层](boot-shade.li8tzFsW/B-splash.csv)、[C 子层](boot-shade.li8tzFsW/C-splash.csv)。两段的 snapshot_id 各自独立。A 的图层采集未覆盖 splash，仅作为画面对照。
- 这解释了为什么看起来是透明的，也解释了过渡结束后全屏齿轮仍然出现。SystemUI 在三次采样中均有 CPU 回退调用，但不能按进程计数归因到具体窗口。

## 根因：新启动的 Quickstep 没收到导航栏位置初值

在已经恢复正常的桌面，只重新启动桌面进程（未重启容器、Weston 或 SystemUI）。从同一设置图标冷启动，异常再次出现；随后展开、收起通知栏，再次冷启动设置，正常。两次均进入 NetworkDashboardActivity。这将触发条件从“Waydroid 冷启动”缩小为“新桌面进程尚未收到首次显示 insets 更新”。

1. 原 B/C 的 Shell 日志：过渡 #41、#45 的打开任务起止边界均为 (0,0)-(1920,1080)，均由同一类型的远程动画 QuickstepLaunch 执行。新实验中，通知栏操作前 [WM 数据](quickstep-insets/wm-before.txt)已经报告导航栏顶边 y=1017，不是显示尺寸或系统栏布局尚未算好。
2. 在机 Launcher3QuickStep.apk SHA-256 为 d97259c2fcdcd16e9d9ccc12ba9a3b4ec35302b066f131e1f8f203e88f6395a6。HomeVisibilityState.navbarInsetPosition 为默认 0 的 int；首次有效赋值来自 IHomeTransitionListener.onDisplayInsetsChanged，用显示底边减去底部 navigationBars inset。QuickstepTransitionManager.getOpeningWindowAnimators 在应用使用常驻 taskbar、不准备显示 IME 的裁剪分支用 min(缓存位置, 任务底边)；初始和中途更新两处都用这个值。它是屏幕坐标，不是栏高。
3. 在机 SystemUI.apk SHA-256 为 a1f8216de2516df004eadfadddc2a33b92a22193cb19ccde893995e2cf08fdf4。IShellTransitions 注册 home listener 的实现会回送已知的 home 可见状态，但不同时回送当前 InsetsState；之后 HomeTransitionObserver 收到实际的 insetsChanged 才转发。与[本次监听器 Binder 摘录](quickstep-insets/listener-events.txt)一致：注册后、首次设置启动及返回期间只有 code 1（home 可见性），没有 code 2（display insets）；展开/收起期间第一次出现 code 2，共两次。节点由注册事务携带的 Binder 引用对应；私有跟踪实例写入/留存均为 10351 条。
4. [重启桌面后的父层 crop](quickstep-insets/before-crop.csv)末段为 (0,0)-(1919,0)；[首次回调后的 crop](quickstep-insets/after-crop.csv)末段为 (0,0)-(1919,1016)，后者与 WM 导航栏顶边 1017 的距离是一个像素（采样为过渡帧）。未读取 Java 堆字段，也未采集 Binder Parcel 内容；字段初值和计算由对应 APK 确认，运行时直接观测的是回调时序和实际裁剪坐标。

**适用范围。** 此结论针对所测镜像和关闭锁屏的启动流程。项目不修改 Quickstep 或 SystemUI，不在启动脚本模拟下拉，也不硬编码导航栏坐标。

---

<a id="english"></a>

# Splash expansion after a cold start with the lock screen disabled

**Known issue in the tested LineageOS 23.2 image. The acceleration layer does not work around it, and no system components are modified.** Reproduced on the test system: before its first navigation-bar insets callback, Quickstep uses a default value of 0 as the bottom crop coordinate of the task animation. With the lock screen disabled, the user saw a transparent splash when launching Settings from Home after a cold start. Opening the notification shade once restored the animation. Visiting the page to the left of Home was not necessary.

**Lock-screen comparison (tested by the user):** On the same image, enabling the swipe lock screen and swiping up to unlock after a Waydroid cold start produced a normal first splash without opening the shade. This establishes a trigger condition, not which callback restored the state: no Binder trace was collected during unlock.

The diagnosis below uses the splash layer, its parent crop and the navigation-bar callback. Process-wide SystemUI CPU fallback counts cannot establish whether a particular splash used an old frame.
## Before and after opening the shade in one session

Waydroid, Weston and system services stayed running. For each run, we returned Home, force-stopped Settings, confirmed its process had exited, and tapped the same icon. All three runs reached NetworkDashboardActivity. Runs A and B did not touch the shade; between B and C we opened and closed it once. The Windows recording captured the Weston window at 960×540, a constant 60 fps, for 264 frames per clip. Video frames are numbered from 0; they do not count actual Android frame submissions.

- [A: first cold launch](boot-shade.li8tzFsW/A.mp4) and [B: second cold launch](boot-shade.li8tzFsW/B.mp4): only a progressively thinner bright strip appeared over Home during expansion. The full-screen gear splash appeared abruptly at video frames 121 and 122 respectively.
- [C: after opening the shade](boot-shade.li8tzFsW/C.mp4): a rounded window containing the gear appeared at the same icon and expanded to full screen. The [Three frame sequences aligned](boot-shade.li8tzFsW/comparison.jpg) aligns the runs at the first frame where light pixels cover more than 80% of the analysis region, not at the tap.
- The SurfaceFlinger snapshots in B and C both captured a visible 1920×1080 splash buffer with alpha=1 and curr_frame=1 during expansion. **The difference was the crop on the parent task's transition leash**, the layer that controls the animation. In B the first vertical crop ran from −89 to 90 and then shrank toward 0; the child layer's on-screen height fell from about 34 pixels to about 1. In C the crop started at 417 to 597 and expanded toward the normal content height. Compare [B parent layer](boot-shade.li8tzFsW/B-leash.csv), [C parent layer](boot-shade.li8tzFsW/C-leash.csv), [B child layer](boot-shade.li8tzFsW/B-splash.csv) and [C child layer](boot-shade.li8tzFsW/C-splash.csv). The two runs have independent snapshot_id values. A's layer capture missed the splash and is used only for visual comparison.
- This explains both the apparent transparency and the full-screen gear at the end of the transition. SystemUI made CPU fallback calls in all three runs, but a process-wide count does not identify the window responsible.

## Root cause: a newly started Quickstep has no initial navigation-bar position

After the animation had recovered, restarting just the launcher process (not the container, Weston or SystemUI) brought the issue back on a cold launch of Settings. Opening and closing the shade again restored the next cold launch. Both launches reached NetworkDashboardActivity. This narrows the trigger from a Waydroid cold start to a new launcher process that has not yet received its first display-insets update.

1. In the original B/C Shell logs, transitions #41 and #45 both opened tasks with start and end bounds (0,0)-(1920,1080) using the same type of remote animation, QuickstepLaunch. In the new experiment, [WM data](quickstep-insets/wm-before.txt) already reported the top of the navigation bar at y=1017 before the shade was touched. Display dimensions and system-bar layout were therefore already available.
2. The installed Launcher3QuickStep.apk had SHA-256 d97259c2fcdcd16e9d9ccc12ba9a3b4ec35302b066f131e1f8f203e88f6395a6. HomeVisibilityState.navbarInsetPosition is an int that defaults to 0. Its first valid assignment comes from IHomeTransitionListener.onDisplayInsetsChanged: the display bottom minus the bottom navigationBars inset. When an app uses the persistent taskbar and the IME is not about to appear, the crop branch of QuickstepTransitionManager.getOpeningWindowAnimators uses min(cached position, task bottom), both initially and during updates. This is a screen coordinate, not a bar height.
3. The installed SystemUI.apk had SHA-256 a1f8216de2516df004eadfadddc2a33b92a22193cb19ccde893995e2cf08fdf4. When IShellTransitions registers a home listener, it sends back the known home visibility but not the current InsetsState. HomeTransitionObserver forwards a later insetsChanged event. This agrees with [Binder excerpt for this listener](quickstep-insets/listener-events.txt): after registration, the first Settings launch and the return Home produced only code 1 (home visibility), not code 2 (display insets). Code 2 first appeared while opening and closing the shade, twice in total. The node was matched via the Binder reference in the registration transaction; all 10351 entries written to the private trace instance were retained.
4. The final sampled crop in [Parent crop after launcher restart](quickstep-insets/before-crop.csv) was (0,0)-(1919,0); in [Crop after first callback](quickstep-insets/after-crop.csv) it was (0,0)-(1919,1016). The latter is one pixel from the WM navigation-bar top at 1017 on the sampled transition frame. We did not read Java heap fields or capture Binder Parcel contents. The APKs establish the field default and calculation; callback timing and crop coordinates were observed at runtime.

**Scope:** This finding applies to the tested image and the start sequence with the lock screen disabled. The project does not patch Quickstep or SystemUI, simulate a shade gesture at startup, or hard-code navigation-bar coordinates.
