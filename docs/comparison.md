# 与 tawcroot / TAWC 的能力对照

> 本文只写**我们实测过的**能力，以及对 TAWC 的**源码/文档核对结论**。
> 逐条标注证据来源：`[实测]` 本项目在设备上跑出来的数字，`[源码]` 读实现读出，
> `[官方]` 来自对方 README，`[推测]` 未验证。

## 一、架构同源，不是独创

必须先讲清楚：tawcroot 与我们**走的是同一条技术路线**，我们不是独创架构。

| 机制 | tawcroot | 我们 | 证据 |
| --- | --- | --- | --- |
| 拦截方式 | cBPF + `SECCOMP_RET_TRAP` | 同 | [源码] 双方都读 |
| 处理入口 | `SIGSYS` + `rt_sigaction(SA_SIGINFO)` | 同 | [源码] |
| 自身 syscall 放行 | IP 白名单比对 64-bit IP 低/高 32 位 | 同（`LOADER_BASE_HI`） | [源码] |
| 重执行 | `execveat(/proc/self/exe, AT_EMPTY_PATH)` + memfd 传状态 | 同 | [源码] |
| 子进程装载 | `--exec-child` + freestanding 手写 ELF loader | 同 | [源码] |
| 进程模型 | 单进程 | 同 | [官方] TAWC README 自述 |

**结论：这条路是 tawcroot 先公开的，DSHA 照做谈不上抄我们。** 我们在架构上没有壁垒，
也不该在宣传里暗示有。

## 二、我们实打实的差异（附证据）

### 2.1 免 root 的 DNS 双向端口改写 `[实测]`

容器内 `/etc/resolv.conf` 是 `nameserver 127.0.0.1`（App 侧的 DNS 代理），而 **53 是
特权端口、App uid 绑不上**。musl 又把 DNS 端口硬编码成 53（写 `nameserver ip:port`
会被整个忽略）。所以由 loader 在 syscall 层改写端口：

- 出程 `sendto/sendmsg/send`：目标 `127.0.0.1:53` → 代理实际端口
- 回程 `recvfrom/recvmsg/recv`：来源端口改回 53（musl 会校验应答来源）

**决定性对照实测**（同一条命令，只差环境变量）：

```
$ ... /bin/sh -c 'export DSH_DNS_PORT=15353; nslookup github.com'
Non-authoritative answer:
Name:	github.com
Address: 20.205.243.166

$ ... /bin/sh -c 'nslookup github.com'          # 不传
nslookup: write to '127.0.0.1': Connection refused
;; connection timed out; no servers could be reached
```

顺带一提：`apk update` 不传变量也可能成功，那是因为 Alpine 镜像走 `/etc/hosts`
静态解析；换成 hosts 里没有的域名立刻就露馅。**验证 DNS 必须用 hosts 外的域名。**

TAWC 的做法是 `ando` 命令 + storage binds，没有在 syscall 层做端口改写
`[官方]`。它用的是 glibc（Arch/Debian），glibc 可以直连 53 也可配
`nameserver ip:port`，所以它不是没能力，而是**不需要**——这点必须诚实。

→ 这条差异的真实价值：**musl 发行版（Alpine）在免 root Android 上能正常解析域名**。

### 2.2 容器内用宿主 GPU 渲染并把画面送回 App `[实测]`

```
EGL 1.4 vendor=Android
GL_RENDERER=Android Emulator OpenGL ES Translator (Google SwiftShader)   # 真机为 Adreno 840
```

全链路帧率（模拟器 / 三档分辨率 / 三轮中位）：

| 分辨率 | 容器→共享文件 | App 显示端 |
| --- | --- | --- |
| 640×360 | 1405.8 fps | — |
| 1280×720 | 324.6 fps | 28~31 fps |
| 1920×1080 | 150.1 fps | 35.8~37.2 fps |

显示端 30 fps 的天花板来自模拟器 vsync（对照实验：去掉 `drawBitmap` 只清屏仍是
30 fps），不是我们的代码。`lib/dsh_gl` 把这个能力封装成 3 文件小库。

TAWC 在这一点上**更强**：它有 Smithay compositor + `libhybris` fork，能开真正的
系统窗口、XWayland 硬件加速、硬件合成（零回读）。我们目前只能「渲染 → 回读 →
贴进 App 的一个 SurfaceView」，没有独立窗口、没有硬件合成。

→ 这条差异是**形态差异**，不是技术优势：他们是「装个 TAWC 跑 Linux 桌面」，
我们是「App 里内嵌一个 Linux」。别把它说成技术壁垒。

### 2.3 x86_64 架构支持 `[实测]`

TAWC README 明确：「Only arm64 official builds for now. Can be built for x86 but
our libhybris tricks rely on arm.」`[官方]`

我们把整个 loader 做成双架构，实测 x86_64 容器能跑（本仓库全部模拟器数据都来自
x86_64 容器）。但要说清代价：**x86_64 只是给模拟器/开发用**，真机交付是 arm64。

### 2.4 跑在 App 内、与引擎共享同一份数据 `[实测]`

容器进程是 App 的子进程（`ps -A -o PID,PPID` 里 PPID = App pid），`/data/data`、
`/storage/emulated/0`、`/eng`、`/ws`、`/dsh` 都按同名 bind 进去，所以容器里的程序
读到的是 App 的同一份 workspace、同一份 profile，不需要额外同步。

TAWC 是独立 App + `ando` 命令做桥接 `[官方]`。

### 2.5 其他实测到的能力（细节见 pitfalls.md）

| 能力 | 说明 | 证据 |
| --- | --- | --- |
| `/proc` `/sys` `/dev` 整棵前缀透传 | 必须排在路径翻译之前，否则 `/proc/cpuinfo` 之类全废 | [源码] + [实测] |
| `getcwd` 前缀裁剪 | 宿主真实路径裁成容器视图（`/data/.../workspaces` → `/ws`） | [实测] |
| bionic 直通 + cleaner 代跑 | 容器内 exec 一个 bionic 程序 → 交给无 filter 的 cleaner 执行 | [实测] GPU 桥就靠它 |
| 老式 syscall 改写 | x86_64 保留 15 个无 dirfd 的 `open/stat/lstat/...`，全部改写成 `*at` | [实测] |
| 两级探针诊断 | 先验骨架（最简 `_start`+raw6）再验业务，避免在复杂程序里瞎猜 | [实测] |
| **AF_UNIX socket 路径翻译** | `bind`/`connect` 的 `sockaddr_un.sun_path` 也翻译；缺任一层则所有 AF_UNIX 服务（wayland/X11/dbus/pulseaudio）起不来 | [实测] `unix_socket_probe` 三目录全 OK；weston/VNC 之所以能起全靠它 |
| **容器内跑 Wayland compositor** | weston 14.0.2 headless + pixman 在容器内运行，真实客户端建出 `xdg_toplevel` 顶层窗口 | [实测] `weston-debug scene-graph` 有 `top-level window 'simple-shm'` 250×250 居中 |
| **realpath / inotify 路径语义** | musl `realpath()` 走 `readlink("/proc/self/fd/N")`，`/proc` 透传宿主导致全失败；`inotify_add_watch` 无 `*at` 变体 | [实测] 缺任一条 → dsh 引擎 `startup failed`、7 个服务 missing |

## 三、我们明确不如 TAWC 的地方

诚实列清楚，不然对比表没有可信度：

1. **图形栈（差距已缩小，但仍存在）**：
   - **已有**：容器内 EGL/GLES 硬件渲染 + 画面回传 App（`lib/dsh_gl` + `LinuxFrameSurface`）；
     容器内 **weston headless compositor 能跑**，真实 Wayland 客户端能建出
     `xdg_toplevel` 顶层窗口（`[实测]` scene-graph 有 `top-level window 'simple-shm'`）。
   - **仍缺**：① **硬件合成** —— headless 后端走 `pixman`（CPU），没有 EGL/GBM 路径，
     真机 GPU 并未参与 compositor 合成；② **系统窗口（SurfaceFlinger layer）** ——
     需要 app Binder，免 root 拿不到；③ **从 compositor 抓帧** —— weston 14 的
     `weston_capture_v1` 只授权它自己的 RDP 客户端，`grim` 要的
     `wlr-screencopy` weston 不提供；④ **XWayland** —— `link()` 被 SELinux 拒，
     无 root 时 X11 路线不通（这也是我们选 wayland 而非 X11 的实际原因）。
   TAWC 用 Smithay + libhybris fork 解决了 ②③④。**差距在最难的两块上**。
2. **发行版生态**：TAWC 用 Arch/Debian（glibc），能直接跑大量预编译程序。
   Alpine 是 musl，很多二进制要重编。
3. **输入法/桌面集成**：TAWC 有 Android 键盘桥接、任务管理器、桌面快捷方式。
   我们只有容器 + 画面。
4. **社区验证**：TAWC 有真实用户与 issue 沉淀。我们的 Linux 模式还没发给用户。

## 四、还没验证、不要写进宣传的

- **phantom process killer 不会杀我们的容器** —— 未验证。`run-as` 测的 124 进程
  存活是假证据（进程挂 adbd 名下、cgroup 是 `/`，不在 App 进程树里）。
  必须真机走 UI 路径验证。
- **真机 Adreno 840 的帧率** —— 未测。上表全部数字来自模拟器 SwiftShader。
- **冷启动 1.9s → 1.0s** —— 未达成。Tier0 已省 140ms，Node 24 / 堆参数 /
  依赖缓存三条路实测否决。**容器内引擎首测 5145ms 但原生基线未取到**
  （`TMPDIR` 权限问题），这个数字不能与 1.9s 直接比。
- **容器内 GUI 窗口的画面能否进 App** —— 未验证。compositor 能开窗口（scene-graph
  有 `xdg_toplevel`），但 weston 14 的 capture 授权挡住抓帧。产品路径走的是
  `lib/dsh_gl` 共享文件（与 compositor 无关），那条已单独验证。
- **容器内装大包（weston/nodejs）的可行性** —— 模拟器上极慢且会被
  ActivityManager 杀掉（`Killing ... app.dsh.mobile (adj 0): stop app.dsh.mobile`），
  未能在干净环境完成端到端装包。这是模拟器 + 网络的限制，不是实现缺陷的定论。

## 五、一句话概括

**同一条架构路线上的另一个实现。** 我们多出的是「免 root musl 的 DNS 改写」、
「双架构」、「App 内嵌共享数据」、「AF_UNIX + compositor 能跑」这些点；
TAWC 多出的是「系统窗口 + 硬件合成 + glibc 生态」这三块大而硬的点。

**边界变化（本轮更新）**：图形栈的差距从「我们没有 compositor」缩小到
「我们有 compositor 但不接硬件合成、也不进 SurfaceFlinger」。**壁垒没有建立，
但第一道门槛已经跨过** —— 容器内能跑 GUI 程序这件事，从「不能」变成了「能」。
