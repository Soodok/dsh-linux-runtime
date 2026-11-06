# 踩坑记录

按发现顺序。每条都附「症状 → 原因 → 修法」，因为这些坑的共同特征是
**症状和原因不在同一层**，光看报错会往完全错误的方向查。

## 1. 静态链接报 TLS underaligned

**症状**：`ld.lld: error: executable's TLS segment is underaligned: alignment is 8,
needs to be at least 64 for ARM64 Bionic`，exit 134。

**原因**：NDK r26b 的 bionic 链接器缺陷。

**修法**：换 NDK r28+。

## 2. `Bad system call`（RC=159）且无任何输出

**症状**：程序启动即死，`echo $?` 得 159，stderr 空。

**原因**：`seccomp_data.instruction_pointer` 在 aarch64 上报的是 svc 指令
**地址 +4**。白名单比对 `&svc_insn` 永远不命中 → 自己的裸 svc 也被 TRAP →
handler 返回重试 → 无限循环。

**修法**：`g_stub_ip = &svc_insn + 4`。

## 3. handler 收到的 `dirfd` 是 `0xFFFFFF9C` 而不是 `-100`

**原因**：bionic 的 `syscall()` 把 int 参数**零扩展**成 64 位，
`-100` 变成 `0x00000000FFFFFF9C`，与 `(long)AT_FDCWD`（`0xFFFF...FF9C`）不等。

**修法**：`long a0 = (long)(int)(unsigned int)r[0];` 先做符号扩展再比较。

## 4. `execve` 之后 handler 失效

**症状**：第一个程序能跑，之后所有 exec 出来的程序全部 `Bad system call`。

**原因**：过滤器跨 `execve` 存活，但 **SIGSYS 处置被内核重置为默认**。

**修法**：新镜像的 `_start` 必须重装 handler（这就是自我重执行的动机）。

## 5. `cat` / `ls` / `id` 段错误，但 `echo` / `uname` 正常

**原因**：handler 里调了 bionic libc。guest 的 musl 启动后改写 `TPIDR_EL0` 接管
TLS，此后 handler 里任何 `snprintf` / `memcpy` / `malloc` 都会 SEGV。

**修法**：handler 全程裸 svc + 手写字节循环。这是本项目最贵的一课 ——
第一版花了很久才意识到「崩在 handler 里」而不是「rootfs 缺文件」。

## 6. fork 出的子进程立刻被杀

**症状**：status=31（SIGSYS 的默认处置是 kill）。

**原因**：fork 发生在 handler 内，子进程继承了「SIGSYS 被阻塞」的信号掩码；
seccomp TRAP 遇到被阻塞的 SIGSYS 直接 kill，不走 handler。

**修法**：子进程第一件事 `rt_sigprocmask(SIG_UNBLOCK, SIGSYS)`。

## 7. `tar -czf` 挂死 >120 秒

**症状**：`gcc -c` 正常，`tar -czf` 死等，CPU 空闲。

**原因**：vfork 死锁。musl 的 `vfork.s` 用
`clone(CLONE_VM|CLONE_VFORK, child_stack=0)`，父进程挂起直到子进程 exec。
BusyBox 的 `vfork_compressor()` 就是这样起 gzip 的。而我们的 handler 拦到
execve 后自己 fork 子进程并 wait4 —— 于是：
`tar(P) 被 vfork 挂起 → handler fork B → B(gzip) 读管道等 P 写 → P 未从 vfork 返回`。
且 A 在 handler 里 mmap 会破坏 P 的共享 mm。

**修法**：BPF 只拦「flags 含 `CLONE_VFORK` **且** `child_stack(x1)==0`」的
clone（排除 posix_spawn 的自定义栈），handler 里清掉 `CLONE_VM|CLONE_VFORK`
两位并把 `pc -= 4` 回退重试，让它退化成普通 fork。

## 8. execve 失败后 PATH 搜索链断裂

**症状**：`tar -czf` 写 0 字节且无报错。

**原因**：`execlp()` 靠 execve 返回 `ENOENT` 判断「这个目录没有，试下一个」。
我早期在 loader 预检失败时直接 `exit_group(127)`，把搜索链打断了。

**修法**：失败必须 `r[0] = -ENOENT; return;`，把 errno 还给 guest。

## 9. rootfs 里 335 个符号链接全部失效

**症状**：`sh: cat: not found`，但 `/bin/busybox` 明明存在。

**原因**：Alpine 的 applet 链接是**绝对路径**（`/bin/cat -> /bin/busybox`），
内核按宿主真实根解析 → ENOENT。

**修法**：部署时重写为相对路径（`fixlinks`，实测 335 个里 306 个需要改）。

## 10. `gcc -o` 报 `undefined reference to 'main'`

**症状**：手工 `ld` 完全正常，走 `gcc` 就缺 `main` / `__libc_start_main`。

**原因**：collect2 传给 ld 的参数有 40 多个，`.o` 排在 34 位左右，
而 loader 的 `g_argv_store` 只有 32 项、复制循环上限 30 → `.o` 被截断丢弃。

**修法**：容量改 256，循环上限 250。这个坑排查了很久，因为「手工能过、
gcc 不过」很容易怀疑到链接器参数上。

## 11. `configure` 脚本探 `as` 失败

**原因**：clang 的依赖闭包**不含 binutils**，装完 clang 后 `as` / `ld.bfd` / `ar`
全缺。llvm 包提供 `llvm-ar` / `llvm-objcopy` 但**不提供 `as`**。

**修法**：扩展里显式带 `binutils`，并在 `usr/bin` 补符号链接
（Alpine 把 binutils 放在 `usr/aarch64-alpine-linux-musl/bin/`）。

## 12. `collect2: fatal error: cannot find 'ld'`

**原因**：同上，`ld` 不在 `usr/bin`。另有一次真因是
`libgcc_s.so.1` 缺失 —— dl 脚本误判「已存在」跳过了 `libgcc` 包。

## 13. Alpine `cc1`（34MB）装载失败

**原因**：loader 用固定 6MB 预读缓冲。

**修法**：`read_into` 改成按 `st_size` raw `mmap`。

## 14. `cd /tmp` 报 `Bad address`

**原因**：`chdir` 只有 1 个参数，我误写成 `raw6(SYS_chdir, g_rootfs_fd, ...)`
把 fd 当成了路径指针。

**修法**：`openat(O_PATH|O_DIRECTORY)` 拿 fd，再 `fchdir`。

## 15. `statvfs` 查的是宿主根

**症状**：`apt` 报 `Couldn't determine free space`。

**原因**：`statfs` 没有 `*at` 变体，不翻译就查宿主。

**修法**：`TRAP_PAIR(SYS_statfs)` + `make_host()` 拼宿主路径。

## 16. `getcwd` 返回宿主路径

**症状**：cmake 配置全错（相对路径构造基于宿主绝对路径）。

**修法**：trap `getcwd`，用 `g_rootfs_host` 裁剪前缀。
注意必须在 rootfs 前缀裁剪**之前**做 bind 反向映射。

## 17. 相对路径 execve 失效

**症状**：`./capp` 跑不了，绝对路径可以。

**原因**：`loader_preload` 只处理绝对路径。

**修法**：不以 `/` 开头时先 `getcwd` 取真实 cwd（再裁剪 rootfs 前缀），
拼成绝对路径再处理。

## 18. shebang 脚本（`npm` / `pip`）退出 125

**原因**：preload 按 ELF 头读，文本脚本读出乱码。

**修法**：检测 `#!`，解析解释器 + 可选参数（`#!/usr/bin/env node` 的 `node`），
argv 组装成 `[interp, arg, script, 原argv[1..]]`。

## 19. `posix_spawn` 段错误（原地替换方案）

**原因**：`posix_spawn` 内部是 `clone(CLONE_VM|CLONE_VFORK, child_stack≠0)`，
子进程与父进程**共享 mm**。在 handler 里原地覆盖映像 = 破坏父进程 mm。
gcc / pip / npm 全依赖 posix_spawn。

**修法**：不能用原地替换，必须让**内核** exec（自我重执行）。

## 20. 环境变量丢失 / PATH 被覆盖

三层坑，缺一不可：

1. `loader_preload` 早期无条件写死 `PATH/HOME/TERM`、完全丢弃 `envp`
2. 自我重执行的 `execveat(..., envp=0, ...)` **每次重执行都清空环境**
3. 全放开继承后宿主路径变量污染容器（`TMPDIR=/data/local/tmp` 让 make
   报 `TMPDIR value ...: No such file or directory`）

**修法**：过滤宿主专属变量（`PREFIX` / `LD_*` / `TMPDIR` / `SHELL` / `ANDROID_*`
等），再补容器默认值；PATH 单独处理 —— **仅首次启动**替换成容器默认，
之后容器内 `export PATH=...` 必须原样保留（否则 ccache 永远 miss）。

## 21. `subprocess` / `posix_spawn` 后所有文件 ENOENT

**原因**：它们默认 `close_fds=True`，把我持有的 rootfs fd 关掉了，
之后所有路径翻译打到无效 fd。

**修法**：handler 入口 `readlinkat(fd, ".")` 探活，`-EBADF` 就重开。
（优化：改成「翻译失败才探活」，省掉每次 syscall 的一次 readlinkat。）

## 22. `nginx` / `postgres` 自己把 SIGSYS 改回 SIG_DFL

**症状**：exit 159，handler 再也不进。

**原因**：这些程序启动时会 `sigaction(SIGSYS, SIG_DFL)`，之后第一个被 TRAP 的
syscall 直接按默认动作杀进程。

**修法**：拦 `rt_sigaction`，`signo == SIGSYS` 时**无条件返回成功但不生效**。
注意**不能在 handler 里解引用 `act` 指针**判断 DFL/IGN —— 那是 guest 结构体，
在 handler 里读会崩（实测 node 崩在这条上）。

## 23. 同一路径两种视图

**症状**：`-m /tmp:/host` 之后 `cd /tmp` 进 rootfs 的 `/tmp`，
但 `cat /tmp/x` 走 bind。

**修法**：`chdir` 与 `openat` 用**同一套** bind_map 优先级。

## 24. 双路径 syscall 缓冲复用（rename 退化成 self-rename）

**原因**：`renameat` / `linkat` / `symlinkat` 有两个路径参数，共用一块缓冲时
第二次翻译会覆盖第一次。

**修法**：两个独立缓冲 `oh[PATH_MAX]` / `nh[PATH_MAX]`。

## 25. Android 冻结 App 导致「代码没生效」

**症状**：DNS 代理线程活着、`ss -ulnp` 显示 Recv-Q 持续上涨，但没人读。

**原因**：屏幕熄灭后 Android cgroup freezer 把整个 App 冻结，
所有线程 `wchan = do_freezer_trap`。

**修法**：这不是代码问题。测试前 `input keyevent 224` 唤醒并
`svc power stayon true`。这条值得单列 —— 它让前面几轮排查全部白费。

## 26. 宿主 PATH 在重执行后泄漏

**症状**：容器内 shell 内建命令（`echo` / `cd` / `export`）正常，
但 `cat` / `head` / `gzip` 全部 `not found`，看起来像 rootfs 缺 applet。

**原因**：自我重执行会重新初始化 `.data` 段，`g_force_container_path`
变回 0。于是从宿主继承来的
`/product/bin:/apex/com.android.runtime/bin:/system/bin:...` 被当成
「用户在容器里自己设的 PATH」原样保留。

**修法**：新增 `path_looks_like_android()`，按前缀识别
`/system/bin`、`/apex/`、`/odm/bin`、`/vendor/bin`、`/data/adb/` 等
（这些绝不会出现在 Linux 发行版里），两处 PATH 分支都加判断。

## 27. 移植到新架构后 `Bad system call`（RC=159），且 `strace` 只报 SIGSEGV

**症状**（在 x86_64 上移植本方案时实测，Android 14 模拟器）：

- 程序 rc=159 `Bad system call`，strace 里却是
  `--- SIGSEGV {si_signo=SIGSEGV, si_code=SI_KERNEL, si_addr=NULL} ---`
  —— 看起来像「handler 返回时跳到 NULL」。
- 极简 demo 只装 `SECCOMP_RET_TRAP` + handler 也复现；
  更怪的是**连 handler 之前的第一条 `write` 都没输出**（strace 只有
  `execve(...) = 0` 和一行 SIGSEGV），极易误判成 `_start` / restorer /
  `sigaltstack` 布局有问题。
- 而不装 filter 时，`rt_sigaction` / `raw6` / `_start` 全部正常。

**原因**：`install_filter()` 的 cBPF 里那条 **IP 高 32 位白名单**被漏掉了。
原实现是：

```c
#define LOADER_BASE_HI 0x20u
BPF_STMT(BPF_LD|BPF_W|BPF_ABS, offsetof(struct seccomp_data, instruction_pointer) + 4),
BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, LOADER_BASE_HI, <->ALLOW>, 0),
```

它的作用是：**凡是从 loader 自己代码里发出的 syscall，一律 ALLOW，先进 BPF 树**。
handler 内部的「自愈」调用（`sigsys()` 里的
`raw6(SYS_rt_sigprocmask, SIG_UNBLOCK, &set, ...)`、`child_run_raw()` 跳转前的
解除阻塞、fork 子进程里的解除阻塞，共三处）正是靠这条放行的。

漏掉它 → handler 里那条 `rt_sigprocmask` 被自己的 filter 再 TRAP 一次 →
**嵌套 SIGSYS**。SIGSYS 在 handler 执行期间默认被内核阻塞，没有 handler 可投递，
内核就按致命错误处理，aarch64 报 `Bad system call`、x86_64 报
`SIGSEGV(SI_KERNEL)`。这就是「看起来在 handler 之前就死了」的来源——
其实早已进了 handler，死在 handler 内部那行。

**修法**：移植到任何新架构，cBPF 头部这 4 条**一条都不能少**：

```c
0  BPF_LD   seccomp_data.arch
1  BPF_JEQ  AUDIT_ARCH_<target>  -> 2 : -> KILL_PROCESS
2  BPF_LD   seccomp_data.instruction_pointer + 4    // 高 32 位（小端低地址是低位）
3  BPF_JEQ  LOADER_BASE_HI        -> ALLOW : -> 4    // ★别漏
4  BPF_LD   seccomp_data.nr
...按 nr 分发...
```

**验证方法**（不依赖真机，模拟器上就能跑）：写一个自包含的 demo，
BPF 只拦 `rt_sigprocmask`，handler 里再调一次 `rt_sigprocmask`：

- 带 IP 白名单 → `HANDLER-SURVIVED-nested`、rc=0；
- 去掉 IP 白名单 → `Bad system call`、rc=159。

再把「guest 桩」`mmap` 到非 loader 基址（如 `0x5000000000`）执行，
确认它**仍被 TRAP**（证明白名单只放行 loader、没把 guest 一起放行）——
两者同时成立才算修对。

**教训**：`strace` 对**嵌套** SIGSYS 是无能为力的，它只会显示外层那一层，
于是现场看起来比真实位置早得多。定位这类问题不要靠「在哪一行停下」，
要靠在 handler 里逐步加 `write` 打点（本方案 freestanding，`raw6(1,...)` 即可）。
另外「最小复现」必须把 IP 白名单也带上，否则复现的是另一个问题。

## 28. x86_64 上容器起得来、但 `ls /` 看到的是 Android 宿主的根目录

**症状**：容器进程正常启动、`busybox echo` 正常、`sh -c` 正常，但
`ls /etc` 列出的是 Android 的 `/etc`（`NOTICE.xml.gz`、`audio_effects.xml`…），
`cat /tmp/x` 报 `No such file or directory`。没有任何报错，纯粹「翻译没生效」。

**原因**：x86_64 **保留了** aarch64 通用 ABI 里根本没有的「老式」路径 syscall
（`open`=2、`stat`=4、`lstat`=6、`access`=21、`unlink`=87、`mkdir`=83、
`rename`=82、`chmod`=90…）。原实现只拦 `*at` 变体（`openat`/`newfstatat`/
`faccessat`…），在 aarch64 上足够，在 x86_64 上则完全漏掉。

strace 实证（容器内 `ls /etc`）：

```
stat("/etc", {st_mode=S_IFDIR|0755, st_size=4096, ...}) = 0
open("/etc", O_RDONLY|O_LARGEFILE|O_CLOEXEC|O_DIRECTORY) = 5
```

两条都不在过滤器里 → 不 TRAP → 内核按原样执行 → 落到宿主根。

**修法**：给 x86_64 的 BPF 树补上 15 个老式 syscall，并在 handler 里把它们
**改写成对应的 `*at` 版本**（这样路径翻译、bind 映射、EBADF 重试全部复用）：

| 老式 | 改写成 |
|---|---|
| `open(p,f,m)` | `openat(AT_FDCWD,p,f,m)` |
| `stat(p,buf)` | `newfstatat(AT_FDCWD,p,buf,0)` |
| `lstat(p,buf)` | `newfstatat(AT_FDCWD,p,buf,AT_SYMLINK_NOFOLLOW)` |
| `access(p,m)` | `faccessat(AT_FDCWD,p,m,0)` |
| `unlink(p)` | `unlinkat(AT_FDCWD,p,0)` |
| `rmdir(p)` | `unlinkat(AT_FDCWD,p,AT_REMOVEDIR)` |
| `mkdir(p,m)` | `mkdirat(AT_FDCWD,p,m)` |
| `rename(o,n)` | `renameat(AT_FDCWD,o,AT_FDCWD,n)` |
| `link(o,n)` | `linkat(AT_FDCWD,o,AT_FDCWD,n,0)` |
| `symlink(t,l)` | `symlinkat(t,AT_FDCWD,l)` |
| `chmod(p,m)` | `fchmodat(AT_FDCWD,p,m,0)` |
| `chown(p,u,g)` | `fchownat(AT_FDCWD,p,u,g,0)` |
| `truncate(p,l)` | `openat` + `ftruncate` |
| `utime(p,t)` | `utimensat(AT_FDCWD,p,t,0)` |
| `readlink(p,b,s)` | `readlinkat(AT_FDCWD,p,b,s)` |

**为什么容易漏**：树是合法的、`seccomp()` 返回 0、编译通过、容器能起、
`echo` 能跑 —— 只有真的去访问一个**绝对路径**才看得出问题。而且
`tools/verify_bpf_semantics.py` 若也用 aarch64 的编号表，同样查不出来
（它只会验证表内那些 syscall 的语义，不会问「宿主架构还多了哪些」）。
所以这个脚本现在改成**按架构从 NDK 头文件取编号**，并另加一条
「拿另一架构的 openat 编号来试，必须是 ALLOW」的交叉断言。

## 29. x86_64 上 handler 一返回就 `SIGSEGV(SI_KERNEL, si_addr=NULL)`

**症状**：装了 filter、SIGSYS 也确实投递到了 handler，但 handler 执行完
准备返回时立刻 `--- SIGSEGV {si_code=SI_KERNEL, si_addr=NULL} ---`，
进程被 SIGSEGV 杀掉。strace 里看着就像「handler 内部解引用了空指针」。

**这个症状极具误导性**，我为此先后错改了四处（都不对）：

1. 以为 `_start` 的段属性有问题（`.text` vs `.ltext`）；
2. 以为 `sigaltstack` 的 `stack_t` 布局在 x86_64 上不同；
3. 以为 `-mcmodel=large` 下裸汇编的重定位不对；
4. 以为返回值写错了寄存器（这一条**确实是另一个真 bug**，见下，但
   不是 `si_addr=NULL` 的原因）。

**真正原因**：x86_64 的 `struct sigaction` 里 `sa_restorer` **必须**填有效
地址，并且 `sa_flags` 要带 `SA_RESTORER`（0x04000000）。内核从这个地址
恢复被信号打断的上下文；不给（=NULL）时 handler 一返回就跳到地址 0。
aarch64 的通用 ABI 有 VDSO 版 restorer，所以原实现把 `sa+16` 留 0 也没事 ——
换成 x86_64 就致命。

**修法**：自己写一段裸汇编当 restorer，并把它填进 `sa+16`：

```c
__asm__(
    ".text\n.p2align 4\n.globl __dsh_rt_sigreturn\n.hidden __dsh_rt_sigreturn\n"
    ".type __dsh_rt_sigreturn,@function\n"
    "__dsh_rt_sigreturn:\n  movq $15, %rax\n  syscall\n"     /* __NR_rt_sigreturn */
    ".size __dsh_rt_sigreturn, .-__dsh_rt_sigreturn\n");
/* ... */
sa_flags = SA_SIGINFO | SA_NODEFER | SA_RESTORER;
*(unsigned long *)(sa + 16) = (unsigned long)__dsh_rt_sigreturn;
```

**同一架构下的第二个必改点：返回值寄存器**

aarch64 的 `x0` 既是第一个入参又是返回值，所以「把翻译结果写回 `r[0]`」
两边都成立。x86_64 **入参在 `rdi`、返回在 `rax`**，只写回 `rdi` 的话
guest 拿到的「返回值」其实是它自己的第一个参数，真正 `rax` 仍是 syscall 号。
实测症状（容器内 `ls /etc`）：

```
stat("/etc", {st_mode=001, st_size=0, ...}) = 4
```

`st_mode=001`、返回值 `4` —— 全是垃圾。修法是 handler 末尾**先把入参
写回 `rdi..r9`，再把 `r[0]` 写进 `rax`**。

**这两条必须都改对才能跑**，只改一条仍是 SIGSEGV，很容易误以为「改了没用」。
## 30. 软链接内容里的绝对路径不经过翻译（架构固有限制，非 bug）

**症状**：容器内 `ln -s /etc/alpine-release /tmp/abs` 成功、`ls -l` 显示链接正确，
但 `cat /tmp/abs` 报 `No such file or directory`。相对软链（`ln -s ../etc/x /tmp/rel`）
则完全正常。

**原因**：内核在处理路径的中间组件是软链接时，会**自己在内核态展开**链接内容，
这发生在地址空间里、不产生新的 syscall，我们的 seccomp 过滤器根本收不到。
于是链接内容里的 `/etc/alpine-release` 被内核按**宿主根**解析。
这不是实现缺陷 —— 只要不用 chroot/namespace，任何纯 syscall 拦截方案
（PRoot 同理）都有这个边界。

**可行的规避**（按侵入性排序）：

1. **优先用相对软链**。rootfs 内的系统级软链大多可以改写成相对形式，
   这是零成本的正解。
2. **对 rootfs 里必须绝对的地方做「实体化」**：把软链替换成真实文件或
   硬链接（本项目实测 `/bin/sh` 就是 busybox 的实体副本，不是软链）。
3. 若某个绝对软链非有不可，可以把它重写成「相对 cwd 的等价路径」——
   前提是 cwd 可预期，一般不推荐。

**诊断提示**：这类问题**不会**在 strace 里留下任何被拦记录（因为压根没有
syscall）。看到「链接建成功、读不到」且 strace 里 `cat` 的 openat 参数是
那个链接名本身，就该想到内核自己展开了。

## 31. 宿主进程自带 seccomp filter 时，容器整体跑不起来（模拟器上才暴露）

**症状**：从 App 进程 `ProcessBuilder` 拉起的容器，任何命令都立刻死，
`rc=139`（SIGSEGV）或 `rc=159`（SIGSYS），**stdout/stderr 全空**、耗时只有
100~200ms（而正常运行 `apk update` 要 1~2 秒）。但同一条命令在 `adb shell`
里、`run-as app.dsh.mobile` 里、手工 `env -i` 里**全部 rc=0 成功**。
极易误判成「文件权限 / SELinux / 环境变量 / APK 里 runner 是旧版」。

**根因**：Android 的 zygote 在 fork 出 App 进程**之后**、跑 App 代码之前，
会由 libseccomp 装上 app policy filter。实测：

| 进程 | `Seccomp:` | `Seccomp_filters:` |
|---|---|---|
| `zygote64` 本体 | 0 | 0 |
| App 进程（zygote fork 出来的） | **2** | **1** |
| `adb shell` / `run-as` 起的进程 | 0 | 0 |

seccomp 是**栈式叠加**的：同一 syscall 必须所有 filter 都返回 ALLOW 才放行，
否则取最严格的 action（`KILL` > `TRAP` > `ERRNO` > `ALLOW`）。于是外层
zygote 的 `KILL` 会先于我们自己的 `TRAP` 生效 —— 我们的 SIGSYS handler
根本没机会被调用，进程直接被内核杀掉。

而加载器的 `PR_GET_SECCOMP` 判据是「为 2 就认为自己的 filter 已装好」（这是
为了跨 `execveat` 自我重执行时不重复叠加），在外层已有 filter 的场景下
**判断错位**：既没装自己的 filter（被跳过），又继承了外层的 KILL 策略。

**为什么真机不复现**：真机（OPPO ColorOS / 一加）的 app seccomp policy 或
内核配置允许了我们需要的 syscall 集合，进程能活。这是**设备/ROM 相关**的
差异，不是代码缺陷。

**诊断手法（本次靠它定位）**：
1. 在调用点跑两个探针，把「容器整体起不来」和「只有某个程序崩」区分开：
   - `runner.run("")`（不跑 guest）→ 拿到 `rc=5` + `LOADER: preload FAIL`
     说明 **loader 本体能 exec、能打印**，崩溃在装载 guest 之后；
   - `runner.run("/bin/sh", ["-c","echo X"])` → `rc=139`。
2. 用 `DSH_ALPINE_NO_FILTER=1` 跳过 `install_filter()` 再做一次对照：
   - 跳过自己的 filter → `rc=159`（SIGSYS，说明外层 filter 在杀）；
   - 装自己的 filter → `rc=139`（说明自己的 handler 内部又炸）。
   两种都失败，就证明**不是我们的 filter 逻辑问题，而是外层 filter 在拦**。
3. 对比 `grep -E "^Seccomp" /proc/<pid>/status`：App 进程 vs `run-as` 进程。

**可行的绕过方向**（未实现，留作后续）：
- App 侧把容器启动放到一个 `Seccomp: 0` 的子进程里 —— 但 zygote fork 的
  子进程必然继承 filter，只能靠 `fork` 后 `exec` 一个**不受 app policy
  约束**的中间程序（受 SELinux `execute_no_trans` 限制，未验证可行性）。
- 扩充我们自己的 BPF 白名单，让所有被外层 policy 覆盖到的 syscall 也能通过
  —— 但外层是 `KILL`，我们的 `ALLOW` 压不过它，此路不通。
- 真机交付不受影响；模拟器上验证容器行为只能走 `run-as`（`Seccomp: 0`）。

**结论**：在模拟器上做容器功能验证时，必须用 `run-as app.dsh.mobile <runner>
<rootfs> <interp> <guest> ...` 而不是走 App UI —— 后者会因为 zygote filter
而必然失败。这与容器实现本身无关。

## 32. X11 / Wayland 显示服务器在免 root 下起不来 —— 但我们的路线根本不需要它

想在容器里跑 Xvfb 或 Wayland compositor 时，会在最开头就撞墙：

```
(EE) Fatal server error:
(EE) Linking lock file (/tmp/.X99-lock) in place failed: Permission denied
```

`Xvfb -nolock` 也救不了：它**只允许 root 使用**（`Warning: the -nolock option
can only be used by root`）。直接 `ln` 验证：

```
$ touch /tmp/q1 && ln /tmp/q1 /tmp/q2
ln: /tmp/q2: Permission denied
```

**根因不在我们的容器**，而在 Android 平台：SELinux 对 `app_data_file` 完全
禁止 `link` 权限。宿主侧用同一 uid 做同样操作一样失败：

```
$ run-as <pkg> sh -c 'cd /data/data/<pkg>/files && touch _h1 && ln _h1 _h2'
ln: cannot create hard link from '_h1' to '_h2': Permission denied

avc: denied { link } ... scontext=u:r:runas_app:s0 tcontext=u:object_r:app_data_file:s0
     tclass=file permissive=0
```

而 X 服务器建 `.X<n>-lock` 必须用「写临时文件再硬链接到目标」的方式（保证
原子性），所以必然失败。Wayland compositor 的 unix socket 也需要
`/tmp/.X11-unix` 这类目录与锁机制，同样受阻。

实测：**以 root 身份能起**（`-nolock` 生效，`xsetroot` 成功、`xwd -root` 拿到
1231979 字节），**免 root 必然失败**。

### 对我们的意义（这才是重点）

我们**不需要任何显示服务器**。当前图形路线是：

```
EGL (pbuffer / surfaceless) → GLES 渲染 → glReadPixels → 共享文件 → App SurfaceView
```

全程零 X11、零 Wayland、零 compositor、零 unix socket。这既是限制也是优势：
- 限制：拿不到「系统级窗口」，别的 X11 程序想直接显示得先有 X server。
- 优势：**免 root 就能跑硬件加速渲染**，而依赖 X/Wayland 的方案在免 root 下
  连 server 都起不来。

要在容器里跑「真正的 X11 应用」，只有三条路：
1. root（不满足免 root 前提）；
2. 让 App 侧自己实现一个 X server（工程量巨大）；
3. 走 Xvfb 但把锁文件目录换到 SELinux 允许 link 的上下文（需要设备策略配合，
   通用性差）。

## 33. phantom process killer 的判定口径 —— 用 run-as 测不出来

Android 12+ 有 phantom process killer：App 派生出的**幽灵进程**总数超过
`max_phantom_processes`（模拟器实测 32）就会被系统整批杀掉。TAWC 的 README
把这条列为限制，要求用户去开发者选项关掉「禁止子进程限制」。

但**用 `run-as` 起容器无法验证这条限制**，因为两者的进程归属不同：

```
run-as 起的容器：  cat /proc/self/cgroup  →  memory:/  cpuset:/  cpu:/
App 自己起的容器：  cat /proc/<apppid>/cgroup → memory:/ cpuset:/top-app cpu:/top-app
```

`run-as` 的父进程是 adbd，不在 App 的进程树里，因此 phantom killer 的计数
根本不会把它算进去。实测 `run-as` 下起 120 个 `sleep` 全部存活（124 个进程），
**这个数字对「App 内能跑多少进程」毫无参考价值**。

要真测这条限制，必须在 App 进程内启动容器（即真机装上 App 走 UI），
再看 logcat 的 `Excessive phantom processes` 与进程存活情况。这轮手机离线，
所以此项**未实测**。

### 对我们可能的结构性优势（待真机验证）

我们的容器进程是 App 自己的子进程（`ps -A -o PID,PPID` 里 PPID = App pid，
走 binder 关联），理论上比 TAWC 那种「独立启动、靠 shell 挂载」的形态更容易
被系统认作 App 的一部分。若真机上确认不触发 phantom killer，那意味着
**用户不用去开发者选项关任何东西**，这是能写进对比表的实打实差异。

**但这只是推测，必须真机验证后才能当结论用。**

## 34. `realpath()` 在 bind 挂载点下全部失败 —— 整条引擎起不来

**症状**：手工起引擎时报

```
hmr (@deepseek-ai/dsh-hmr): Error: EACCES: permission denied, watch '/'
dsh: startup failed: 2 required plugins did not activate
```

同时 `[android-plugin-health] profile is degraded: missing=typert,typertGateway,
directoryPicker,directoryPickerController,workspaceRegistry,workspaceController,
webServer,loader failed=...`（7 个服务全 missing）。

**这条坑最贵的地方在于症状离根因极远**：日志里只有 `watch '/'`，看不到任何
「路径没翻译」的线索。而 `/` 是 chokidar 沿父目录逐级回退的**终点**，不是起点。

### 定位方法（可复用）

用 `--require` 给 `fs.watch` 打桩，把每次调用的真实入参打出来：

```js
const fs = require("node:fs");
const orig = fs.watch;
fs.watch = function (target, options) {
  fs.writeSync(2, "[watchhook] watch(" + target + ", cwd=" +
    (options && options.cwd ? options.cwd : "-") + ")\n");
  try { return orig.apply(this, arguments); }
  catch (e) { fs.writeSync(2, "[watchhook] THROW " + target + " :: " + e.code + "\n"); throw e; }
};
```

实测输出（全部 ENOENT）：

```
[watchhook] watch(/dsh/profiles/web)                        -> ENOENT
[watchhook] watch(/dsh/profiles/web/cordis.patch.yml)        -> ENOENT
[watchhook] watch(/dsh/.credentials.yaml)                    -> ENOENT
[watchhook] watch(/dsh)                                      -> ENOENT
```

**链条**：`fs.watch` → `inotify_add_watch` 收到 guest 路径 → 内核按**宿主**根解析
→ ENOENT → chokidar 沿父目录回退到 `/` → `EACCES, watch '/'` → dsh-hmr 激活失败
→ 依赖 `webServer` 的 `web-runtime` / `open-in-app` / `client-hmr` 一起挂 → 整条
startup 判死。

### 原因一：`realpath()` 的 musl 实现依赖 `/proc/self/fd`

musl 的 `realpath()` 走 `open(path, O_PATH)` + `readlink("/proc/self/fd/N")`。
而 `/proc` 整棵前缀是**透传给宿主**的（`is_host_passthrough`），所以 `readlink`
返回的是**宿主机真实路径**：

| guest 路径 | readlink 实际返回 |
|---|---|
| `/tmp` | `/data/data/app.dsh.mobile/files/linux/distros/alpine/tmp` |
| `/dsh` | `/data/data/app.dsh.mobile/files/dsh-home` |

`realpath()` 拿到后还要 `stat` 这条宿主路径 —— 容器里不存在，于是 ENOENT。
**任何** bind 挂载点下的 `realpath` 全失败，不只是 hmr 用的路径。

**修法**：在 `SYS_readlinkat` 分支里识别 `/proc/[self|PID]/fd/N`，把内核返回的
宿主路径反查成 guest 路径：

```c
if (nr == SYS_readlinkat && a0 == -100 && is_proc_self_fd(path)) {
    long rvf = raw6(nr, -100, (long)path, (long)a2, (long)a3, 0, 0);
    if (rvf > 0) {
        int n2 = host_to_guest((char *)a2, (int)rvf);   // bind 表反查，再退 rootfs 前缀
        if (n2 >= 0) rvf = n2;
    }
    r[0] = (unsigned long long)rvf;
    return;
}
```

`host_to_guest()` 的顺序是**先 bind 表反查（最长匹配）再 rootfs 前缀裁剪** ——
和 `getcwd` 那边同一个道理：只裁 rootfs 前缀对 bind 目录无效。

### 原因二：`inotify_add_watch` 没进 BPF 树

`inotify_add_watch(fd, pathname, mask)` 的路径在 **a1**，而且**没有 `*at` 变体**
（内核只给了这一个入口）。原树只拦 `*at` 一族，这一条被透明转发，guest 路径
直接落到宿主根。

**修法**：加进 `tools/gen_bpf_tree.py` 的 `NEEDED`，并在 handler 里像 `statfs`
那样用 `make_host()` 拼宿主绝对路径：

```c
case SYS_inotify_add_watch: {
    const char *ip = (const char *)a1;
    if (ip && ip[0] == '/' && !is_host_passthrough(ip)) {
        char ib[PATH_MAX];
        if (bind_map(ip, ib)) { r[0] = raw6(SYS_inotify_add_watch, a0, (long)ib, a2, 0,0,0); return; }
        make_host(ib, ip);
        r[0] = (unsigned long long)raw6(SYS_inotify_add_watch, a0, (long)ib, a2, 0, 0, 0);
        return;
    }
    r[0] = (unsigned long long)raw6(nr, a0, (long)ip, a2, 0, 0, 0);
    return;
}
```

inotify 在**宿主路径**上注册不影响事件回传：wd 只跟 inode 走，与打开的路径无关。

### 验证

```sh
# 1) realpath 全通
node -e 'const {realpathSync}=require("node:fs");
for (const p of ["/","/dsh","/dsh/profiles/web","/eng","/ws","/tmp"])
  console.log(p, "->", realpathSync(p));'
# / -> /   /dsh -> /dsh   /dsh/profiles/web -> /dsh/profiles/web   /eng -> /eng ...

# 2) fs.watch 全通
node -e 'const {watch}=require("node:fs");
for (const p of ["/dsh/profiles/web","/eng","/tmp"]) {
  try { watch(p,{persistent:false},()=>{}).close(); console.log("OK", p); }
  catch(e){ console.log("FAIL", p, e.code); } }'
# WATCH-OK /dsh/profiles/web    WATCH-OK /eng    WATCH-OK /tmp

# 3) 引擎完整启动（修复前 startup failed，修复后只有一行）
# dsh web: http://127.0.0.1:3080/?token=...
```

**修复前后对比**：修复前 `dsh: startup failed: 2 required plugins did not activate`
+ 7 个服务 missing；修复后日志**只有一行** `dsh web:`，首页 46437 字节、
70 个客户端插件全部入 boot manifest。

## 35. AF_UNIX socket 路径不翻译 —— 所有 compositor / X11 / dbus 都起不来

**症状**（三种表现，同一条根因，极易误判成三个不同问题）：

```
# 服务端（weston / Xvfb / 任何监听 AF_UNIX 的程序）
libwayland: bind() failed with error: No such file or directory
fatal: failed to add socket: No such file or directory

# 客户端（修好 bind 之后才会看到这一层）
failed to create display                    # grim / wayland 客户端
libwayland: failed to connect to socket

# X11
Fatal server error:
Linking lock file (/tmp/.X99-lock) in place failed: Permission denied
```

**原因**：`sockaddr_un.sun_path` 也是 guest 路径，但 `bind` / `connect` 这两个
syscall **原本都不在 BPF 树里**，于是被透明转发，内核拿 guest 路径去宿主根下
解析 —— 那个目录在宿主不存在，返回 ENOENT（X11 那条是 `link()` 另有限制，
见下）。

这是**两层独立的缺失**，必须都修，否则症状会从「服务端起不来」变成
「服务端起来了但客户端连不上」：

| 层 | syscall | 缺了会怎样 |
|---|---|---|
| 1 | `bind` | socket 文件建不出来 → compositor 直接 fatal 退出 |
| 2 | `connect` | socket 建出来了，但客户端连不上 → `failed to create display` |

**修法**（`dsh-linux-runtime/src/dsh_alpine.c`）：

1. `tools/gen_bpf_tree.py` 的 `NEEDED` 加 `"bind"`（`connect` 本来就在，
   但它只处理 AF_INET 的 DNS 端口改写，要另开 AF_UNIX 分支）。
2. 两个 handler 里对 `fam == 1`（AF_UNIX）且 `sun_path[0] == '/'` 的地址做
   路径翻译：先 `bind_map()`（命中 bind 挂载点），再 `make_host()`
   （拼 rootfs 前缀）。
3. **必须重新拼一个完整的 `sockaddr_un`**，不能只把路径字符串换掉再传原
   `a1` 指针 —— 长度语义要求 `addrlen = 2 + strlen(path) + 1`，且
   `sun_path` 紧跟 `sun_family`。实测只改 addrlen 而内容仍指 guest 缓冲区时
   内核看到「新长度 + 旧内容」，直接 `EINVAL`（三个路径全部 Invalid argument）。

```c
struct sockaddr_un_s { unsigned short sun_family; char sun_path[108]; } sa2;
char hb[PATH_MAX];
const char *use;
if (bind_map(pb, hb)) use = hb;
else { make_host(hb, pb); use = hb; }
int plen = ldr_strlen(use);          // freestanding：没有 strlen，用 ldr_strlen
if (plen > 107) plen = 107;
sa2.sun_family = 1;                   // AF_UNIX
for (int i = 0; i < plen; i++) sa2.sun_path[i] = use[i];
sa2.sun_path[plen] = 0;
r[0] = raw6(SYS_bind, a0, (long)&sa2, (long)(2 + plen + 1), 0, 0, 0);
```

抽象命名空间（`sun_path[0] == 0`）与相对路径**不翻译**，原样透传。

**验证**（模拟器 dsh34 / Android 14 / x86_64）：

```
# 1. 裸 syscall 层
bind(/tmp/wl/t1) = 0 OK    listen = 0 OK
bind(/tmp/t3)     = 0 OK    listen = 0 OK
bind(/dsh/t2)     = 0 OK    listen = 0 OK

# 2. 真实 compositor（weston 14.0.2 headless 后端）
[log] Loading module '/usr/lib/libweston-14/headless-backend.so'
[log] Output 'headless' enabled with head(s) headless
[log] Loading module '/usr/lib/weston/desktop-shell.so'
[log] launching '/usr/libexec/weston-desktop-shell'
[fs]  /tmp/wl/test        (srwxrwxrwx)   ← socket 建出来了
[fs]  /tmp/wl/test.lock

# 3. 真实 Wayland 客户端
weston-simple-shm  → 存活（持续重绘），不再 failed to create display
grim /tmp/shot.png → 报 "compositor doesn't support wlr-screencopy-unstable-v1"
                     ↑ 这是协议不支持，说明**已经连上 compositor 了**
                     （修 connect 之前报的是 "failed to create display"）
```

**顺带澄清一条旧结论**：`docs/pitfalls.md` #32 曾把 X11/Xvfb 起不来归因成
「SELinux 禁止 `link()`」。那个限制**确实存在**（avc 实测：
`avc: denied { link } for comm="ln" scontext=u:r:runas_app:s0
tcontext=u:object_r:app_data_file:s0 tclass=file permissive=0`，
而无 root 时 `Xvfb -nolock` 也报 "can only be used by root"），但它只是
**X11 专属的第二道限制**；任何 AF_UNIX 服务的**第一道**限制都是本文这条路径
翻译缺失。wayland 路线不需要 `link()`，所以走 wayland compositor 可以绕开
X11 那道。

## 36. headless 后端默认 noop 渲染器 —— 窗口建了但什么都没画

**症状**：`weston-debug scene-graph` 明明列出了 view（窗口存在、位置尺寸都对），
但截图拿不到内容；`weston-screenshooter` 报 `Assertion failed: width > 0`。

**原因**：`--backend=headless-backend.so` 的 `--renderer` 默认是 `noop`
（名字就写着 "No-op renderer for testing only"）。`noop` 只走合成流程、不算像素，
所以 view 有几何信息但缓冲区里没有内容。

**修法**：

```sh
weston --backend=headless-backend.so --renderer=pixman ...
# 日志里应出现：Using Pixman renderer
```

**验证**（模拟器 dsh34 / x86_64）：

```
[15:01:11.290] Using Pixman renderer
[15:01:11.290] Output 'headless' enabled with head(s) headless
```

加上 `--renderer=pixman` 后 scene-graph 才有真实缓冲区（`SHM buffer` +
`format: 0x34325241 ARGB8888`），窗口内容才可能被抓出来。

### 顺带记录：weston 14 的 capture 授权限制

即使渲染器换成 pixman，`weston-screenshooter` 仍报
`Output capture error: unauthorized`。这是 weston 14 起 `weston_capture_v1`
的**授权模型**：能力只授予 screen-share 模块拉起的 RDP 客户端，第三方工具
（screenshooter / grim）一律拒绝。`--modules=screen-share.so` 加载模块本身
不足以授权，需要走它的 RDP/pipewire 输出链路。

**结论**：想在容器内验证「窗口里画了什么」，别指望 weston 的截图工具。
两条可行路径：
1. 让客户端的 `wl_shm` 池落成真实文件（LD_PRELOAD 一份 `memfd_create` 替身，
   把池子建在 `$rootfs/tmp/` 下，宿主直接读那些 .bin）。**未实测**——容器里
   没有 gcc，装不上编译器。
2. 走 weston 自己的 `weston_capture_v1`（需要写一个绑定该协议的客户端程序）。

**但更重要的判断**：这两条都不是产品路径。产品要的是「容器内 GUI 程序的画面
进 App」，而这条链路我们**已经用另一套机制解决过** —— 容器内的 GL 程序直接用
`lib/dsh_gl` 把帧写进共享文件，App 侧 `LinuxFrameSurface` mmap 同一个文件贴到
SurfaceView。compositor 截图的困难不影响那条路径。

### 关键实测：GUI 窗口确实在容器内合成出来了

```
$ weston-debug scene-graph
Weston scene graph at 31126.295404300:
Output 0 (headless):
	position: (0, 0) -> (640, 360)
	mode: 640x360@60.000Hz
Layer 4 (pos 0x80000000):
	View 0 (role xdg_toplevel, PID 29355, surface ID 3,
	        top-level window 'simple-shm' of org.freedesktop.weston.simple-shm):
		position: (307, 79) -> (557, 329)
		outputs: 0 (headless) (primary)
		SHM buffer
			format: 0x34325241 ARGB8888
			modifier: LINEAR (0x0)
	View 0 (role (null), PID 29351, surface ID 16,
	        background for output headless):
		position: (0, 0) -> (640, 360)
```

`role xdg_toplevel` + `top-level window 'simple-shm'` = 一个真实的顶层窗口，
250×250 居中（307,79 → 557,329），带 panel 与 background。**这是容器内
GUI 应用开窗口可行性的直接证据**，且它依赖的就是本文档 #35 的 AF_UNIX 修复。

## 37. 容器内装包完全依赖 App 进程的 DNS 代理 —— App 不在线就装不了任何东西

**这不是 bug，是一条必须知道的架构约束**，但排查时极易误判成「网络坏了」。

**现象**：手工用 `root`（或 `run-as`）在设备上直接跑容器：

```sh
env DSH_DNS_PORT=15353 <runner> <rootfs> /lib/ld-musl-x86_64.so.1     /sbin/apk add --no-progress weston
# → rc=99
# → ERROR: Failed to open apk database: temporary error (try again later)
```

**原因**：App uid 绑不上 53（特权端口），所以容器的 DNS 由 **App 进程内的
`LinuxDnsProxy`** 在 127.0.0.1:15353 上代答，loader 负责双向端口改写。
**App 进程不在，就没人监听 15353** —— 而 `DSH_DNS_PORT=15353` 仍照常传给
loader，于是容器把查询发到一个死端口，表现和「镜像站挂了」一模一样。

**判据**（一行确认）：

```sh
adb shell "cat /proc/net/tcp | awk '{print \$2}' | grep -ci 3BF9"
# 0 = 代理没在跑（0x3BF9 = 15353）
```

**实际后果**：
- 用 `root`/`run-as` 手工起容器做实验时，**必须先让 App 跑起来**，否则任何
  联网操作（`apk update` / `apk add` / `wget`）都失败。
- 反过来，如果用户能装上包，说明 App 一定是活的、代理一定起来了。
- 若 App 被省电策略杀掉而后台引擎还在跑，也会落到这个状态（`DshApp` 的注释
  里记了这条，所以代理预启动放在 `onCreate` 而不是 `readiness()`）。

**已修的相邻问题**：`LinuxPackageManager` 现在会在 `refreshIndex()` /
`install()` 执行前调一次 `ensureDnsProxy()`，避免「刚开 App、代理还在后台线程
起、用户已点安装」的竞态。但这只是让代理"尽快起来"，不能改变「App 必须在」
这个前提。

## 38. 在容器里装大包会被 ActivityManager 杀掉 —— 不是 phantom killer

**现象**：在设备上手工跑 `apk add nodejs`（14 个依赖，其中 `icu-data-full` /
`nodejs` 是几十 MB 的大包），进度卡在 `(14/14) Installing nodejs` 十几分钟不动，
最后进程消失、`DONE` 标记没写、`/usr/bin/node` 不存在。

**判断依据**（关键，别往 phantom killer 上猜）：

```sh
adb shell "logcat -d | grep -E 'Killing.*app.dsh.mobile|phantom'" | tail
# ActivityManager: Killing 30965:app.dsh.mobile/u0a204 (adj 0):
#   stop app.dsh.mobile due to from pid 32189
```

`ActivityManager ... adj 0 ... stop app.dsh.mobile` = **Activity 全被销毁后进程被判
可回收**，与 phantom process killer（那条日志是 `Excessive phantom processes`）
是两回事。手工在 `root`/`run-as` 下跑容器时 App 界面不在前台，很容易踩到。

**规避**：
- 装大包前保持 App 在前台（或至少让它有活跃 Activity）。
- 分批装，避免一次拉十几个大依赖。
- 大包（`weston` 110 包 / 458 MiB、`nodejs` 14 包含 `icu-data-full`）在模拟器上
  **实测极慢**（>15 分钟），不适合当自动化验证的一部分。

**顺带记录：`apk` 被 `kill -9` 后会留下数据库锁**，之后所有 `apk` 操作报
`ERROR: Unable to lock database: temporary error (try again later)`。清掉即可：

```sh
rm -f <rootfs>/lib/apk/db/lock
```

**顺带记录 2：手工解包 minirootfs 必须把绝对软链改成相对**，否则
`LOADER: preload FAIL`。官方 minirootfs 的 applet 全是
`cat -> /bin/busybox` 这种**绝对**链接，而软链内容不经过路径翻译
（见 #30），内核按宿主根找 `/bin/busybox` → 不存在。App 的部署逻辑
（`LinuxEnvironmentManager` 的 `linkPath` 重写）会自动改成相对，手工解包不会。

改写时**必须只改真软链、不能碰 `busybox` 本体**，否则会把本体变成指向自己的
死链（我第一次就写错了，症状同样是 `LOADER: preload FAIL`）：

```sh
cd <rootfs>
for d in bin sbin usr/bin usr/sbin; do
  [ -d "$d" ] || continue
  for f in "$d"/*; do
    [ -L "$f" ] || continue          # ← 关键：只碰 symlink
    t=$(readlink "$f")
    case "$t" in
      /bin/busybox|*/bin/busybox)
        case $d in
          usr/bin|usr/sbin) ln -sf ../../bin/busybox "$f" ;;
          sbin)             ln -sf ../bin/busybox "$f" ;;
          *)                ln -sf busybox "$f" ;;
        esac ;;
    esac
  done
done
# 验证：/bin/sh -c 'cat /etc/alpine-release; uname -m' → 3.22.2 / x86_64
```

## 39. 有 root 时容器可直接用真实外部 DNS —— 只有免 root 才依赖 App 的代理

补 #37 的另一半。**之前把「容器必须靠 App 进程的 `LinuxDnsProxy`」当成了普遍
约束，实测有 root 时不成立。**

`root` 域（`u:r:su:s0` 或 adb root）下容器可以直连真实外部 DNS：

```sh
echo 'nameserver 223.5.5.5' > <rootfs>/etc/resolv.conf
<runner> <rootfs> /lib/ld-musl-x86_64.so.1 /bin/sh -c 'nslookup mirrors.aliyun.com'
# Server:  223.5.5.5
# Address: 223.5.5.5:53
# mirrors.aliyun.com canonical name = mirrors.aliyun.com.w.alikunlun.com
# Address: 112.15.7.205
```

**所以两条路的分工**：

| 场景 | DNS 走法 | 前提 |
|---|---|---|
| App 内（免 root，`untrusted_app`/`runas_app`） | App 进程的 `LinuxDnsProxy` + loader 双向端口改写（`DSH_DNS_PORT`） | **App 必须在跑**（见 #37） |
| root / adb root | 直接写真实 nameserver，走真 53 | 有 root |

排查时先确认**当前是哪个域**，别看错方向：`run-as app.dsh.mobile id` 会显示
`context=u:r:runas_app:s0:cXXX,...`；`adb root` 后是 `u:r:su:s0`。

## 40. AHB 零拷贝：四个坑串成一条链，每个都只差一步

AHardwareBuffer（AHB）能让 GPU 直接渲染进一块跨进程可见的缓冲，彻底省掉
`glReadPixels`（1080p 占 83% 的那一步）。设备侧能力全都在（实测
`/system/lib64` 里有 `AHardwareBuffer_allocate/lock/describe/getNativeHandle`，
`libEGL.so` 扩展含 `EGL_ANDROID_get_native_client_buffer`），但要走通必须
按顺序踩过这四个坑，**任何一个错了都会表现为「看起来通了其实没通」**。

### 坑 1：`eglCreateContext` 传 NULL config → `EGL_BAD_CONFIG(0x3005)`

```c
void *ctx = eglCreateContext(dp, (void*)0, (void*)0, ctxAttr);   // ✗ 0x3005
```

必须先 `eglChooseConfig`：

```c
int cfgAttr[] = {
    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
    EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE
};
EGLConfig cfg; EGLint n = 0;
eglChooseConfig(dp, cfgAttr, &cfg, 1, &n);
```

### 坑 2：`eglMakeCurrent` 用 surfaceless → `EGL_BAD_MATCH(0x3009)`

```c
eglMakeCurrent(dp, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);   // ✗ 0x3009
```

该 EGL 实现没开 `EGL_KHR_surfaceless_context`。**必须绑一个真实 pbuffer**：

```c
int pbAttr[] = { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE };
EGLSurface surf = eglCreatePbufferSurface(dp, cfg, pbAttr);
eglMakeCurrent(dp, surf, surf, ctx);                        // ✓
```

（`bench/framesrv.c` 一直走 pbuffer，所以它从没碰到这个问题 —— 这也是为什么
「照抄成功案例」比「凭 EGL 规范推理」可靠。）

### 坑 3：`AHardwareBuffer_lock` 少声明一个参数 → `-22(EINVAL)`

真实签名是 **5 个参数**：

```c
int AHardwareBuffer_lock(AHardwareBuffer *buffer, uint64_t usage,
                         int32_t fence, const ARect *rect, void **outVirtualAddress);
```

漏掉 `rect` 时调用方按 4 参压栈，被调方从错误位置取 `rect` → 返回 `-22`。
**编译不报错**，只是永远锁不上。这个错最隐蔽。

另外 `usage` 必须与申请时一致：申请 `CPU_READ_OFTEN`(0x2) 就传 0x2；
传 `CPU_READ_RARELY`(0x1) 同样 `-22`。

### 坑 4（最关键）：只把 AHB 变成 EGLClientBuffer 不等于渲染进 AHB

```c
void *cbuf = eglGetNativeClientBufferANDROID(ahb);   // 拿到 client buffer
glClear(...);                                        // ✗ 画在 pbuffer 上！
```

实测症状：**整个探针打印 `RESULT: AHB_OK`，但 CPU lock 读回的 center RGBA 是
`0,0,0,0`** —— 渲染落在当前绑定的 pbuffer surface 上，AHB 那块内存一个字节都没被写。
这就是典型的「以为通了其实没通」。

必须走完整四步把 AHB 挂成 FBO 的 colour attachment：

```c
/* 1. AHB -> EGLClientBuffer */
void *cbuf = eglGetNativeClientBufferANDROID(ahb);

/* 2. EGLClientBuffer -> EGLImage（target 必须是 EGL_NATIVE_BUFFER_ANDROID=0x3140） */
int imgAttr[] = { EGL_IMAGE_PRESERVED_KHR, 1, EGL_NONE };
EGLImage img = eglCreateImageKHR(dp, EGL_NO_CONTEXT,
                                 EGL_NATIVE_BUFFER_ANDROID, cbuf, imgAttr);

/* 3. EGLImage -> GL texture */
GLuint tex; glGenTextures(1, &tex);
glBindTexture(GL_TEXTURE_2D, tex);
glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)img);

/* 4. texture -> FBO attachment */
GLuint fbo; glGenFramebuffers(1, &fbo);
glBindFramebuffer(GL_FRAMEBUFFER, fbo);
glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                       GL_TEXTURE_2D, tex, 0);
/* 必须检查 status == GL_FRAMEBUFFER_COMPLETE(0x8CD5) */
```

### 验证：四步全通 + 像素正确 + 量化

```
nativewindow ok
AHB symbols ok (lock=0x... unlock=0x...)
AHB allocated 640x360 ahb=0x...
AHB describe: 640x360 fmt=0x1 usage=0x302 stride=640
EGL 1.4 vendor=Android
EGL_ANDROID_get_native_client_buffer: present
EGLConfig chosen n=1 cfg=0x5
pbuffer surface = 0x... (err=0x3000)
GL_RENDERER=Android Emulator OpenGL ES Translator (Google SwiftShader)
EGLClientBuffer from AHB = 0x...
EGLImage from AHB = 0x...
texture=1 bound to EGLImage
FBO=1 status=0x8cd5 (0x8CD5=COMPLETE)
cleared into AHB
center RGBA = 255,89,26,255          ← 对齐 glClearColor(1.0, 0.35, 0.1)
render into AHB: 0.341 ms/frame (100 frames)   ← 1080p
AHB lock: 4.498 ms
RESULT: AHB_OK
```

三档分辨率实测（模拟器 dsh34 / x86_64 / SwiftShader，**只渲染、不含取像素**）：

| 分辨率 | render into AHB（纯渲染） |
|---|---|
| 640×360 | 0.044 ms/帧 |
| 1280×720 | 0.129 ms/帧 |
| 1920×1080 | 0.341 ms/帧 |

> ⚠️ **不要把上面这张表拿去和「render + glReadPixels」的数字比。**
> 这里只有渲染，没有 GPU→CPU 取像素那一步，横比会算出 19.9 倍这种假收益
> （本节初次发布就是这么错的）。真实收益必须用**同进程 A/B** 测：
> 见 pitfalls #41 与 benchmark 第 14 节 —— 1080p 端到端 **1.72 倍**
> （AHB 6.344 ms vs glReadPixels 10.930 ms）。
>
> `AHB_lock` 也不是一次性的：每次取像素都要 lock 一次，它是每帧成本。

### 一个遗留现象（不影响结论）

探针结尾稳定打印
`FORTIFY: pthread_mutex_lock called on a destroyed mutex (0x...)`。
这是 bionic 在进程退出阶段对已销毁 mutex 的检查（`glFinish` 后 EGL 已开始
teardown），发生在 `RESULT: AHB_OK` **之后**，不影响任何测量数据。
真机（Adreno）上未验证是否同样出现。

## 41. 跨探针数字不能横比 —— 我把 1.72 倍写成了 19.9 倍

这是本仓库唯一一条**关于测量方法本身**的坑，比任何单个技术 bug 都值得记。

### 事情经过

`bench/ahb_test.c` 测出「1080p 渲染进 AHB = **0.341 ms/帧**」。
`benchmark.md` 第六节记录的是「1080p render + glReadPixels + write = **6.777 ms/帧**」。

我把这两个数一除，写成了 **19.9 倍**，还发布进了 README 级的对比表。

**错在哪**：`0.341 ms` 只包含渲染，`6.777 ms` 包含渲染 + 回读 + 落盘。
一个是 `glClear + glFinish`，另一个是完整帧泵。**分子分母口径不同。**

### 正确的测法：同进程 A/B

必须让两条路径在**同一个二进制、同一轮循环、同一个 EGL 上下文**里各跑一遍
（`bench/ahb_topipe.c`）：

```
[A] AHB  render 1.144 + lock 3.954 + copy 1.213 = 6.344 ms/frame (157.6 fps)
[B] RPIX render 1.111 + read 8.379 + copy 1.440 = 10.930 ms/frame (91.5 fps)
>>> A/B ratio = 1.723x  (AHB FASTER)
```

真实收益：**1.72 倍**（720p 1.58×、360p 1.35×），不是 19.9 倍。
来源集中在「GPU→CPU 取像素」：`AHB_lock` 3.954 vs `glReadPixels` 8.379（2.1 倍）。
render 和 copy 两段基本没变，所以整体倍数被稀释。

### 顺带纠正两个说法

1. **`AHB_lock` 不是一次性的**。它每次取像素都要调，是每帧成本。之前写的
   「4.5ms 是一次性开销，只在交给 SurfaceTexture 时发生」也是错的。
2. **AHB 并不「免掉回读」**。GPU 渲染进 AHB 之后，CPU 要拿数据仍然必须
   `AHB_lock`，而 lock 内部同样做 GPU→CPU 同步。AHB 的价值是**这条路更快**
   （驱动侧对 dmabuf 的 cache 处理比 `glReadPixels` 那套逐行拷更高效），
   不是「不需要回读」。
   **真正不需要回读的只有一种情况**：AHB 直接交给 SurfaceFlinger / SurfaceTexture
   由 GPU 侧消费 —— 但那需要 `AHardwareBuffer_createFromHandle`，本机不可用（见下）。

### `AHardwareBuffer_createFromHandle` 在本机不可用

想在另一个进程里重建 AHB，唯一官方途径是 `createFromHandle`。本机（模拟器
dsh34 / Android 14 / x86_64）实测**一律返回 `-22(EINVAL)`**：

```
[selfcheck] real describe: 640x360 fmt=0x1 usage=0x30a stride=640
[selfcheck] createFromHandle(real desc, CLONE) rc=-22 ahb=(nil)
[selfcheck] createFromHandle(real desc, REFERENCE) rc=-22 ahb=(nil)
[selfcheck] createFromHandle(zero desc, CLONE) rc=-22 ahb=(nil)
[child] handle copy 104 bytes (version=12 numFds=2 numInts=21)
[child] FAIL createFromHandle rc=-22
```

**已经排除的因素**（每一条都实测过）：

| 怀疑点 | 排除方式 | 结果 |
|---|---|---|
| 参数个数错（是 4 参：desc/handle/method/out） | 逐个补齐 | 仍 -22 |
| `desc` 用了申请时那份（stride=0） | 改用 `AHardwareBuffer_describe` 回读的真实 desc（stride=640） | 仍 -22 |
| `desc` 应该全零让底层推断 | 试零 desc | 仍 -22 |
| method 选错（CLONE vs REFERENCE） | 两个都试 | 两个都 -22 |
| handle 复制长度错（`version`=12 只是头，不是全长） | 改成 `sizeof + (numFds+numInts)*4` = 104 字节 | 仍 -22 |
| 跨进程 fd 继承 / SELinux | **同进程自测也失败** | 说明不是跨进程问题 |

**真根因（环境级）**：

```
servicemanager: Could not find android.hardware.graphics.allocator.IAllocator/default
                in the VINTF manifest.
Gralloc4: mapper 4.x is not supported
```

模拟器的 gralloc 没有暴露 allocator HAL 的 VINTF 入口，`createFromHandle`
需要走 mapper 去校验/重建，因此直接 EINVAL。**这是模拟器限制，真机（Adreno 840）
很可能可用，但本轮真机离线（`192.168.101.21:9000` 连续 10+ 轮连不上）→ 未验证。**

### 可行的退路

`bench/ahb_topipe.c` 走「AHB 渲染 → lock → memcpy 到共享文件」，
App 侧仍是原来的 mmap 路径、零改动。1080p 端到端 6.344 ms/帧（158 fps），
比 glReadPixels 路径快 1.72 倍。**这是当前设备上能拿到的最好数字。**

### 附带踩到的两个小坑

1. **bionic 程序经 cleaner 在宿主侧执行时，路径按宿主根解析**。
   探针里写 `open("/tmp/ahbf.bin")` 会失败（容器 `/tmp` ≠ 宿主 `/tmp`），
   必须传宿主绝对路径。症状是 `FAIL open` 但容器里 `touch /zz` 明明成功。
2. **`glReadPixels` 是 7 个参数**（x, y, w, h, format, type, pixels）。
   少声明一个编译期就会报 `too many arguments`，这条倒是很好抓。
