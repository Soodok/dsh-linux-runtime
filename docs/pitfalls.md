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
