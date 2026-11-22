# 已知问题

[项目首页](../README.md) · [English](#english)

## 关闭锁屏后，冷启动设置时启动画面透明

**复现条件：** 在测试的 LineageOS 23.2 镜像中关闭锁屏，冷启动 Waydroid 后从桌面打开设置，splash 展开时看似透明。只重启桌面也能复现。

**原因：** Quickstep 在收到首次导航栏位置回调前，把默认值 0 当作任务动画的裁剪底边。这是测试镜像的裁剪问题，不是共享纹理丢帧。

**规避方法：** 展开并收起一次通知栏即可恢复。也可以开启滑动锁屏：实测冷启动后上滑解锁，第一次启动设置时动画就正常。项目不修改 Launcher、SystemUI 或加速层，也不在启动时模拟下拉通知栏。

## 明日方舟和 Limbus Company 加载时间较长

在当前 Waydroid 测试环境中，两款游戏均可正常游玩，但加载等待较长。Limbus Company 在从标题页进入主界面、进入战斗时都观察到这一现象；明日方舟暂未记录具体加载阶段和分段耗时。原因尚未确认，目前没有经过验证的规避方法。

## English

[Project overview](../README.en.md)

### Transparent Settings splash after a cold boot with the lock screen disabled

**Reproduction:** On the tested LineageOS 23.2 image, disable the lock screen, cold-boot Waydroid and open Settings from Home. The expanding splash looks transparent. Restarting just the launcher can also reproduce it.

**Cause:** Until Quickstep receives its first navigation-bar position callback, it uses the initial value of zero as the bottom crop edge for the task animation. This is a cropping issue in the tested image, not a dropped shared-texture frame.

**Workaround:** Open and close the notification shade once. Alternatively, enable swipe-to-unlock: in testing, the first launch after a cold boot and unlock displayed correctly. The project does not patch the launcher, SystemUI or the acceleration layer for this issue, or simulate opening the shade at startup.

### Long loading times in Arknights and Limbus Company

Both games are playable in the current Waydroid test environment, but loading takes a long time. In Limbus Company, this was observed both when moving from the title screen to the main menu and when entering battle. For Arknights, the affected loading stage and its duration have not been recorded. The cause is unconfirmed, and there is no verified workaround.
