# Digitalis 转译器补丁：CC1 + 短叶函数

[English](#english)

这两份补丁针对 Digitalis 的 ARM64 → x86_64 Lite 转译器。源码基点是 Android 16 分支的 `frameworks/libs/binary_translation`，提交 `8a2c30cb5383f89f8175eacc06ab1065ab7c5b6f`。补丁按编号应用：先 CC1，再叠加短叶函数补丁。

### 1. [CC1：合并整数比较和条件分支](patches/0001-cc1.patch)

CC1 识别同一 4 KiB 页内的三条连续指令：`CMP Wn, Wm`、向前的条件分支（`EQ/NE/CS/CC`），以及立即数 `SUBS`。若不跳转，紧接着的减法会覆盖 ARM 条件标志（NZCV），因此不必先写回比较产生的标志；若跳转，则在离开当前代码段前保存比较结果。补丁不跳过减法，也不改变其他比较或分支的转译方式。

### 2. [短叶函数：把简单的直接调用合进调用方](patches/0002-short-leaf.patch)

对已经转译过的短函数，Lite 可以把函数体直接编进调用方，省去一次调用和返回经过转译器调度的开销。候选函数必须在同一 4 KiB 页内，由 1–8 条受支持的加载、整数运算或位运算指令组成，并以 `RET X30` 结束。每个转译区域最多合并一次直接调用（`BL`）。

若被调函数的转译缓存入口已变化，或进入函数时有待处理信号，就走常规路径；返回调用方前仍检查信号。这个优化不适用于间接调用或带嵌套调用的函数。

---

<a id="english"></a>

# Digitalis translator patches: CC1 + short leaf functions

These patches target the Digitalis ARM64-to-x86_64 Lite translator in `frameworks/libs/binary_translation`, based on Android 16 commit `8a2c30cb5383f89f8175eacc06ab1065ab7c5b6f`. Apply them in numeric order: CC1 first, then the short-leaf patch.

### 1. [CC1: combine an integer comparison and conditional branch](patches/0001-cc1.patch)

CC1 recognizes three consecutive instructions within one 4 KiB page: `CMP Wn, Wm`, a forward conditional branch (`EQ/NE/CS/CC`), and an immediate `SUBS`. On fall-through, the subtraction overwrites the ARM condition flags (NZCV), so there is no need to write back the comparison flags first. On a taken branch, the translator saves those flags before leaving the region. The subtraction still executes on fall-through; other comparisons and branches retain their usual translation.

### 2. [Short leaf functions: fold a simple direct call into its caller](patches/0002-short-leaf.patch)

For an already translated short function, Lite can emit the body into its caller, avoiding a trip through the dispatcher on call and return. The entire callee must fit in one 4 KiB page and consist of 1–8 supported load, integer arithmetic or bitwise instructions followed by `RET X30`. Each translation region can fold at most one direct call (`BL`).

If the callee’s translation cache entry has changed or a signal is pending on entry, execution takes the normal path. The return boundary still checks for signals. Indirect calls and functions containing nested calls are not eligible.
