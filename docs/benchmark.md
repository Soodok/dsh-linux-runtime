# 性能数据与测量方法

## 一、测量纪律

这几条都是踩出来的，**这个项目踩过最大的坑不是实现，是测错了方向**。

1. **在设备内计时**。adb 往返的协议开销会淹没毫秒级差异。
2. **不要用 shell 循环计时**。`/proc/uptime` 精度受 `HZ=100` 限制（10 ms 粒度）；
   容器内 busybox 的 `date` **不支持 `%N`**（`date +%s%N` 只输出秒）。
   正确做法：批 N 次求总时再平均，或程序内 `clock_gettime(CLOCK_MONOTONIC)`。
3. **基线必须验证「真的跑成功」**。曾用「宿主直接跑 `cd $R && ./bin/busybox`」
   当基线得 11 ms，宣称「慢 7~9 倍」—— 那个 baseline 是假的：bionic 链接器跑不了
   musl 二进制，11 ms 只是失败命令的耗时。
4. **异常好的结果先当可疑**。首次测 PRoot 启动得 2~3 ms，差点当成「PRoot 更快」，
   加退出码检查发现全是 `rc=127`（exec 失败）。
5. **交替轮次取中位/最优**，不要所有 A 跑完再所有 B —— 设备会热、会限频。
6. **杀净旧进程**。`pidof app.dsh.mobile` 杀不死，需反复 `kill -9` 到 `ps` 为空；
   端口判就绪只认 `LISTEN` 且核对监听者 PID（`netstat | grep 3080` 会匹配到
   TIME_WAIT 残留行，给出假数据）。

## 二、基准程序

[`bench/bench_full.c`](../bench/bench_full.c)：7 维，程序内自计时，输出 `key=value`。

| 维度 | 内容 |
|---|---|
| `exec` | `fork` + `execl("/bin/true")` + `waitpid`，一轮 |
| `io` | `open` + `read` + `close` 同一文件 |
| `stat` | `stat("/etc/passwd")` |
| `getpid` | 纯 syscall，不经路径翻译 |
| `cpu` | 2000 万次浮点除法累加（零 syscall） |
| `dir` | `readdir` 遍历 `/usr/bin` |
| `write` | `open(O_TRUNC)` + `write` + `close` |

每一维都输出 `*_fail` 计数；**fail 必须为 0 才采信该轮**。

## 三、与 PRoot 的逐项对比

条件：同设备、同 rootfs、同脚本、`rounds=500`、3 轮取中位。

| 指标 | dsh_alpine | PRoot 5.1.107 | 倍率 |
|---|---:|---:|---:|
| `exec` | **1.46 ms** | 2.76 ms | 1.89× |
| `io` | **0.009 ms** | 0.19 ms | 21× |
| `stat` | **0.0037 ms** | 0.10 ms | 27× |
| `write` | **0.0089 ms** | 0.22 ms | 25× |
| `dir`（2025 项） | **0.082 ms** | 0.56 ms | 6.8× |
| `getpid` | 0.00023 ms | 0.00046 ms | 2× |
| `cpu` | 0.0045 ms | 0.0075~0.16 ms | 1.7~35× |

所有 `*_fail` = 0，`dir_entries` 双方一致（1012500 / 2025000）。

> `exec` 一项受系统负载影响明显（同一实现跨轮次可差 2×）。上表取 3 轮中位；
> 交替跑 6 轮时 dsh_alpine 有 5 轮胜出。

## 四、三方对照（含 root 基线）

### 4.1 启动耗时

绝对路径、`rc=0`、3 轮取最优：

| 程序 | 真 chroot（root） | dsh_alpine（非 root） | PRoot（非 root） |
|---|---:|---:|---:|
| `python3 -c pass` | 18.1 ms | **17.1 ms** | 43.8 ms |
| `node -e 0` | 44.3 ms | 111.5 ms | 44.9 ms |
| `git --version` | 1.1 ms | 6.8 ms | 2.3 ms |
| `gcc --version` | 1.1 ms | 6.8 ms | 2.3 ms |
| `sqlite3` | 1.3 ms | 7.4 ms | 3.1 ms |
| `cmake --version` | 10.2 ms | 31.2 ms | 31.1 ms |

### 4.2 文件 IO / syscall

2 万次 `open+read+close` / 20 万次 `getpid`：

| 组 | 文件 IO | syscall |
|---|---:|---:|
| 真 chroot（root） | 62 ms | 32 ms |
| **dsh_alpine（非 root）** | **116~164 ms** | **34~35 ms** |
| PRoot（非 root） | **4104~4920 ms** | 91~96 ms |

→ 文件 IO 比 PRoot 快约 **30 倍**，syscall 快 **2.7×**，CPU 运算持平。

### 4.3 单次 exec 的优化过程

| 阶段 | 单次 exec | 说明 |
|---|---:|---|
| 初始版本 | 11.92 ms | interp 每次重读 723 KB + guest ELF `read`+`memcpy` + 8 MB 栈 |
| + interp 缓存 | 10.19 ms | musl ld 内容不变，首次读入后复用 |
| + guest ELF 文件映射 | 8.15 ms | `PROT_READ|MAP_PRIVATE` 直接映射，去掉 memcpy |
| + 栈 8 MB→2 MB | **4.72 ms** | 实测 guest 极少用超 2 MB |
| 真 chroot 基线 | 0.73 ms | 无任何拦截 |

> 进一步尝试「execve 不 fork、进程内 reload」能降到 3.64 ms，但会破坏多进程管道
> （`ABC|gzip|zcat` 全部段错误），**不采用**。栈降到 2 MB 后小数据管道偶发失败，
> 实际部署保留 4~8 MB。

分阶段打点（打点版）：

| 阶段 | 耗时 |
|---|---|
| interp 装载（723 KB） | 906~2094 µs ← **总开销的 70%** |
| guest ELF 装载（919 KB） | 230~530 µs |
| 栈 `mmap`（2 MB） | 75~111 µs |
| 其余（handler 往返 / 路径翻译 / fork） | 约 7 ms（未细分） |

## 五、真实负载

| 负载 | dsh_alpine | PRoot | 倍率 |
|---|---:|---:|---:|
| SQLite 3.54.1 `-O2` 编译（271131 行） | **38.6 s** | 49.2 s | 1.27× |
| SQLite `-O0` 编译 | 8.23 s | 8.24 s | 持平 |
| SQLite 全量链接 | **47.7 s** | 51.3 s | 1.08× |
| 30 个 `.c` 编译 + 链接 | **1.27~1.40 s** | 2.50~4.23 s | 1.8~3.3× |
| `git` 40 次 commit | **1.17 s** | 3.20 s | 2.7× |
| `tar` 解压 3.9 MB | **0.20 s** | 0.76 s | 3.8× |
| `tar` 打包 300 文件 | **0.05 s** | 0.23 s | 4.6× |
| `cp` 300 文件 | **0.65 s** | 1.80 s | 2.8× |

**产物一致性**：SQLite 编译产物 sha256 与 chroot 侧完全相同
（`-O0` → `7a6bace197c4`，`-O2` → `515bba8fdb83`）。

## 六、内存

| 对象 | VmRSS |
|---|---:|
| 容器 loader 进程本身 | **1836 kB** |
| 宿主原生 `sh` | 5396 kB |
| 容器内 Python 启动 | 8172 kB（chroot 7224 kB） |
| 容器内 Python + numpy 400×400 | 36836 kB（chroot 34288 kB，+7.4%） |

- 分配 200 MB 大内存：容器 212844 kB vs chroot 212024 kB，**差 +820 kB，无放大**
- `VmSize` 显示 2 GB+ 是固定基址 `0x7000000000` 的保留映射，**不是实占**

## 七、硬件 GPU

同一程序同参数（10000 三角形 × 20 帧 @ 1024²）：

| 后端 | 总计 | 每帧 |
|---|---:|---:|
| Adreno 840（硬件，经 bionic 桥） | **14.9 ms** | **0.74~1.76 ms** |
| llvmpipe（软件，mesa） | 525.9 ms | 26.3~30.5 ms |

差 **35×**。`glReadPixels` 1024²×10：硬件 1.4~2.2 ms（1792~2862 MB/s）。

## 八、复现

```bash
# 1. 推基准源码进 rootfs
cp bench/bench_full.c /data/data/<pkg>/files/linux/distros/alpine/tmp/

# 2. 容器内编译
dsh_alpine --dsh-reexec-v1 --rootfs $R --interp /lib/ld-musl-aarch64.so.1 \
  --guest /usr/bin/gcc /usr/bin/gcc -O2 -o /tmp/bench_full /tmp/bench_full.c

# 3. 跑（rounds 越大越稳，500 约需 1 分钟）
dsh_alpine --dsh-reexec-v1 --rootfs $R --interp /lib/ld-musl-aarch64.so.1 \
  --guest /tmp/bench_full /tmp/bench_full 500

# 4. PRoot 侧同参数跑同一份二进制，交替 3~5 轮取中位
```

---

## 六、容器内硬件 GPU 通路（2026-11-06 实测）

### 6.1 结论先行

在**免 root** 的 musl 容器里，bionic 图形程序能用上**宿主 GPU**，并完成真实
渲染（不只是枚举设备）：

| 项 | 实测值 | 环境 |
| --- | --- | --- |
| GL_RENDERER | `Android Emulator OpenGL ES Translator (Google SwiftShader)` | 模拟器 dsh34 / x86_64 / API 34 |
| EGL | 1.4 Android META-EGL，vendor=Android | 同上 |
| GL_VERSION | OpenGL ES 3.0 (SwiftShader 4.0.0.1) | 同上 |
| 1080p 清屏（含 glFinish） | **1.125 ms/帧** | 60 帧 + 10 帧预热 |
| 拾取/回读正确性 | 中心像素 RGBA = `255,89,26,255`（对齐 `glClearColor(1.0,0.35,0.1)`） | 同上 |

真机上对应的是 `Adreno (TM) 840`（历史实测，见 mechanism.md 的 cleaner 一节），
模拟器上则是 SwANGLE/SwiftShader。**两者走的是同一条通路**，只换后端实现。

### 6.2 为什么这条路能成立

musl 装不了 bionic 的 `libc.so` / `linker64`（实测 `__register_atfork` 符号
缺失），所以容器内**不能**直接 `dlopen libEGL.so`。唯一出路是「让一个不带
seccomp filter 的进程去 exec bionic 程序」——也就是 cleaner 机制：

1. `dsh_alpine` 在第一次 `install_filter()` **之前** fork 出 cleaner
   （此刻进程干净，`PR_GET_SECCOMP=0`）；
2. 容器内任何 `execve` 打到 bionic ELF（`PT_INTERP` 含 `linker64`）时，
   handler 不翻译路径、不装载，改由 cleaner 代跑宿主真实路径；
3. cleaner 是 bionic 环境，`dlopen("libEGL.so")` 直接成功。

**必须经 `sh -c` 触发**：直接 `dsh_alpine ... /gpu_probe` 会先按 musl 装载，
报 `__libc_init: symbol not found`；只有容器内再发起一次 `execve` 才命中直通分支。

### 6.3 帧回传成本分解（决定「能不能开窗口」）

免 root 时容器侧建不了 SurfaceFlinger layer（需要 app Binder 权限）。可行的
窗口方案只能是「容器离屏渲染 → 回读 → 交给 App 的 SurfaceView 贴图」。实测
这条链路里容器侧那一段：

| 分辨率 | render+finish | glReadPixels | write | 合计 | 等效 fps |
| --- | --- | --- | --- | --- | --- |
| 1280×720 | 0.455 ms | 2.705 ms | 0.008 ms | **3.183 ms** | 314 |
| 1920×1080 | 1.125 ms | 5.630 ms | 0.014 ms | **6.777 ms** | 148 |

**`glReadPixels` 是唯一瓶颈**（占 83%），因为它强制同步等待 GPU 完成并回拷。
即便如此，1080p 仍有 148 fps 的理论上限，是 60fps 门槛的 2.4 倍——说明
「回读 + App 贴图」这条路在性能上可用，不需要自建 compositor 才能开窗口。

真正的代价在 App 侧的每帧贴图与格式转换（未测），以及回读带来的输入延迟。

### 6.4 与自建 compositor（TAWC 路线）的取舍

tawcroot 走的是「Smithay compositor + libhybris fork」：容器内直接建 Wayland
surface，硬件合成，零回读。代价是要维护 aarch64 TLS thunk patcher、bionic
TLS/IE-TLSDESC 兼容、AHB gralloc backend、Wayland EGL 平台适配（含
`NATIVE_WINDOW_BUFFER_AGE=0`）等几十个补丁。

本方案用「回读 + App 贴图」绕过全部这些：不需要 AHB、不需要 gralloc、
不需要 TLS thunk（因为根本不在 musl 里加载 bionic 的 GL 库）。代价是
6.8ms/帧的回读开销与一帧延迟。

**两条路的性能差距在 1080p 下不足以否定后者**（148 fps vs 屏幕 60/120Hz）。
对「手机里有个能跑图形程序的 Linux」这个产品形态，回读方案是更短的路径。

### 6.5 端到端窗口路径（容器渲染 → memfd → 宿主可取）

免 root 建不了 SurfaceFlinger layer，所以 App 侧只能「取像素 → SurfaceView
贴图」。这条路的另一半是**回读出来怎么零拷贝交给宿主**。实测走 memfd：

| 项 | 实测值 |
| --- | --- |
| `memfd_create` | 可用（Android 无 `/dev/shm`，`shm_open` 不可用，必须裸 syscall） |
| 整帧写入（1080p / 7.91 MB） | **0.362 ms/frame**（2763 fps） |
| `msync(MS_SYNC)` | 0.001 ms/次 |
| `SCM_RIGHTS` 传 fd（往返） | **0.0111 ms/次** |
| 端到端（720p：render + 回读进 memfd） | **3.325 ms/frame → 301 fps** |
| 渲染内容正确性 | 落 PPM 后中心像素 RGB = `255,89,26`，整帧视觉确认纯橙 |

**整条窗口路径的容器侧总成本**（1080p）：渲染 1.1~1.4ms + 回读 5.3~6.0ms
= 6.4~7.3 ms/帧。IPC 那部分（0.36ms 写入 + 0.011ms 传 fd）相比之下可忽略。

### 6.6 三个必须知道的坑

1. **必须经 `sh -c` 触发**。直调 `dsh_alpine <rootfs> <interp> /gpu_probe` 会按
   musl 装载，报 `__libc_init: symbol not found`；只有容器内再发起一次 `execve`
   才命中 bionic 直通分支。
2. **不能静态链接 libEGL**。`-lEGL` 会在 bionic 非 zygote 进程里触发
   `FORTIFY: pthread_mutex_lock called on a destroyed mutex` 直接 abort。
   必须 `dlopen` + `dlsym`；`printf` 同样要走裸 `write()`。
3. **输出文件路径必须显式传**。程序跑在 cleaner（bionic 环境）里，
   bionic 的 `/tmp` 与容器内的 `/tmp` 不是同一个东西且通常不可写，
   `fopen("/tmp/...")` 会静默失败（连文件都不出现）。

### 6.7 跨进程帧共享：必须用普通文件，不能用 memfd（实测踩过）

容器渲染出的帧要交给 App 贴图，中间那块内存怎么共享是关键。实测结论：

| 方案 | 结果 |
| --- | --- |
| `memfd` + 宿主 `open("/proc/<pid>/fd/<n>")` | **不行**。只有 root 能 open，同 uid 的 `run-as` 进程被 SELinux 拦成 `Permission denied`（root 读 921600 B 正常，run-as 读报拒绝） |
| `SCM_RIGHTS` 传 fd | 成本极低（0.011 ms），但需要宿主侧先建立 unix socket 连接，多一层生命周期管理 |
| **普通文件 + 双方各自 mmap** | **可行**，零 root、零 Binder、零额外进程 |

采用第三种：容器侧 `open(sharepath, O_RDWR|O_CREAT)` + `ftruncate` +
`mmap(MAP_SHARED)`，`glReadPixels` 直接写进映射；宿主 `mmap` 同一文件即得
RGBA 像素。共享文件必须放在 **App 可写目录**（`/data/data/<pkg>/files/`），
不能放 rootfs 里 —— 那样宿主看不到。

端到端实测（640×360 / SwiftShader / 16ms 间隔）：

```
DSH_FRAME pid=19271 w=640 h=360 fmt=rgba8888
          path=/data/data/app.dsh.mobile/files/dshframe.bin
          renderer=Android Emulator OpenGL ES Translator (Google SwiftShader)
共享文件 921600 B = 640*360*4 ✓
宿主读回中心像素 RGBA = 87,168,128,255（整帧渐变，视觉确认正确）
稳定 32.8 fps（16ms 间隔的调度上限）
```

`framesrv.c` 就是这套方案的可运行实现，首行输出 `DSH_FRAME` 握手行供宿主
解析尺寸与路径。

### 6.8 至此「免 root 图形栈」的完整能力边界

| 能力 | 状态 |
| --- | --- |
| 容器内 bionic 程序用宿主 GPU 渲染 | ✅ 实测 1080p 1.1~1.4 ms/帧 |
| 像素回读 | ✅ 5.3~6.0 ms/帧（1080p） |
| 跨进程交给宿主 | ✅ 普通文件 + mmap，零拷贝 |
| 全链路帧率上限 | ✅ 1080p 137~156 fps / 720p 300+ fps |
| 直接建系统窗口（SurfaceFlinger layer） | ❌ 需 app Binder，免 root 拿不到 |
| 硬件合成（AHB/gralloc 零回读） | ❌ 需 libhybris，未实现 |

结论：**「容器里跑图形程序并把画面显示在 App 里」这条路已经通**，缺的只是
App 侧的 SurfaceView 贴图代码；而「容器程序像原生 App 一样拥有自己的系统
窗口、硬件合成」仍然需要 compositor + libhybris，那是另一条路线。

### 6.9 tri 模式：证明 GL 管线是完整的，不只是能清屏

`framesrv` 第六个参数传 `tri` 会走真正的几何渲染路径：

```
/framesrv 1280 720 0 16 <共享文件> tri
```

它做的事：编译顶点/片元着色器 → `glGenBuffers` + `glBufferData` 上传顶点
→ 每帧 `glDrawArrays(GL_TRIANGLES)` 画一个绕 Z 轴旋转的三角形 → `glFinish`
→ `glReadPixels` 写共享文件。

实测输出（模拟器 x86_64 / 480x270）：

```
tri mode: prog=0x3 vbo=1
done frames=30 elapsed=1441ms fps=20.8
```

回读的帧里三角形区域是纯黄 `(255,242,51)`，清屏区是深绿 —— 视觉确认
着色器、顶点缓冲、draw call 全部生效。

**★这一步必须做，不能只验 `glClear`**：清屏能出像素只证明 EGL 上下文
与回读通路通了，完全不能证明「着色器能编译、VBO 能上传、draw call 能
光栅化」。踩到的具体坑：`vbo` 初值 0 是 GL 的「默认缓冲」，对它
`glBufferData` 是 `INVALID_OPERATION`（静默失败，不报错），顶点数据根本
不会上传，表现就是「program 链接成功、帧里只有清屏色」。必须
`glGenBuffers(1, &vbo)` 先拿到名字。

### 6.10 全链路帧率基准（多分辨率 × 多轮取中位）

`bench/bench_frame.py` 是这套基准的可复现脚本：每轮独立起帧服务、跑固定帧数、
读回共享文件核对尺寸，三轮取中位数。测的是**完整链路**——容器渲染 →
`glReadPixels` → 共享文件 → 宿主读回。

模拟器 dsh34（x86_64 / SwiftShader 软件渲染 / `interval=0` 不限速）：

| 分辨率 | 中位 fps | 各轮实测 | 每帧耗时 |
| --- | --- | --- | --- |
| 640×360 | **1405.8** | 1170.9 / 1459.1 / 1405.8 | 0.71 ms |
| 1280×720 | **324.6** | 340.8 / 309.0 / 324.6 | 3.08 ms |
| 1920×1080 | **150.1** | 113.7 / 151.0 / 150.1 | 6.66 ms |

三点值得注意：

1. **1080p 组离散较大**（113.7~151.0）。这一档 `glReadPixels` 要搬 8.3 MB，
   已经撞到 SwiftShader 的内存带宽上限，调度抖动被放大。720p 与 360p 组
   离散就小得多。
2. **这是软件渲染的数字**。真机 Adreno 840 的 `glReadPixels` 通常比
   SwiftShader 快数倍，所以真机上 1080p 很可能跑到 300 fps 以上。真机数据
   待补（本轮手机离线）。
3. **`glReadPixels` 仍是唯一瓶颈**，占 1080p 每帧 6.66 ms 中的约 5.6 ms。
   想再快只有两条路：硬件合成（AHB/gralloc 零回读，需 libhybris）或降分辨率。
   在当前「免 root + 无 libhybris」的前提下，150 fps @ 1080p 已经是这条
   路线的上限。

### 6.11 显示端（App 侧贴图）实测与瓶颈定位

容器 → 共享文件这一段能跑 150 fps @1080p（6.10），但**贴到屏幕上**是另一段，
必须单独量。App 侧 `LinuxFrameSurface` 每 2 秒打一行分段日志：

```
display fps=30.5 shown=133 lastSeq=13956 size=1280x720 pull=1.4ms draw=0.7ms
```

模拟器 dsh34 / x86_64 / SwiftShader / 720p：

| 阶段 | lockCanvas（改前） | lockHardwareCanvas（改后） |
| --- | --- | --- |
| pull（mmap→Bitmap） | 1.4 ms | 1.3 ms |
| draw（Canvas→Surface） | **12.7 ms** | **0.4~0.8 ms** |
| 显示帧率 | 29~30 fps | 28~31 fps（同） |

**结论一：软件 Canvas 是显示端的头号瓶颈。** `lockCanvas()` 拿的是软件
Canvas，`drawBitmap` 走 CPU 逐像素合成，720p 一次 12.7 ms；换
`lockHardwareCanvas()` 交给 GPU 合成后降到 0.4~0.8 ms，**20 倍**。

**结论二：剩下的 ~30 fps 不是我们的代码。** 对照实验——把 `drawBitmap`
整个去掉、只留 `drawColor(BLACK)`，帧率**仍是 30 fps**。所以那个上限来自
模拟器的 vsync 调度（该 AVD 的 `renderFrameRate` 报 60，但实际只给到 30
个回调），不是绘制开销。真机 60/120Hz 屏上这个上限会相应提高。

**结论三：字节序错了会让脏帧检测完全失效。** `ByteBuffer` 默认
`BIG_ENDIAN`，而容器（aarch64/x86_64 都是小端）按小端写 seq。不设
`order(LITTLE_ENDIAN)` 时 `getInt(4)` 读出字节反转的垃圾值——实测打出
`lastSeq=-1475936256`、`1057488896`、`-418906112` 这类乱数，于是**每帧都被
判成新帧**，重复帧也要白拷一次整帧（1080p 是 8.3 MB）。修正后 lastSeq
变成正常单调递增（6002 → 6719）。

## 7. 路径语义三件套（realpath / inotify / getcwd）

这三条 syscall 都不在最初的 BPF 树里，症状全都表现为「某个插件起不来」，
离根因极远。一次全跑耗时 < 20ms，建议每次改 path 相关代码后跑一遍。

```sh
# 编译（x86_64 示例；aarch64 换 toolchain 前缀即可）
x86_64-linux-android28-clang -O1 -static -o path_probe bench/path_probe.c

# 跑（必须经 sh -c，直调会按 musl 装载）
dsh_alpine -m /dsh:<home> -m /eng:<engine> <rootfs> \
  /lib/ld-musl-x86_64.so.1 /bin/sh -c '/path_probe'
```

实测（模拟器 dsh34 / Android 14 / x86_64）：

```
--- realpath ---
realpath(/) = /        realpath(/tmp) = /tmp     realpath(/dsh) = /dsh
realpath(/eng) = /eng  realpath(/ws) = /ws
--- inotify ---
inotify_add_watch(/) = OK     inotify_add_watch(/tmp) = OK
inotify_add_watch(/dsh) = OK  inotify_add_watch(/eng) = OK
inotify_add_watch(/ws) = OK
--- getcwd ---
chdir(/) -> getcwd = /          chdir(/tmp) -> getcwd = /tmp
chdir(/dsh) -> getcwd = /dsh    chdir(/eng) -> getcwd = /eng
chdir(/ws) -> getcwd = /dsh/workspaces

RESULT: PATH_PROBE_OK (fails=0)
```

> `chdir(/ws)` 回 `/dsh/workspaces` 不是 bug：`/ws` 与 `/dsh/workspaces` 是
> 同一个宿主目录的两条 bind 名，`host_to_guest` 按最长宿主前缀反查，两条都
> 合法。两者都能访问到同一个目录。

### 修复前后的端到端差异

| | 修复前 | 修复后 |
|---|---|---|
| `realpath("/dsh")` | ENOENT | `/dsh` |
| `fs.watch("/dsh/profiles/web")` | ENOENT → 回退到 `/` → EACCES | OK |
| 引擎启动 | `startup failed: 2 required plugins did not activate` | 只有一行 `dsh web:` |
| profile 健康 | 7 个服务 missing | 全绿 |
| 首页 | 无 | HTTP 200 / 46437 B / 70 个客户端插件 |

## 8. AF_UNIX socket（compositor / X11 / dbus 的前置条件）

`bind` 与 `connect` 的 `sockaddr_un.sun_path` 都要翻译，缺一层就会出现
「服务端能起、客户端连不上」。探针在同一进程内自 bind 自 connect 并传一个
字节，一次覆盖两层。

```sh
x86_64-linux-android28-clang -O2 -static -o unix_socket_probe bench/unix_socket_probe.c
dsh_alpine -m /dsh:<home> <rootfs> /lib/ld-musl-x86_64.so.1 /bin/sh -c '/unix_socket_probe /tmp /dsh /var/tmp'
```

实测（模拟器 dsh34 / Android 14 / x86_64）：

```
/tmp                     bind+connect+rw OK
/dsh                     bind+connect+rw OK
/var/tmp                 bind+connect+rw OK

RESULT: UNIX_SOCKET_OK (fails=0)
```

### 端到端：weston 14.0.2 headless compositor

| 阶段 | 修复前 | 修复后 |
|---|---|---|
| `bind` socket | `fatal: failed to add socket: No such file or directory` | `/tmp/wl/test` 建出 `srwxrwxrwx` |
| weston 启动 | 立刻退出 | 4 个进程驻留（weston + keyboard + desktop-shell） |
| `weston-simple-shm` | — | 存活（持续重绘），**不再 failed to create display** |
| `grim` 截图 | `failed to create display` | `compositor doesn't support wlr-screencopy-unstable-v1`（协议不支持，但**已连上**） |

> grim 那条错误是「连上了但 compositor 没实现该协议」，与「连不上」是两回事。
> weston 的 headless 后端不提供 `wlr-screencopy`，需要换成 weston 自己的
> `weston-screenshooter`（它走 `weston_capture_v1`）；实测该工具在无窗口内容
> 时报 `Assertion failed: width > 0`，需先有客户端 surface 输出。

weston 相关 Alpine 包（已实测可装）：`weston`、`weston-backend-headless`、
`weston-clients`、`weston-shell-desktop`、`grim`。

## 9. 容器内 Wayland GUI 窗口（compositor + 真实客户端）

前置：AF_UNIX 路径翻译（第 8 节）+ `--renderer=pixman`（见 pitfalls #36）。

```sh
# 1) 装（Alpine，已实测可装）
apk add weston weston-backend-headless weston-clients weston-shell-desktop

# 2) 起 compositor（headless + pixman，不开 X11、不需要 link()）
mkdir -p /tmp/wl && chmod 700 /tmp/wl
weston --backend=headless-backend.so --renderer=pixman        --socket=/tmp/wl/test --width=640 --height=360 --debug --log=/tmp/weston.log &

# 3) 起一个真实客户端
export WAYLAND_DISPLAY=/tmp/wl/test
weston-simple-shm &

# 4) 看合成结果
weston-debug scene-graph
```

实测（模拟器 dsh34 / Android 14 / x86_64）：

```
[weston.log] Using Pixman renderer
[weston.log] Output 'headless' enabled with head(s) headless
[weston.log] Loading module '/usr/lib/weston/desktop-shell.so'
[weston.log] launching '/usr/libexec/weston-desktop-shell'
[weston.log] launching '/usr/libexec/weston-keyboard'

[fs] /tmp/wl/test        srwxrwxrwx
[fs] /tmp/wl/test.lock

[scene-graph]
	View 0 (role xdg_toplevel, PID 29355, surface ID 3,
	        top-level window 'simple-shm' of org.freedesktop.weston.simple-shm):
		position: (307, 79) -> (557, 329)
		SHM buffer
			format: 0x34325241 ARGB8888
			modifier: LINEAR (0x0)
	View 0 (role (null), PID 29351, surface ID 16,
	        background for output headless):
		position: (0, 0) -> (640, 360)
```

**这是「容器内能开 GUI 窗口」的直接证据**：`xdg_toplevel` 角色的顶层窗口、
250×250 居中、带 panel 与 background，全部由容器内的 weston 合成。

### 6.9 `lib/dsh_gl` 库层三路径（rpix / ahb / auto）

产线 `framesrv.c` 好，不等于**库**好。`lib/dsh_gl` 是给容器内任意 GL 程序用的，
它支持 AHB 才算「容器图形栈补齐」——否则每个程序都要各自复制四步链路。

探针：`lib/pixmode_probe.c`，输出 `RESULT: DSH_GL_PIXMODE_OK`。

```sh
R=/data/data/app.dsh.mobile/files/linux/distros/alpine
RUN=/data/data/app.dsh.mobile/files/linux/runner/dsh_alpine
# ★共享文件必须传宿主路径：bionic 经 cleaner 执行时按宿主根解析（pitfalls #41）
for m in rpix ahb auto; do
  $RUN $R /lib/ld-musl-x86_64.so.1 /bin/sh -c     "/dsh_gl_pixmode /data/local/tmp/g_$m.bin $m 1280 720 60"
done
cmp /data/local/tmp/g_rpix.bin /data/local/tmp/g_ahb.bin   # 必须 IDENTICAL
```

实测（模拟器 dsh34 / x86_64 / SwiftShader，各 60 帧）：

| 分辨率 | rpix | ahb | 倍数 |
|---|---|---|---|
| 640×360 | 761.4 fps | **1027.2 fps** | 1.35× |
| 1280×720 | 207.9 fps（三轮 181.0 / 207.9 / 239.7） | **324.0 fps**（310.7 / 354.5 / 324.0） | **1.56×** |
| 1920×1080 | 101.1 fps | **149.8 fps** | **1.48×** |

`req` 与 `actual` 的对应关系（这是设计的核心）：

| 传入 | 实际 | 说明 |
|---|---|---|
| `rpix` | 必为 `rpix` | 绝不会偷偷升到 ahb |
| `ahb` | `ahb` 或 `rpix` | gralloc 不支持时**安静退回**，返回值仍是 0 |
| `auto` | 同 `ahb` | 产线用这个 |

**像素逐字节一致**（`cmp` → `IDENTICAL`）：三条路径写出的共享文件完全相同，
「快」不是拿画面对不上换来的。

`stride` 实测 640 宽时为 640（无对齐余量），1024 宽时为 1024。真机上
1080p 常见 1088/1152，所以库必须按行拷，不能整块 `memcpy`。

### 6.10 容器内开真实 GUI 窗口（weston + xdg_toplevel）

判据是三层，缺一层都不算开成（只看 weston 进程存活会假通过，见 pitfalls #43）：

| 层 | 证据 |
|---|---|
| 1 | `wl_compositor` / `xdg_wm_base` / `wl_shm` 三个 global 都拿到 |
| 2 | `xdg_surface.configure` —— compositor 对开窗请求的正式回应 |
| 3 | `wl_surface_frame` 回调（`frame_callback >= 1`）—— 真的跑了一帧绘制 |

```sh
# 容器内一次性准备
apk add --no-interactive weston weston-shell-desktop weston-backend-headless
apk add --no-interactive wayland-dev wayland-protocols gcc musl-dev
sh /build_wlwin.sh /wlsrc

# 起 compositor 并开窗口
rm -rf /tmp/wl; mkdir -p /tmp/wl; chmod 700 /tmp/wl     # 必须 0700
XDG_RUNTIME_DIR=/tmp/wl weston --backend=headless-backend.so   --renderer=pixman --socket=wl-t1 --idle-time=0 &
sleep 5
cd /wlsrc && WAYLAND_DISPLAY=wl-t1 ./wlwin 3 320 200
```

实测输出（模拟器 dsh34 / x86_64 / Android 14）：

```
weston: Using Pixman renderer
        Output 'headless' enabled with head(s) headless
        launching '/usr/libexec/weston-desktop-shell'
wlwin : WLWIN_KV compositor=4
        WLWIN_KV xdg_wm_base=5
        WLWIN_KV shm=1
        WLWIN_KV configure=0x0
        WLWIN_KV committed=1 size=320x200
        WLWIN_KV frame_callback=1
        RESULT: WLWIN_OK
```

两轮连续跑都拿到 `frame_callback=1`，可重复。

### 6.10.1 wlshot 持续抓帧（--loop）：画面镜像而不是快照

App 侧 `LinuxFrameSurface` 的脏判据是共享文件头里的 **seq**，所以抓帧器必须
持续递增 seq，否则画面会卡在第一次映射到的那张图上（**不报任何错**，
看起来像「画面停住」）。见 pitfalls #46。

```sh
# 持续抓，8 fps，直到被杀
WAYLAND_DISPLAY=wayland-1 /wlsrc/wlshot     /data/data/<pkg>/files/dshframe.bin 5000 --loop 8
```

实测帧率（模拟器 dsh34 / x86_64 / 1280×720 / pixman）：

| 请求 | 4 秒内 seq 增量 | 实际帧率 |
|---|---|---|
| 2 fps | 8 | 2.0 |
| 5 fps | 19 | 4.75 |
| 10 fps | 37 | 9.25 |

节流准确。上限受 pixman 软件合成 + screencopy 往返限制。

**每帧都要新建 `zwlr_screencopy_frame_v1`**（协议规定一个 frame 对象只能
copy 一次），但 `wl_buffer` 可以复用。输出尺寸中途变化时直接退出报错，
不原地换 buffer —— 错位画面比失败难查。

### 6.11 图形栈的真实边界（llvmpipe vs 硬件）

`--renderer=gl` 能起，但拿到的是软件渲染：

```
GL version: OpenGL ES 3.2 Mesa 25.1.9
GL renderer: llvmpipe (LLVM 20.1.8, 256 bits)
warning: failed to query rendering device from EGL
failed to initialize allocator
```

根因：容器内 **没有 `/dev/dri`，也没有 `/dev/kgsl-3d0`**（模拟器上宿主本身
这两个设备都不存在，`ls /dev/dri` 与 `ls /dev/kgsl-3d0` 均为空）。

| 能力 | 状态 |
|---|---|
| 容器内开真实 GUI 窗口（compositor 合成、xdg_toplevel、frame 回调） | ✅ 实测 |
| 容器内 GL 渲染 + 像素送回 App（`lib/dsh_gl`，AHB） | ✅ 实测，1080p 1.48× |
| compositor 的 GL 合成走 GPU | ❌ 走 llvmpipe 软件渲染 |
| 免 root 硬件合成（AHB/gralloc 零回读） | ❌ 需 libhybris fork |

**不要把「窗口能开」说成「硬件加速窗口能开」** —— 这两件事差着一整条 libhybris。

### 6.12 容器合成画面 → App 共享文件（wlr-screencopy）

`weston_capture_v1` 有授权模型（只授予 screen-share 拉起的 RDP 客户端），
`grim` 在 weston 下报 unauthorized（#36）。改用 wlroots 系（sway）的
`wlr-screencopy`，无授权限制。

`bench/wlshot.c` 抓一帧后**直接写成 dsh_gl 共享文件格式**，App 侧
`LinuxFrameSurface` 零改动即可显示：

```sh
apk add --no-interactive wlr-protocols wayland-dev gcc musl-dev sway grim
sh /build_wlshot.sh /wlsrc

WLR_BACKENDS=headless WLR_RENDERER=pixman WAYLAND_DISPLAY=wayland-1   /wlsrc/wlshot /data/data/<pkg>/files/dshframe.bin 5000
```

实测（模拟器 dsh34 / x86_64）：

```
WLSHOT_KV output=1280x720
WLSHOT_KV format=0x1            ← WL_SHM_FORMAT_XRGB8888
WLSHOT_KV copied=1
WLSHOT_KV bytes=3686400
RESULT: WLSHOT_OK
```

共享文件 3686416 B = 16 头 + 1280×720×4；magic `0x4D415246`、seq=1。

**★XRGB8888 的第 4 字节是 X（未定义），不是 alpha。** 实测同一窗口内
alpha 全 0、窗口外全 255 —— 纯垃圾。App 侧 `ARGB_8888` +
`copyPixelsFromBuffer` 会当真读，alpha=0 的区域被当全透明 →
**用户看到一片空白且不报错**。转换时必须 `pix[i+3] = 255`，
不能抄源字节。修复前后同一帧渲染成 PNG：修复前窗口区是**纯白一块**，
修复后蓝→橙渐变完整可见。

这个坑查不出来是因为**所有结构性检查都通过**：字节数 ✓ magic ✓
seq ✓ 尺寸 ✓。必须逐像素采样 + 看图（#45）。

### 能力边界

| 项 | 状态 |
|---|---|
| compositor 在容器内运行 | ✅ 实测 |
| 真实 Wayland 客户端连上并建窗口 | ✅ 实测（scene-graph 有 xdg_toplevel） |
| 客户端持续重绘 | ✅ 实测（`weston-simple-shm` 存活） |
| 从 compositor 抓帧 | ❌ 未通（weston 14 的 `weston_capture_v1` 只授权 RDP 客户端；`grim` 要 `wlr-screencopy`，weston 不提供） |
| 硬件 GPU 合成 | ❌ 未做（`pixman` 是 CPU 渲染；headless 后端没有 EGL 路径） |
| X11 / Xvfb | ❌ 仍不可行（无 root 时 `link()` 被 SELinux 拒） |

**注意**：产品不需要 compositor 抓帧。容器内 GL 程序的画面走
`lib/dsh_gl` 的共享文件 + App 侧 `LinuxFrameSurface` mmap，那条路径与
compositor 无关，已单独验证过（第 7 节）。

## 10. 可复现的独立验证环境（不依赖 App）

前面的实测都在 App 内（debug 包 + `LinuxEnvironmentManager` 部署）跑。App 被
换包/被杀后无法重放。这一节给出**完全不依赖 App 的复现步骤**：用 `adb root`
在 `/data/local/tmp` 建一套等价环境，跑同样的探针。

```sh
T=/data/local/tmp/dshtest
adb root
adb shell "rm -rf $T; mkdir -p $T/rootfs $T/home/w $T/eng $T/dsh"
adb push app/src/main/jniLibs/x86_64/libdsh_alpine.so $T/dsh_alpine
adb push app/src/main/assets/linux/rootfs-bootstrap-x86_64.tgz $T/rt.tgz
adb shell "cd $T/rootfs && tar xzf ../rt.tgz && chmod 755 ../dsh_alpine"

# ★ 必须把绝对软链改成相对，且只碰 symlink（见 pitfalls #38）
adb shell "cd $T/rootfs && for d in bin sbin usr/bin usr/sbin; do
  [ -d \"\$d\" ] || continue
  for f in \"\$d\"/*; do
    [ -L \"\$f\" ] || continue
    t=\$(readlink \"\$f\")
    case \"\$t\" in
      /bin/busybox|*/bin/busybox)
        case \$d in
          usr/bin|usr/sbin) ln -sf ../../bin/busybox \"\$f\" ;;
          sbin)             ln -sf ../bin/busybox \"\$f\" ;;
          *)                ln -sf busybox \"\$f\" ;;
        esac ;;
    esac
  done
done"

# root 域可直接用真实外部 DNS（见 pitfalls #39）
adb shell "echo 'nameserver 223.5.5.5' > $T/rootfs/etc/resolv.conf"
```

跑探针（**必须带 `-m` 挂载，否则 `/dsh` `/eng` `/ws` 不存在，探针会假失败**）：

```sh
adb shell "$T/dsh_alpine -m /dsh:$T/dsh -m /eng:$T/eng -m /ws:$T/home/w     $T/rootfs /lib/ld-musl-x86_64.so.1 /bin/sh -c '/path_probe'"
```

实测输出（2026-11-06，模拟器 dsh34 / x86_64）：

```
--- realpath ---
realpath(/) = /        realpath(/tmp) = /tmp     realpath(/dsh) = /dsh
realpath(/eng) = /eng  realpath(/ws) = /ws
--- inotify ---
inotify_add_watch(/) = OK (wd=1)     inotify_add_watch(/tmp) = OK (wd=1)
inotify_add_watch(/dsh) = OK (wd=1)  inotify_add_watch(/eng) = OK (wd=1)
inotify_add_watch(/ws) = OK (wd=1)
--- getcwd ---
chdir(/) -> getcwd = /          chdir(/tmp) -> getcwd = /tmp
chdir(/dsh) -> getcwd = /dsh    chdir(/eng) -> getcwd = /eng
chdir(/ws) -> getcwd = /ws

RESULT: PATH_PROBE_OK (fails=0)
```

```
$ /unix_socket_probe /tmp /dsh /eng /ws /tmp/wl
/tmp                     bind+connect+rw OK
/dsh                     bind+connect+rw OK
/eng                     bind+connect+rw OK
/ws                      bind+connect+rw OK
/tmp/wl                  bind+connect+rw OK

RESULT: UNIX_SOCKET_OK (fails=0)
```

**意义**：这两条结论（路径语义修复、AF_UNIX 修复）从「App 内一次性实测」
升级为「可在独立环境重放的复现」，不再依赖 App 是否被并发会话换包。

### 这个环境装不了大包（如实记录）

`apk add weston`（110 包 / 458 MiB）与 `apk add nodejs`（14 包，含
`icu-data-full`）在模拟器上 **>15 分钟**仍未完成，最后进程被
`ActivityManager` 杀（`Killing ... adj 0`，不是 phantom killer，见 pitfalls #38）。
所以这个独立环境只适合跑**已编译好的小探针**，不适合装 GUI 栈或 node。

## 11. 容器层单进程开销 —— 冷启瓶颈到底在哪一层

冷启优化的前提是先量清楚「容器这层自己花多少」。如果它很贵，那优化容器；
如果它几乎免费，那所有努力都得放在 Node 引擎侧。

### 测法（`/proc/uptime` 差值法，避免单次测量被 HZ 粒度吃掉）

Android 内核 `HZ=100`，`/proc/uptime` 只有 10ms 粒度，单次测必然读到 0 或 10。
必须**批 N 次取平均**：

```sh
S=$(cut -d' ' -f1 /proc/uptime)
for i in $(seq 20); do
  <runner> -m /dsh:$T/dsh <rootfs> /lib/ld-musl-x86_64.so.1 /bin/sh -c 'true' >/dev/null 2>&1
done
E=$(cut -d' ' -f1 /proc/uptime)
awk "BEGIN{printf \"%.2f ms/run\", ($E-$S)*1000/20}"
```

### 实测（模拟器 dsh34 / Android 14 / x86_64，root 域）

```
20 runs: 4 ms/run
```

单次 `fork + execve + seccomp 装载 + SIGSYS 拦截 + busybox sh 启动 + 退出`
**约 4ms**。

单次 `date +%s%N` 包装的粗测（含两次 `date` 自身开销）：13 / 15 / 13 / 13 / 18 ms，
差值就是 `date` 的开销 —— 这也是「用外部时钟包内层」的固有误差，
所以上表用批平均法。

### 对冷启目标的意义

真机记录（同一台 OPPO PLZ110 / Android 16 / 内核 6.12.38，见 AI_CONTEXT 甲·五）
的端到端「从零 → 进 Linux → DSH 就绪」**中位 1.33 秒**，其中
**容器层仅占 0.8%（约 10ms），引擎插件加载占 96%**。本轮 4ms/进程 的独立测量
与那个结论方向一致：

> **容器层不是冷启瓶颈。** 把冷启从 1.9s 压到 1.0s 的空间几乎全在
> Node 引擎的模块加载与插件激活上，容器侧再优化也只有个位数毫秒可捡。

这也解释了为什么三条容器侧优化（Node 24、堆参数、依赖盘点缓存）实测都没有
正向收益 —— 它们动的是引擎侧，而引擎侧的瓶颈是 `ModuleWrap` 与插件激活，
不是容器拦截成本（实测 `(idle)` 只占 1.4%，`ModuleWrap` 占 13.8%）。

### 已知的测量污染（都踩过）

- **端口 TIME_WAIT**：连续测引擎启动必须每轮换端口 + `sleep 2`，否则
  `EADDRINUSE` 直接失败（实测过三轮全废）。
- **`pidof dsh_alpine` 杀不死**：重执行后进程名可能是 `dsh-alpine`，要两种都杀。
- **`/proc/uptime` 只有 10ms 粒度**：单次测量全 0，必须批平均。
- **toybox `date +%s%N` 不支持纳秒**：返回 0ms。
- **App 不在前台会被 ActivityManager 杀**（adj 0）：见 pitfalls #38。

## 12. 编译缓存实测省 1.1 秒（24%）—— 且之前那条 6197ms 是污染的

冷启优化里唯一还有正收益的一项。这轮做了干净的 A/B。

### 实验设置（模拟器 dsh34 / Android 14 / x86_64，bionic node v24.18.0）

- 同一份引擎、同一份 profile（只剩 `@deepseek-ai/dsh-base` + `dsh-web-app`）
- 同一 `NODE_COMPILE_CACHE` 目录、同一 `--require readyat.cjs`（在
  `console.log("dsh web: ...")` 那一刻读 `process.uptime()`）
- **每轮之间清场**：`kill -9` 所有 `node` / `dsh_alpine` / `dsh-alpine`，
  再 `sleep 3`；端口必须真空（否则 `EADDRINUSE` 直接失败）
- 无缓存态：每轮开始前 `rm -rf .node-compile-cache`

### 结果（各 3 轮）

| 状态 | 三轮 | 中位 |
|---|---|---|
| **缓存命中** | 3652.2 / 3416.8 / 3385.7 ms | **3416.8 ms** |
| **缓存清空** | 4540.2 / 4088.8 / 4528.4 ms | **4528.4 ms** |

**省 1111.6 ms / 24.5%。**

缓存落盘量：**15 MB**（`v24.18.0-x64-cf738c9d-0` 一个目录）。

### 关键前提：必须有 flusher，否则缓存等于不存在

`NODE_COMPILE_CACHE` 是**懒写盘**的 —— 实测不注入
`node-compile-cache-flusher.cjs` 时，跑完一整轮缓存目录只有 **16 KB**
（vs 注入后 15 MB）。原生启动路径早就注入了这个 flusher
（`EngineConfig.kt` 的 `COMPILE_CACHE_FLUSHER`），所以这条优化**在原生路径上
一直生效**；容器路径也注入了（`LinuxEngineLauncher.kt:256`）。

### 顺带纠正一条被污染的历史数字

之前记的「原生引擎 6197 ms」是**不可用**的：那一轮的 `TMPDIR` 指向一个不存在
的目录，`spill-local` 插件报

```
spill-local: Error: ENOENT: no such file or directory,
  mkdtemp '/data/data/app.dsh.mobile/files/dsh-home/tmp/dsh-spill-XXXXXX'
```

（`dsh: warning: 1 entry did not activate`），运行环境与生产不一致。
`mkdir -p $TMPDIR` 之后重测，无缓存态是 4089~4540 ms。

**教训**：`TMPDIR` 必须真实存在且可写，否则 `spill-local` 起不来，
整个测量不可比。

### 与 1.0 秒目标的距离

即使缓存全命中，**中位 3.42 秒**，距 1.0 秒还差 **2.4 秒**。
这条数据的作用是**收窄范围**：它证明还有 1.1 秒是「第一次编译模块」，
剩下 3.4 秒里必定是别的东西（插件激活 / ESM 解析 / 依赖图构建）。

下一步要砍的不是缓存，而是那 3.4 秒的构成。已知线索（前几轮 profile）：
`ModuleWrap` 占 13.8%、`GC` 81.5ms、`compileSourceTextModule` 62.2ms，
而 `(idle)` 只占 1.4% —— **说明不是等 IO，是真的在算**。

## 13. 冷启 3.4 秒的精确构成（--cpu-prof 截到就绪时刻）

### 一、先说一个被证伪的候选

`--no-warnings`：交替 4 轮，中位 3416.3 vs 基线 3402.1，**差 14.2 ms**，
两组数据完全重叠（A: 3285.8/3308.2/3496.1/3832.6，B: 3314.9/3406.3/3426.3/3881.6）。
**判定噪声，不采用。** 这条记下来是为了避免以后有人再试一次。

### 二、profile 怎么取（关键：必须按时间截到就绪点）

`--cpu-prof` 的 profile 覆盖**整个进程生命周期**。如果只是起引擎、等 20 秒、
再 `kill -TERM`，profile 里会包含就绪后的等待时间，`(idle)` 会虚高到 68%，
结论完全错。

正确做法：用 `timeDeltas` 累加到「`readyat` 那一刻」为止：

```python
READY_US = 4247 * 1000          # readyat 打印的 ms
cum = 0
sel = []
for i, sid in enumerate(profile['samples']):
    cum += profile['timeDeltas'][i]
    if cum <= READY_US:
        sel.append(sid)
```

这样得到 541 个样本（总 1795），覆盖 4247ms。

### 三、4247ms 归谁在算

| 归谁 | 占比 | 折算 |
|---|---|---|
| **node 内建（JS 层）** | **44.7%** | **1900 ms** |
| **dsh 插件代码** | **36.8%** | **1562 ms** |
| 第三方 `node_modules` | 11.8% | 502 ms |
| GC | 5.9% | 251 ms |
| idle/program | 0.7% | 31 ms |

**`idle` 只占 0.7%** —— 这是最重要的一条：**不是在等 IO，是真的在算。**

### 四、node 内建那 1900ms 的细分

| 模块 | 占比 | 折算 |
|---|---|---|
| `internal/modules/cjs/loader` | 5.7% | 243 ms |
| `node:fs` | 5.2% | 220 ms |
| `internal/modules/esm/utils` | 5.0% | 212 ms |
| `internal/bootstrap/realm` | 4.3% | 181 ms |
| `internal/modules/package_json_reader` | 3.5% | 149 ms |
| `internal/url` | 3.1% | 133 ms |
| `internal/modules/esm/resolve` | 2.2% | 94 ms |
| `internal/path` | 1.7% | 71 ms |

**模块解析基础设施合计约 1.1 秒**（loader + fs + esm/utils + package_json_reader
+ url + esm/resolve + path）。这些是「找文件、读 package.json、构造 URL」
的开销，与具体业务无关。

### 五、dsh 插件那 1562ms 的细分（TOP 6）

| 包 | 占比 | 折算 |
|---|---|---|
| **`cordis`（依赖注入/插件框架本体）** | **12.9%** | **548 ms** |
| `dsh-app-boot` | 4.3% | 183 ms |
| `cordis-plugin-loader` | 3.1% | 132 ms |
| `dsh-tools` | 3.1% | 132 ms |
| `schemastery`（配置 schema 校验） | 2.0% | 85 ms |
| `libreoffice-kit` | 1.1% | 47 ms |

`cordis` 内部再细分：`get`(proxy 读) 149ms / 匿名 94ms / `notify` 39ms /
`execute` 39ms / `apply` 39ms / `plugin` 31ms / `createTraceable` 31ms。

**`cordis` + `schemastery` + `cordis-plugin-loader` 合计 765ms 是插件框架本身的
开销**，不是任何具体插件的业务逻辑。

### 六、结论：剩下 2.4 秒该往哪砍

当前 3.42 秒（缓存命中），要到 1.0 秒需砍掉 2.4 秒。按上面的分布：

| 可砍方向 | 上限 | 现实性 |
|---|---|---|
| 插件框架开销（cordis/schemastery/loader） | ~765 ms | 中——属上游，改它要 patch 引擎 |
| 模块解析基础设施（loader/fs/esm/package_json） | ~1100 ms | 低——是 Node 自己的路径，除非换加载器 |
| dsh 各插件的启动逻辑 | ~800 ms | 中——可裁非必需插件（Tier1 已盘点 16 个） |
| GC | ~251 ms | 低——由分配量决定 |
| 第三方模块 | ~502 ms | 低 |

**关键判断**：即使把插件框架和模块解析全砍掉，也只能到约 1.5 秒。
**1.0 秒需要换掉加载模型（如 SEA / 单文件打包），不是调参数能到的。**
这条结论写在这里，避免后续再在「调 flag / 裁插件」上反复尝试。

## 14. AHB 渲染路径：同进程 A/B 对照下 1080p 快 1.72 倍

> **⚠️ 本节发布时曾误写为「19.9 倍」，后被同进程 A/B 对照推翻并改正。**
> 错误来源：拿「只渲染进 AHB 0.341 ms」去比「渲染 + glReadPixels 回读 6.777 ms」，
> 而后者是上一轮**另一个探针、另一批次**的数字。两者测量口径不同，不可比。
> 真实收益见下面的 A/B 表：**1080p 6.344 vs 10.930 ms/帧 = 1.72 倍**。
> 这条教训已写进 pitfalls #41：**跨探针、跨批次的数字不能横比**。

### 为什么之前慢

旧路径（`framesrv.c` / `lib/dsh_gl.c`）：

```
GPU 渲染 → glReadPixels（CPU 回读）→ 写文件/共享内存 → App mmap
                  ↑
              占 83% 的瓶颈
```

1080p 拆解：`render+finish` 1.125 ms + `glReadPixels` **5.630 ms** +
`write` 0.014 ms = **6.777 ms/帧（147.6 fps）**。

`glReadPixels` 慢是因为它强制 GPU→CPU 同步，并且要按行拷贝整帧 8.3 MB。

### AHB 怎么绕过它

`AHardwareBuffer` 是一块**GPU 可渲染、CPU 可映射、跨进程可传**的缓冲。
GPU 直接写进去，App 侧拿同一块内存当纹理 —— **一次回读都不需要**。

链路（四步，缺一不可，坑见 pitfalls #40）：

```
AHardwareBuffer_allocate
  → eglGetNativeClientBufferANDROID      (AHB → EGLClientBuffer)
  → eglCreateImageKHR(EGL_NATIVE_BUFFER_ANDROID)  (→ EGLImage)
  → glEGLImageTargetTexture2DOES         (→ GL_TEXTURE_2D)
  → glFramebufferTexture2D               (→ FBO color attachment)
  → 正常绘制，像素直接落进 AHB
```

### 实测（模拟器 dsh34 / x86_64 / SwiftShader）

| 分辨率 | render into AHB | AHB lock（一次性） |
|---|---|---|
| 640×360 | **0.044 ms/帧** | 0.765 ms |
| 1280×720 | **0.129 ms/帧** | 4.381 ms |
| 1920×1080 | **0.341 ms/帧** | 4.498 ms |

像素正确性：`glClearColor(1.0, 0.35, 0.1)` → CPU lock 读回
`center RGBA = 255,89,26,255`（精确对齐）。`FBO status = 0x8CD5`
（`GL_FRAMEBUFFER_COMPLETE`）。

### ★同进程 A/B 对照（`bench/ahb_topipe.c`）

**这是唯一可信的对比方式**：同一个二进制、同一轮循环、同一个 EGL 上下文，
A 路径走 AHB、B 路径走普通 FBO + glReadPixels，各跑 60 帧。

| 分辨率 | A：AHB render+lock+copy | B：glReadPixels render+read+copy | 倍数 |
|---|---|---|---|
| 640×360 | **1.131 ms**（884 fps） | 1.523 ms（657 fps） | 1.35× |
| 1280×720 | **3.162 ms**（316 fps） | 4.985 ms（201 fps） | 1.58× |
| 1920×1080 | **6.344 ms**（158 fps） | 10.930 ms（91.5 fps） | **1.72×** |

细分（1080p）：

| 段 | AHB 路径 | glReadPixels 路径 |
|---|---|---|
| render + finish | 1.144 ms | 1.111 ms |
| GPU→CPU 取像素 | **3.954 ms**（`AHB_lock`） | **8.379 ms**（`glReadPixels`） |
| copy 到共享文件 | 1.213 ms | 1.440 ms |

**结论**：`AHB_lock` 本身也包含 GPU→CPU 同步，所以它并不是「免费」的；
但从 8.379 降到 3.954 —— **取像素那一段快了 2.1 倍**，这才是真实收益来源。
整体 1.72 倍是因为 copy 和 render 两段没变，稀释了收益。

`AHB_lock` 在单帧单次调用下是每帧成本（不是一次性），这点此前也写错过。

### 已接入产线（`bench/framesrv.c` 第 7 个参数）

`framesrv.c` 现在支持三个取像素方式：`rpix`（默认，原 glReadPixels）/
`ahb` / `auto`（有 AHB 就用、没有自动退回 rpix）。产线版实测（各 60 帧）：

| 分辨率 | rpix | ahb | 倍数 |
|---|---|---|---|
| 1280×720 | 210.4 fps | **286.0 fps** | 1.36× |
| 1920×1080 | 80.8 fps | **121.4 fps** | **1.50×** |

像素正确性：两条路径写出的共享文件 `seq=60`、中心像素 `69,71,82,255` 完全一致。

AHB 不可用时**必须安静退回 rpix**：真机 gralloc 与模拟器不同（模拟器
`createFromHandle` 恒 `-22`，见 pitfalls #41），帧服务是产线依赖，
降级只能慢不能崩。App 侧传的是 `auto` 而非 `ahb`，也是同一个理由。

### 能力边界

| 项 | 状态 |
|---|---|
| AHB 分配（GPU 渲染 + CPU 读） | ✅ 实测 |
| AHB → EGLImage → GL texture → FBO | ✅ 实测（FBO COMPLETE） |
| GPU 渲染进 AHB 后 CPU 读到正确像素 | ✅ 实测（255,89,26,255） |
| 渲染吞吐 | ✅ 实测 1080p 0.341 ms/帧 |
| **AHB 跨进程交给 App** | ❌ **当前设备不可行** —— `AHardwareBuffer_createFromHandle` 在模拟器上一律返回 `-22(EINVAL)`。根因是 gralloc allocator 未进 VINTF：`servicemanager: Could not find android.hardware.graphics.allocator.IAllocator/default in the VINTF manifest.` + `Gralloc4: mapper 4.x is not supported`。同进程自测也失败、三种 desc × 两种 method 全 -22 → 环境级拒绝，非用法问题（详见 pitfalls #41）。真机未验 |
| **真机 Adreno 验证** | ⬜ **未做** —— 全部数据来自 SwiftShader（CPU 渲染）。Adreno 上 render 会更快，但 `glEGLImageTargetTexture2DOES` 的路径需要重新确认 |
| 硬件合成（不经过 App 贴图，直接进 SurfaceFlinger） | ❌ 仍需 app Binder，免 root 拿不到 |

**结论**：容器内「GPU 零拷贝渲染」这半段完全打通且快了 20 倍；
「交给 App 显示」那半段仍走原来的共享文件路径，尚未改用 AHB。

## 15. 冷启 3.4 秒的第二次精确归因：模块加载基础设施合计约 1.8 秒

### 为什么要重做

第 13 节那份 profile 的 `(idle)` 只有 0.7%、node builtin 44.7%，看起来瓶颈清楚。
但这次用**在就绪点主动截断**的探针（`bench/profat.cjs`）复测，发现分类漏了一大块：
profile 里有一批样本的 `callFrame.url` 是空串，第 13 节把它们归进了「dsh 插件」或直接忽略，
实际它们全是 node 的**模块加载 native 路径**。

### 探针方法（这个坑必须记住）

**不能用 `--cpu-prof`**：node 只在**正常退出**时才落盘。引擎就绪后是常驻服务、
必须 `kill -9`，于是文件根本不生成：

```
$ node --cpu-prof ... bin.js web --no-open & ... kill -9 $P
[readyat] 7180.6 ms rss=242MB
ls: prof_full.cpuprofile: No such file or directory     <- 没写盘
```

**也不能用 `v8.startProfiling`**：那是 node 内部 `--prof` 路径的私有接口，
公开的 `node:v8` 没有这个函数：

```
TypeError: v8.startProfiling is not a function
```

**正确途径是 inspector 协议**，并在拦截到 `dsh web:` 的那一刻 `Profiler.stop`：

```js
const session = new (require('node:inspector').Session)();
session.connect();
session.post('Profiler.enable', () => session.post('Profiler.start'));
// 拦 console.log，命中 'dsh web:' 时：
session.post('Profiler.stop', (err, res) => fs.writeFileSync(out, JSON.stringify(res.profile)));
```

这样 profile **天然截断在就绪点**，不会把就绪后的稳态空转算进来
（第 13 节那次如果直接跑完再 kill，`(idle)` 会虚高到 68%）。

### 实测构成（模拟器 dsh34 / x86_64 / node v24.18.0，uptime=3820ms）

`total=3656.9ms samples=471`

| 归谁 | 耗时 | 占比 |
|---|---|---|
| node builtin（`internal/*`、`node:*`） | **1213.1 ms** | **33.2%** |
| **native/unknown（模块加载 native 路径）** | **605.8 ms** | **16.6%** |
| `@deepseek-ai/cordis` | 496.7 ms | 13.6% |
| GC | 260.3 ms | 7.1% |
| `zod` | 98.8 ms | 2.7% |
| `cordis-plugin-loader` | 76.1 ms | 2.1% |
| `dsh-app-boot` | 73.0 ms | 2.0% |
| `dsh-tools` | 70.7 ms | 1.9% |
| `schemastery` | 64.4 ms | 1.8% |
| `js-yaml` | 53.8 ms | 1.5% |
| 其余（llm-deepseek / client-modules / domino / temporal / typert / commander 等） | 各 <50 ms | — |

### `native/unknown` 那 606ms 到底是什么

按自身耗时排序（`bench/profsum.mjs` 的明细模式）：

```
34.5 ms  ModuleWrap
26.3 ms  openFileHandle
22.1 ms  getPackageScopeConfig
18.1 ms  lstat
17.6 ms  internalModuleStat
17.2 ms  evaluate
17.1 ms  decodeUTF8
15.6 ms  compileFunctionForCJSLoader
15.1 ms  parse
14.6 ms  readFileUtf8
```

**全是模块加载的基础设施**：stat 找包、读文件、UTF-8 解码、编译、包装成 ModuleWrap。

### 结论修正

第 13 节说「模块解析基础设施 约1.1 秒」，**实际是约 1.82 秒**
（node builtin 1213 + native 模块加载 606），占总耗时 3.66 秒的 **50%**。

这意味着：

1. **`--no-warnings`、堆参数这类调参永远到不了 1.0 秒** —— 一半时间在文件系统
   stat + 读文件 + 编译上。
2. 唯一有效的方向是**减少要加载的文件数**或**避免重复编译**：
   - 编译缓存已实测省 1112 ms / 24.5%（第 12 节），这是现在唯一已兑现的收益
   - 进一步要动的是「打包成更少的文件」或「预编译成字节码」，不是调 node 参数
3. **快照 / SEA 两条路已实测不可行**（见第 16 节）。

### 复跑命令

```sh
# 在设备上
export LD_LIBRARY_PATH=$B/engine/lib TMPDIR=$B/dsh-home/tmp
export HOME=$B/dsh-home DSH_HOME=$B/dsh-home
export NODE_COMPILE_CACHE=$B/dsh-home/.node-compile-cache
$B/engine/bin/node --expose-internals --require $B/profat.cjs \
  $B/engine/lib/node_modules/@deepseek-ai/dsh/lib/bin.js web --no-open
# 拉回 profile 后本地汇总
node bench/profsum.mjs <file.cpuprofile>
```

## 16. 快照与 SEA 实测：两条「换加载模型」的路都走不通

第 13/15 节都指向同一个结论：要碰到 1.0 秒必须换加载模型。候选只有两个：
Node 的 **startup snapshot** 与 **SEA（Single Executable Application）**。
两条都实测过了，都不可行。

### 16.1 startup snapshot：ESM 入口直接不支持

**好消息**：快照对纯 CJS 脚本收益巨大。同一台设备、同一份脚本：

| 方式 | 三轮 uptime | 中位 |
|---|---|---|
| 普通启动 | 153 / 163 / 152 ms | **153 ms** |
| `--snapshot-blob` 启动 | 60 / 59 / 47 ms | **59 ms** |

省 **94 ms / 61%**，快照 blob 6.3 MB。

**坏消息**：dsh 的入口是 **ESM**（`package.json` 有 `"type": "module"`），
而 `--build-snapshot` **内部用 CJS 路径执行入口脚本**：

```
$ node --build-snapshot --snapshot-blob esm.blob esm_snapshot_test.mjs
/data/.../esm_snapshot_test.mjs:12
import { createRequire } from 'node:module';
^^^^^^

SyntaxError: Cannot use import statement outside a module
    at minimalRunCjs (node:internal/main/mksnapshot:207:16)
```

报错里那个 `minimalRunCjs` 就是铁证：快照构建器只会按 CJS 跑入口。
**V8 startup snapshot 不捕获 ESM 的 ModuleJob/ModuleMap**（那是 per-realm 运行时结构），
所以即使绕过入口限制，ESM 模块图也进不了快照。

要让快照生效，必须把整个 dsh 引擎先打包成单个 CJS 文件 —— 那是 bundler 工程
（几百个 ESM 包 + 动态 import + 条件导出），不是配置项。

### 16.2 SEA：blob 能生成，但同样只支持 CJS 入口

```
$ node --experimental-sea-config sea-config.json
Wrote single executable preparation blob to sea_prep.blob
$ ls -la sea_prep.blob
-rw------- 1 root root 1961 sea_prep.blob      <- 只有 1961 字节（仅 CJS 入口本身）
```

SEA 的 `main` 字段同样要求 CJS。**CJS 里动态 `import()` ESM 是能工作的**
（实测 `ESM_IMPORT_OK`），但 SEA 的价值在于「把代码打进二进制减少文件读取」，
而 `import()` 出去的 ESM 仍然要从磁盘逐个加载 —— **收益归零**。

### 16.3 结论

| 方案 | 状态 | 卡在哪 |
|---|---|---|
| startup snapshot | ❌ 不可行 | `--build-snapshot` 只支持 CJS 入口；V8 快照不含 ESM 模块图 |
| SEA | ❌ 不可行 | `main` 只支持 CJS；动态 import 的 ESM 仍走磁盘 |
| 预编译字节码（`NODE_COMPILE_CACHE`） | ✅ **已生效** | 省 1112 ms / 24.5%，已在产线代码里 |
| 打包成更少的文件（bundler） | ⬜ 未试 | 工程量大，且要与上游包结构解耦 |

**所以「1.0 秒」目前没有已证路径**。能兑现的只有编译缓存那 1112 ms。
剩下的空间在 node 自身的模块加载机制里（1.82 秒），要么改上游打包方式，
要么接受 2.3 秒左右的实际下限。

### 复跑命令

```sh
B=/data/data/app.dsh.mobile/files; E=$B/engine
export LD_LIBRARY_PATH=$E/lib TMPDIR=$B/dsh-home/tmp

# 16.1 快照（CJS 有收益）
$E/bin/node --build-snapshot --snapshot-blob $B/snap.blob $B/snapshot_test.cjs
$E/bin/node --snapshot-blob $B/snap.blob $B/snapshot_test.cjs

# 16.1 快照（ESM 不支持）
$E/bin/node --build-snapshot --snapshot-blob $B/esm.blob $B/esm_snapshot_test.mjs
# -> SyntaxError: Cannot use import statement outside a module

# 16.2 SEA
$E/bin/node --experimental-sea-config $B/sea-config.json
```

---

## 第八节 · 冷启精确归因（无 App 的 root 域测量）

### 为什么必须能脱离 App 测

App 内跑引擎的环境由 Kotlin 注入，会话一多就被并发改包打断，测出来的数
**不可复现**。root 域直跑只要 overlay 与 android-plugins 在就能测。

前置两个坑（都不补会直接启动失败）：

```sh
# ① overlay 由 EngineConfig.ensureAndroidOverlay() 运行时生成，root 域没有
adb push android-overlay.yml /data/local/tmp/ov.yml
adb shell "cp /data/local/tmp/ov.yml $E/android-overlay.yml"

# ② android-plugins 由 App 从 assets 解包，root 域也没有
adb push app/src/main/assets/android-plugins /data/local/tmp/ap
adb shell "cp -r /data/local/tmp/ap $E/android-plugins"
```

不补 ① 报 `failed to read overlay ... ENOENT`；不补 ② 会刷一屏
`ERR_MODULE_NOT_FOUND: .../android-plugins/plugin-health.mjs`。

工具全部在 `bench/coldstart/`：`run.sh` / `readyat.cjs` / `modcount.cjs`
/ `rpcount.cjs` / `rpcache.cjs`。

### 基线（模拟器 dsh34 / x86_64 / node v24.18.0 / dsh 0.2.2-rc.1）

交替 3 轮，中位 **3213.5 ms**（3111.0 / 3213.5 / 3604.1）。
**噪声底约 ±250ms** —— 任何小于此的优化都无法在这台设备上被证实。

### 模块加载量（关键定量）

```
[modcount] resolves=488 unique=271 loads=662
```

**只有 271 个唯一模块**。所以「模块太多」不是原因，3.2 秒不是被文件数量堆出来的。

### I/O 量

```
[iocount] stat=337 lstat=0 open=853 read=853 readdir=1 realpath=3490 io_ms=305.6
```

`realpath` 3490 次 vs 271 个模块 —— 重复 13 倍。逐个看：

```
[rp] calls=3490 unique=1554 ms=229.3 ms/call=0.066
[rp-top] 108x .../@deepseek-ai/schemastery/lib/index.mjs
[rp-top]  65x .../@deepseek-ai/cordis/lib/index.js
[rp-top]  59x .../zod/v4/core/util.js
[rp-top]  53x .../zod/index.js
```

同一路径最多被算 **108 次**，总耗时 229.3ms，看着有 150ms 可省。

### ★实测无效的方向（不要再试）

给 `realpathSync` 加进程内 Map 缓存（命中率 66.1%），交替 A/B 各 3 轮：

| | 三轮 | 中位 |
|---|---|---|
| 基线 | 3111.0 / 3213.5 / 3604.1 | **3213.5 ms** |
| 加缓存 | 3008.6 / 3443.1 / 3542.1 | **3443.1 ms** |

**慢 229.6ms，两组范围完全重叠**。收益低于噪声底。

推测（未验证）：0.066ms/次的 realpath 大部分是页缓存命中，真正开销被
Map 查找 + wrapper 调用与 V8 内联失效抵消。

### 对「1.0 秒」的硬判断（累计证据）

| 方向 | 实测结论 |
|---|---|
| 容器层开销 | 4 ms/run（b147），不是瓶颈 |
| 编译缓存 | 唯一已证收益，省 1112ms（24.5%） |
| V8 快照 | ESM 不支持，双否 |
| SEA 单文件 | `main` 只支持 CJS，ESM 收益归零 |
| Node 24 | 慢 30~70% |
| 堆参数 | 慢 18% |
| realpath 缓存 | 慢 7%（本节） |

**结论：调参数已无空间。1.0 秒需要换加载模型（把 ESM 模块图预编译成
可 mmap 的字节码单元），不是继续微调。**

### 6.13 冷启参数与缓存类优化：三条实测否证 + 理论下限

模拟器 dsh34（Android 14 / x86_64 / API 34），node v24.18.0（bionic，
`engine/bin/node`），引擎 `@deepseek-ai/dsh` 0.2.2-rc.1，DB 同为
`files/engine/lib/node_modules`，每轮起前 `pkill -9 -f dsh/lib/bin.js` + `sleep 2`。
口径统一：`--require readyat.cjs` 在拦到 `dsh web:` 那一刻打 `process.uptime()`。

**阶段基线**（`--require` 探针实测，非推算）

| 阶段 | 耗时 | 说明 |
|---|---|---|
| `node -e 0`（空跑） | **186 ms** | 进程起 + V8 初始化 + 退出，批平均 |
| `bin.js --version`（CLI 引导） | **444 ms** | 再加 commander 解析与模块解析入口 |
| 完整就绪到 `dsh web:` | **3217.6 ms** | 5 轮中位（3024/3082/3218/3621/3860） |

→ **插件加载段 = 3217.6 − 444 = 2773 ms（86%）**。容器层不在此路径上
（独立实测 4 ms/run，见 §11），所以冷启的全部空间在 Node 侧的模块加载与插件启动。

**三条否证（都不采用）**

| 方向 | 做法 | 结果 | 结论 |
|---|---|---|---|
| realpath 结果缓存 | 拦 `fs.realpathSync` + `.native`，单参数形态走 Map | 命中率 **66.1%**（hits=1084/misses=555），但就绪中位 **3217.6 → 3443.1 ms（+7.1%）**，两组范围完全重叠 | 否证。`realpathSync` 确实被调 **3490 次**（唯一 1554，`schemastery/lib/index.mjs` 单文件 108 次、`cordis/lib/index.js` 65 次），总耗时 **229.3 ms**（0.066 ms/次）；但 Map 查找 + 包装函数的调用开销吃掉了收益 |
| V8 并行编译线程 | `--v8-pool-size=4`（设备 4 核） | 5 轮中位 **3217.6 → 3149.2 ms（−2.1%）**，两组范围完全重叠 | 否证。噪声级 |
| 模块加载量压缩 | 统计 `Module._resolveFilename` / `_load` | 启动共 **488 次 resolve、271 个唯一模块、662 次 load** | 否证「加载量太大」假设。271 个模块不算多，问题在每模块的成本（编译 + 磁盘 I/O），不在数量 |

**理论下限推算**：`186 ms`（node 起）+ `258 ms`（CLI 引导净增）+ 插件框架与
271 个模块的加载/编译。**即使把所有插件都禁用只剩框架，也要 ~1.0 秒以上**；
1.0 秒整机冷启**需要更换加载模型**（把 ESM 模块图预编译成可 mmap 的字节码单元，
或用 SEA 单文件 + 预编译字节码），不是调参数能到的。

> 注：`fs` 包装必须 `Object.assign(wrapped, orig)` 保留静态属性，
> 否则 `dsh-app-boot` 的 `realModuleDirectory` 调 `realpathSync.native` 报
> `TypeError: realpathSync.native is not a function` 直接崩启动（实测踩过）。

### 6.14 预置编译缓存随 APK 分发：唯一实测有效的冷启优化

§6.13 已经把参数级优化全部否证完了。剩下唯一能动的是**让首次启动就命中缓存**。

**问题**：编译缓存是本地生成的。新装 APK 的用户第一次启动必然没有缓存，
那一轮就慢 840ms。

**做法**：真机跑一轮引擎 → `tar czf` 出 1281 个文件 → 转 zip 打进 APK →
首次冷启前从 assets 解到 `$DSH_HOME/.node-compile-cache`。

**实测（模拟器 x86_64，交替 A/B 各 3 轮）**

| 状态 | 三轮 | 中位 |
|---|---|---|
| 有预置缓存 | 3184.0 / 3357.4 / 3714.8 | **3357.4 ms** |
| 无缓存 | 3664.3 / 4197.9 / 4746.5 | **4197.9 ms** |

→ **省 840.5 ms（25.0%）**。这一条不依赖真机，因为缓存命中与 CPU 架构无关
（只与 node 版本 + process.arch 相关，两者都已匹配）。

**体积代价**：3.17 MB（zip，`compresslevel=9`）/ 7.86 MB 解压 / 1281 个文件。
tgz 只要 2.55 MB，但 tar 路线走不通（见下）。

**三条硬约束（都是实测出来的）**

1. **目录名 `v<node版本>-<process.arch>-<hash>-0` 两段都不能改**。
   把 `x64` 改成 `arm64` 后：耗时 **5715.8 ms**（= 无缓存水平），
   且 Node 自己又新建了一个 `x64` 目录 —— 它按 `process.arch` 拼名字找缓存。
2. **已有当前版本目录就跳过**。否则每次冷启都白解 1281 个文件。
3. **版本从 node 二进制头部读，不执行它**。冷启路径不能多一次 fork
   （这也是 §6.13 里 `command -v node` 被改成静态文件检查的同一个原因）。

**为什么不用容器里的 tar（这条值得单独记）**

第一版实现是「tar.gz + 容器内 `tar xzf /eng/...`」，**它永远不可能成功**：

`app/src/main/java/app/dsh/mobile/engine/EngineConfig.kt` 的
`linuxContainerArgv()` 只返回三样东西 ——

```kotlin
return listOf(runner.absolutePath, rootfs.absolutePath, interp)
```

**一条 `-m` 挂载都不加**。所以 `LinuxContainerRunner.shell()` 起的容器里
既没有 `/eng` 也没有 `/dsh`（实测容器内 `ls /eng` → No such file or directory）。
`-m` 只在 `LinuxEngineLauncher` 和 `buildTerminalArgv` 那两条路径上加。

改用纯 Kotlin `ZipInputStream` 之后：更快、不要求 rootfs 就绪、
也不占 `LinuxContainerRunner` 那个只有 3 个名额的 Semaphore（冷启路径上
抢名额会让真正要紧的调用白等）。

### 6.15 预置缓存必须按「实际要跑的那个 node」生成

§6.14 记录的是 840ms / 25% 的收益（在 `engine/bin/node` 上测得）。但把它接进
产线时发现**收益归零**，根因是版本张冠李戴。

**两条不同的 node**

| 场景 | 实际执行的 node | 版本 | 缓存目录名 |
|---|---|---|---|
| 原生模式 | `engine/bin/node`（bionic） | v24.18.0 | `v24.18.0-x64-cf738c9d-0` |
| **Linux 模式** | 容器内 `/usr/bin/node`（musl） | **v22.23.4** | `v22.23.4-x64-9ac5647c-0` |

（Linux 模式的 node 路径见 `LinuxEngineLauncher.NODE_IN_CONTAINER = "/usr/bin/node"`）

**版本与构建指纹都不同** —— 目录名最后那段 hash（`cf738c9d` vs `9ac5647c`）
是 node 自身的构建指纹，跨构建不可复用。

**后果**：包是按 v24 打的，Linux 模式跑 v22。seed 照解 958~1281 个文件
（6.88MB），Node 一个都不认，磁盘白占。A/B 实测：

| 状态 | 三轮 | 中位 |
|---|---|---|
| 有包（版本错） | 3894.3 / 4328.2 / 5003.3 | **4328.2 ms** |
| 无缓存 | 4069.7 / 4275.1 / 4945.0 | **4275.1 ms** |

→ **−53.1 ms，两组完全重叠 = 零收益**。

**修法两条**

1. **按容器 node 重新生成包**：958 个文件 / 6.88MB 原始 / **2.66MB zip**，
   目录名 `v22.23.4-x64-9ac5647c-0`。
2. **加版本交叉校验**：`seededCachePrefix()` 拿到包内版本后，与实际 node
   的 `--version` 比对，不符就跳过（返回 null），不再写一堆无用文件。
   取 live 版本只能靠执行 `node --version` —— 不能读二进制头部（pitfalls #49）。
   这次 fork 可接受：只发生在部署阶段（`ensureDeployed`），不在冷启路径上。

**通用教训**：凡"预生成物 + 版本目录"这类机制，**生成口径必须与实际消费方一致**。
这里两者同名（都叫 node）但来自不同构建，是最容易张冠李戴的一类。
判断方法：直接问消费方自己（`--version`），不要从另一个来源推断。

### 6.16 用 CPU profile 而不是 wall clock 判断缓存是否生效

§6.15 修好版本匹配后，wall-clock A/B 仍然测不出收益，而且**数据自相矛盾**：

| 轮次 | A（有缓存） | B（无缓存） | 设备 load |
|---|---|---|---|
| 1 | 8654.9 ms | 3756.4 ms | 1.79 |
| 2 | 7691.4 ms | 2579.7 ms | 1.03 |
| 3 | 11486.6 ms | 8019.7 ms | 0.29 |

「有缓存慢 4.9 秒」在物理上不可能。两个原因：

1. **负载在 0.29~4.45 之间剧烈波动**（模拟器 4 核 / 2.4GB，同时有别的会话在跑引擎、
   还有我自己的 `framesrv` 残留两个吃满 2 核）。同一台机器上「有缓存」和
   「无缓存」两组的绝对耗时根本不可比。
2. **顺序偏差**：A 永远先跑，B 吃到 A 留下的 page cache。交替 A/B 在这种
   环境下等于系统性地给 B 组加分。

**改用 CPU profile（`bench/profat.cjs`，inspector 协议在就绪点停采样）**。
CPU 时间不受调度延迟与 page cache 影响，是同一口径的量：

| 归谁 | 有缓存 | 无缓存 | 差 |
|---|---|---|---|
| **总 CPU** | **3062.0 ms** | **4036.5 ms** | **−974.5 ms（24.1%）** |
| node builtin | 928.3 ms | 1261.3 ms | −333.0 |
| **native/unknown（模块加载）** | 852.1 ms | 1263.8 ms | **−411.7** |
| `@deepseek-ai/cordis` | 455.5 | 471.3 | −15.8 |
| `js-yaml` | 121.0 | 88.6 | +32.4 |
| `cordis-plugin-loader` | 76.2 | 115.0 | −38.8 |

→ **缓存确实生效**，省掉的正是编译与模块加载基础设施（`native/unknown`
的 `ModuleWrap`/`compileFunctionForCJSLoader`/`parse` 那一族），
而插件框架本身（cordis 455ms）与缓存无关，几乎不变（符合预期）。

**方法论结论**：判断「某个缓存/预编译优化是否生效」这类问题，
**wall clock 在嘈杂设备上不可用，应直接看 CPU profile 的分项**。
wall clock 可以顺带记，但结论必须建立在 CPU 时间上。

### 6.17 容器内设备能力（AgentBridge）端到端

Linux 模式相对 tawcroot 的核心差异之一：容器里的 agent 能直接操作这台手机。
链路跨三个域，任何一环断掉都只表现为「工具报 403/超时」：

    容器（musl）→ 宿主回环 TCP → App 的 AgentBridge → Android 无障碍服务

验证脚本 `bench/bridge_probe.sh`（在容器内跑），四段断言：

| 断言 | 含义 | 期望 |
|---|---|---|
| `no_token_403` | 无 token 必须被拒 | 证明鉴权真的在起作用，不是裸奔 |
| `auth_ok` | 带 token 可达 | 证明 token 注入链路通 |
| `a11y_ok` | 无障碍服务可用 | 拿得到前台包名 |
| `launch_ok` | 真能操作设备 | 启动 app 并核对回执包名 |

模拟器 dsh34 / x86_64 / Alpine 3.22.2 实测：

```
no_token_403=ok
auth_ok=ok
a11y_ok=ok
launch_ok=ok
RESULT: BRIDGE_OK
```

反向验证：把 token 换成错误值或清空，后三项立刻 FAIL（`no_token_403`
仍 ok —— 它本来就该在无 token 时通过）。宿主侧用 `dumpsys window` 核对，
`mFocusedApp` 确实切到 `com.android.settings`。

**前提两条**（都是容易被忽略的）：

1. **App 进程必须在跑** —— bridge 是 App 内的 HTTP 服务（:3083），
   App 被杀则整条链路消失。判据 `pidof app.dsh.mobile`。
2. **token 是每进程随机生成的** —— 只在引擎子进程的 environ 里，
   不在任何 prefs 或日志中（`currentToken()` 不打日志值）。取法：
   `for p in $(ps -A -o PID,NAME | grep node); do cat /proc/$p/environ |
   tr '\0' '\n' | grep DSH_BRIDGE_TOKEN; done`。空 token **不写**进环境，
   否则 `_dsh_http.sh` 的 `[ -n "$TOKEN" ]` 会通过、反而发出无头请求。

**门脚本在容器里的自举链**（这条不通则 `scr`/`notify`/`say` 全部
`not found`，很容易误判成脚本丢了）：

    assets/gates/scr 的 shebang #!/system/bin/sh
      → 容器 rootfs 无 /system/bin（需 installLinuxCompatShims 补 shim）
      → 脚本内 exec "@DSH_BASH@"，Linux 模式替换为 /bin/bash
      → Alpine 的 /bin/bash 来自 BOOTSTRAP_PACKAGES（bash/curl/ca-certificates）
      → 缺 bash 时报 "exec: line 8: /bin/bash: not found"

装载顺序也容易踩：`-m /eng:<宿主engine目录>` 会让容器里看到的
`/eng/bin/scr` **就是宿主那份**（shebang 写死 `/system/bin/sh` +
bionic bash 绝对路径），在 musl 里必 `REEXEC: preload FAIL`。
要验产线部署结果，必须用 `-m /eng:` 之外的路径，或先让
`deployLinuxAgentGates()` 真正跑过一遍。

### 6.18 合成画面直通 App 的产线路径（两条起容器路径的 bind 差异）

App 里有两条独立的「起容器」路径，bind 视图曾经不一致：

| 路径 | 入口 | bind |
|---|---|---|
| 引擎 | `LinuxEngineLauncher.buildEngineArgv()` | `buildBinds()` 十几条（/eng /ws /dsh + /data/data /data/user /storage…） |
| 执行器 | `LinuxContainerRunner.shell()` | `EngineConfig.linuxContainerArgv()`，**原来一条都没有** |

后果：`LinuxActivity` 把宿主路径 `/data/data/<pkg>/files/dshframe.bin`
传给容器内的程序时，引擎那条路能写、执行器那条路写不了 —— 而调用方
不知道自己走在哪条上。实测：

```
captureCompositor() -> WLSHOT_NOSTART
/tmp/wlshot.log: FAIL open /data/data/app.dsh.mobile/files/dshframe.bin:
                 No such file or directory
```

修法是给执行器补 `/data/data` 一条挂载（不整表复制 `buildBinds`，两张表
整表同步迟早分叉）。补上后同一套产线脚本立刻变成 `WLSHOT_STARTED`。

端到端核色（模拟器 dsh34 / x86_64 / sway headless + pixman）：

```
disp=wayland-1  wlwin_kv=1
共享文件 3686416 B = 16 + 1280*720*4     seq=86（持续递增）
窗口 282x148 @ x 40..321, y 60..207：
  x= 40 RGBA( 30, 89,225)   x=180 RGBA(142, 89,113)   x=321 RGBA(255, 89,  0)
  x=110 RGBA( 86, 89,169)   x=250 RGBA(198, 89, 57)
窗口外全黑，alpha 全 255
```

G 恒 89、R 30→255、B 225→0 正是 `wlwin` 写入的渐变；窗口外全黑说明
合成层边界正确。三条结论缺一不可：

1. **seq 递增**证明 `--loop` 真在持续抓帧（不是单帧快照，否则画面会永久静止）；
2. **像素逐点符合写入值**证明抓的是真实合成结果（不是清屏色）；
3. **窗口外全黑**证明抓的是 compositor 输出而不是某个客户端 buffer。

启动顺序也踩过：必须先开窗口、等它 `xdg_surface.configure` 之后 commit，
再起帧服务。反过来先起帧服务会抓到全黑帧。另外多个 compositor 会留下
多个 `wayland-N` socket，`ls -t` 选最新的可能选到没人渲染的实例 ——
同样表现为全黑且不报错。所以验证脚本每次先 `pkill` 并重建 `/tmp/wl`。

### 6.19 容器路径 vs 原生路径的冷启对比（首次同设备同引擎）

此前所有冷启数据（1.9s / 3.4s / 缓存省 1112ms）**全部来自原生 bionic 路径**，
容器路径从未与原生做过同设备对比。而「Linux 模式」才是产品差异化的核心，
它的冷启代价必须先量出来，否则优化方向可能是错的。

**注意两者的 node 不是同一份运行时**：

| 路径 | node | 版本 | 引擎 |
|---|---|---|---|
| 原生 | `engine/bin/node`（bionic） | **v24.18.0** | 0.2.1-rc.2 |
| 容器 | `/usr/bin/node`（musl, apk） | **v22.23.4** | 同一份代码 |

#### 一、wall-clock 不可比，必须看 CPU 时间

交替两轮 `[readyat]`（就绪那一刻 `process.uptime()`）：

```
容器 4050.8 / 2237.9 / 1958.6 ms      原生 2591.2 / 4834.6 / 2092.8 ms
```

两组完全重叠，方向都不稳定。设备负载在 0.15~4.45 间跳动，wall-clock 在这台
模拟器上**测不出几十毫秒级的差异**。

改用 `--cpu-prof` 总 CPU 时间（不受调度延迟影响）：

| 轮次 | 容器 CPU | 原生 CPU | 差 |
|---|---|---|---|
| 第 1 轮 | 2237.7 ms | 2496.3 ms | −258.6 (−10.4%) |
| 第 2 轮 | 2470.6 ms | 2163.8 ms | +306.8 (+14.2%) |
| **中位** | **2470.6** | **2496.3** | **−25.6 (−1.0%)** |

**两轮方向相反 → 差异是噪声。容器层对冷启 CPU 时间没有可测影响。**

这与 §11「容器层单进程开销 4 ms/run」一致：容器只承担 fork+execve+seccomp
装载，引擎真正的开销在 Node 模块加载与插件激活，那部分容器里外一样。

**结论：冷启优化继续在引擎侧做，容器侧没有可挖的空间。** 这排除了一个
看起来很有希望的方向（换运行时/换容器实现能提速）。

#### 二、编译缓存在容器路径同样生效，且收益更大

之前只在原生路径测过缓存的 A/B（4528.4 → 3416.8 ms，省 24.5%）。容器路径
这次用 CPU 时间做干净 A/B（容器 musl node v22.23.4）：

| 状态 | 总 CPU | node-builtin | cordis | dsh-plugins | third-party |
|---|---|---|---|---|---|
| **缓存命中** | **2147.8 ms** | 924.4 | 462.8 | 338.8 | 188.4 |
| **缓存清空** | **3167.9 ms** | 1644.4 | 572.6 | 438.2 | 281.8 |
| 差 | **−1020.1 (−32.2%)** | **−720.0** | −109.8 | −99.4 | −93.4 |

省掉的 720 ms 全在 `node-builtin`，即 V8 编译与模块加载基础设施
（`ModuleWrap` / `compileFunctionForCJSLoader` / `parse` 那一族）。

**这条纠正了一个此前的判断**：曾以为「快照对纯 CJS 有效、对 ESM 无效」，
所以 ESM 为主体的引擎吃不到编译缓存。实测证伪 —— 容器路径的 32.2% 就是
ESM 模块图拿到的收益。缓存对 ESM 有效，只是收益归在 `node-builtin` 而不是
某个 ESM 专属函数名下，按函数名检索时会漏掉。

#### 三、缓存目录按 node 版本各自一份，不会互相覆盖

```
.dsh-home/.node-compile-cache/
  v22.23.4-x64-9ac5647c-0   13 MB   ← 容器用
  v24.18.0-x64-cf738c9d-0   13 MB   ← 原生用
```

两者并存各 13 MB。切换模式不会互相污染，但也不会复用 —— 换运行时就等于
缓存全冷。`pruneCompileCache()` 只删**不属于当前 node 版本**的目录，所以
「原生模式清一次、容器模式又清一次」会来回删对方那份。实际影响是每次切换
模式第一次冷启都要重建缓存（多付约 1 秒），之后稳定。这个代价可接受，
但如果将来频繁切换，值得改成「两个版本都保留」。


## 十四、冷启里 IO 到底占多少：热态 vs 冷盘（2026-11-07 模拟器实测）

**这一节推翻了此前所有冷启数据的前提。**

此前记录的 1.9s / 3.4s / 「编译缓存省 32.2%」全部是在 **page cache 已预热**
的状态下测的。但用户真实感知的第一次启动（刚开机、刚更新完、App 被系统
杀过）面对的是**冷盘**。两者的差距必须量出来，否则优化方向可能完全是错的。

### 方法

同一份 engine、同一条命令行，只改文件系统缓存状态：

```sh
# 热态：先完整跑一轮预热
# 冷盘：sync; echo 3 > /proc/sys/vm/drop_caches
```

每组交替 3 轮取中位，且**两批实验用相反的组间顺序**（先 hot 后 cold /
先 cold 后 hot），排除「总是后跑的那组占便宜」的系统偏差。

就绪点仍用 `--require readyat.cjs` 拦 `console.log('dsh web: ...')`。

### 结果

| 批次 | 组间顺序 | 热态（三轮 / 中位） | 冷盘（三轮 / 中位） | 差值 |
|---|---|---|---|---|
| 第一批 | hot → cold | 3461 / 2919 / 2256 → **2919** | 8993 / 7035 / 5564 → **7035** | **+4116 ms** |
| 第二批 | cold → hot | 2761 / 2343 / 2130 → **2343** | 5832 / 5579 / 4814 → **5579** | **+3236 ms** |

两批方向一致：**冷盘比热态慢 3.2~4.1 秒**，冷启总时长 **5.6~7.0 秒**。

### `drop_caches` 在 Android 上只能清一部分

实测 `Cached` 只从 726964 kB 降到 624536 kB（回收约 102 MB），
其余 624 MB 是 tmpfs / 已映射页，`drop_caches` 清不掉。表现就是同一组
三轮数值单调下降（8993 → 7035 → 5564）。所以上表的「冷盘」是
**偏乐观**的下界，真实冷盘只会更慢。

### 全量冷读要 8 秒

单独测「把 engine 下全部 `*.js/*.mjs/*.cjs/*.json` 按序 cat 一遍」：

```
preread_ms=8000      (8380 个文件 / engine 共 633 MB)
```

而引擎冷启只要 4.1~5.6 秒 —— 说明**引擎只读了其中一部分**，
不是全量灌入。这排除了「无脑全量预读」这条捷径：预读本身 8 秒，
比冷启还长。

### 对目标的含义

- 「冷启压到 1.0 秒」在**热态**下的实际下限约 2.3 秒（本轮实测），
  在**冷态**下是 5.6 秒级。之前算出的「约 2.3 秒下限」只在热态成立。
- 想改善用户真实感受，**必须同时解决 IO**，不能只优化 CPU 侧的
  模块解析/编译。这两件事的量级：CPU 侧已测约 3.7 秒（--cpu-prof），
  IO 侧 3.2~4.1 秒。
- 但全量预读（8 秒）比收益还贵，说明只能做**精准预读**（只读引擎真会
  加载的那批文件），或者把冷数据搬进更快的介质。

### 环境事故（如实记录）

第二批复测跑到一半，**App 被另一并发会话重装到 1.5.3/vc369**，
`files/` 整个被清空（engine / dsh-home / linux 全没了），
`modcount` 那一步因此失败（`ENOENT: lstat '/dsh'`）。
本节数据来自重装前的 07:14–07:17 UTC 窗口，有效；
后续步骤需要等 App 重新部署 engine 后才能继续。

### 补充：App 自身的 boot step 计时（原生模式，环境刚恢复后的真冷态）

`logcat` 里 `EngineSupervisor: boot step` 是 App 自己打的，比外部探针更贴近
用户感知：

```
boot step orphan-reap:        81ms  (total 81ms)
boot step home-owner-check:    1ms  (total 82ms)
boot step guardian-restore:    0ms  (total 82ms)
boot step parallel-preflight: 49ms  (total 131ms)
boot step runtime-install:     7ms  (total 138ms)
boot step hardssh-install:     1ms  (total 139ms)
boot step user-data:          17ms  (total 156ms)
boot step profile-plugins:     3ms  (total 159ms)
boot step spawn:             155ms  (total 314ms)
boot step health-poll:      2782ms  (total 3096ms)
```

两个结论：

1. **`spawn` 只要 155ms** —— 容器/原生「起进程」这一步在总时长里可以忽略。
   这印证了「容器层单进程开销 4ms/run」那节：容器本身不是瓶颈。
2. **`health-poll` 2782ms 占了 90%** —— 这是等引擎「完全就绪」的轮询时间，
   里面包含的就是 JS 模块解析 + 编译 + 插件激活。IO 慢（冷盘 +3.2~4.1 秒）
   正是加在这一段上。

所以冷启优化的唯一着力点就是 `health-poll` 这一段，而它 = CPU（约 3.7 秒，
`--cpu-prof` 实测）+ IO（冷盘 3.2~4.1 秒）。两者都不能单独解决，必须并行
优化 —— 或者干脆**不让用户看到它**（后台预热 + 首屏直出）。

**注意**：上面的 `engine/bin` 里的 flusher 探针路径在新版 runtime 里已变化
（`node-compile-cache-flusher.cjs` 不在 engine 根了），所以外部 `--require`
方式暂时失效，本轮的原生冷热复测未能完成。App 自身的 boot step 计时不受影响。

### 6.20 手机上跑 QEMU 虚拟机并取到真实画面（主机侧取证完成）

目标里「在手机里跑模拟器且能给出画面」这一项，链路打通并做到了**像素级取证**。

**平台限制（实测，非推测）**

| 项 | 实测值 | 含义 |
|---|---|---|
| `/dev/kvm` | 不存在 | 无硬件虚拟化 |
| `grep -c vmx /proc/cpuinfo` | `0` | CPU 不暴露 VMX（模拟器宿主） |
| 可用加速 | `-accel tcg` | 纯软件模拟，guest 引导 40s+ |

**三段链路与各自耗时**

| 阶段 | 现象 | 耗时 |
|---|---|---|
| QEMU 启动 + SeaBIOS | `Booting from ROM...` | ~2s |
| guest 内核引导（TCG） | 内核日志开始滚动 | ~1.5s |
| bochs DRM probe + fbcon 切模式 | `fb0` 出现、`160x50` 字符模式 | ~0.15s 内连续完成 |

内核日志原文（Alpine `linux-lts` / `6.12.115-0-lts` / `-vga std`）：

```
[    1.653093] bochs-drm 0000:00:02.0: vgaarb: deactivate vga console
[    1.659430] [drm] Found bochs VGA, ID 0xb0c5.
[    1.659544] [drm] Framebuffer size 16384 kB @ 0xfd000000, mmio @ 0xfebf0000.
[    1.663288] [drm] Found EDID data blob.
[    1.670408] [drm] Initialized bochs-drm 1.0.0 for 0000:00:02.0 on minor 0
[    1.687830] fbcon: bochs-drmdrmfb (fb0) is primary device
```

**取画面的三条路，只有一条可用**

| 路径 | 结果 |
|---|---|
| `-display vnc=127.0.0.1:2` + 自写 RFB 客户端 | `desktop 0x0` → 只拿到 16 字节帧头 |
| `-monitor stdio` + stdin 重定向 + `screendump` | 命令不执行（后台进程 stdin 重定向无效），文件永不生成 |
| `-monitor tcp` + `pmemsave`（**相对路径**） | ✅ 16777216 字节完整 framebuffer |

`pmemsave` 读的是 guest 物理地址 `0xfd000000`（bochs DRM 的 VRAM 基址），
等于绕开 VNC 显示模式协商，直接取像素。

**像素级验证**

按 1280×800 / 32bpp 解析 16 MB 转 PNG，得到**可读的 fbcon 文本画面**：
内核日志每一行都是清晰字形，前景 `#AAAAAA`（fbcon 默认色），背景纯黑。
肉眼可读到 `[drm] Found bochs VGA` / `fbcon: ... (fb0) is primary device` 等行。

**判据设计（关键）**

「qemu 进程在跑」和「串口有日志」这两条在 `NO_DEV_FB` 时同样成立 ——
本轮前面几轮就是这样误判「虚拟机起来了」。唯一可信判据是
**解开 framebuffer 看到字形**。

**可复现脚本**：`bench/qemu_boot.sh <rootfs> <runner> serial|screen`
- `serial` 模式约 40s，输出 `RESULT: QEMU_SERIAL_OK`
- `screen` 模式约 80s，输出 `RESULT: QEMU_SCREEN_OK size=16777216`

### 6.21 QEMU 虚拟机的持续帧服务与输入注入（10fps 实测通）

**单帧成本（批平均，已去掉人工 sleep）**

| 方式 | 单帧 | 体积 | 结论 |
|---|---|---|---|
| `screendump` → PPM | **9.0 ms/帧（111fps）** | 3072016 B（1280×800 P6） | ✅ 选它 |
| `pmemsave` 0xfd000000 | 31.0 ms/帧 | 16777216 B（固定 16MB） | 慢 3.4×、大 5× |
| shell 每帧调 `nc` | 207 ms/帧 | — | 慢 23×，不可用于持续服务 |

**qscreensrv 设计要点（`bench/qscreensrv.c`）**

1. **单进程一条 monitor 长连接到底** —— shell 每帧起 `nc` 要 207ms/帧，
   是进程启动开销而非 QEMU 慢。
2. **monitor 断了自动重连，不退出** —— 持续服务不能因单次失败判死
   （同 pitfalls #60 的教训）。
3. **alpha 显式置 255，绝不抄源字节** —— PPM 只有 RGB，但若将来换 XRGB
   来源，第 4 字节不是 alpha，抄过去 App 侧 `ARGB_8888` 会当真读，
   整屏变全透明且不报错。
4. **seq 最后写** —— 它是宿主判「有没有新帧」的唯一依据，先写会导致读到
   半帧像素。
5. 共享文件格式与 `lib/dsh_gl.h` 完全一致，App 侧 `LinuxFrameSurface.kt`
   直接复用，无需改动。

**实测输出**

```
MONITOR_OK 127.0.0.1:4470
CAPTURE 1280x800 ppm=3072016B
RESULT: QSCREENSRV_STARTED
共享文件 magic=0x4D415246 seq=247 w=1280 h=800 bytes=4096016
```

`4096016 = 16 + 1280×800×4` ✓。非零像素 37666，**alpha≠255 的像素 0 个**。
解成 PNG 后可读看到 guest 的 fbcon 全屏内容（内核日志、`/proc/fb`、
`/dev/fb0` 设备节点行全部清晰）。

**输入注入验证（像素级）**

QEMU monitor 的 `sendkey` / `mouse_move` / `mouse_button` 直通。

判据**不能是「命令返回 OK」** —— 那只能证明 monitor 收下了命令。
唯一可信判据是注入前后各抓一帧、比像素：

```
注入 h e l l o 前后：
  不同像素数: 174
  变化区域:   x 0..47   y 594..607     ← 正好一个字符格宽度
  变化颜色:   aaaaaa(158) + 000000(16)
```

`#AAAAAA` 是 fbcon 默认前景色、`#000000` 是背景 —— 说明屏幕真的多出了
6 个字符格的字形，不是光标闪烁（光标只有 1 格）。

**PNG 导出的一个坑**（不是 QEMU 的问题但踩了）
共享文件是 RGBA（4 字节/像素），写 PNG 时 `color type` 必须用 **6**（RGBA）。
用 2（RGB）会得到「Unsupported or malformed image data」，两个看图器都打不开，
而 PNG 结构与 CRC 全都合法 —— **又一次「结构正确 ≠ 内容正确」**。

### 6.22 qscreensrv 修复后的干净复测（可复现脚本 `bench/qsrv_verify.sh`）

**上一轮数字的可信度问题**（审查时发现，必须记下来）：

`§6.21` 里的 9.0ms/帧、alpha≠255=0、seq=247 这些数字，当时是手工逐步操作得出的，
**没有入库的原始输出与检查脚本**。而且「alpha≠255 的像素 0 个」在 `qscreensrv`
产物上是**构造性必然**（源码每个像素都写 255），当成「实测发现」写会误导。
`pmemsave` 的 16777216 B 也是脚本里固定 `0x1000000` 的 dump 长度，不是
「guest 实际 framebuffer 大小」的独立测量 —— 两者必须分开表述。

**修完 4 处缺陷后的干净复测**（脚本 `bench/qsrv_verify.sh`，先按 PID 精确
清场并确认 0 残留）：

```
=== 1) 清场 ===
  leftover=0
=== 5) 轮询等共享文件 ===
  SHARE READY after 0s
=== 6) 共享文件头 ===
-rw-r--r-- 1 root root 4096016 2026-11-07 21:34
0000000 1296126534         19       1280        800
=== 7) seq 增量（3 秒）===
  seq 19 -> 49
  delta=30
=== 9) 判定 ===
RESULT: QSRV_OK seq_delta=30
```

`delta=30 / 3 秒 = 10fps`，与设定一致 —— **这是「服务活着」的硬证据**，
比「文件存在」或「日志有 STAT」强得多。

拉回共享文件解析：

```
magic=0x4D415246 seq=151 w=1280 h=800 size=4096016 expect=4096016
非零像素=42144  alpha!=255=0
```

解成 PNG（color type **6**，RGBA）后可读看到 guest 全屏 fbcon 内容，
`/dev/fb0` 设备节点行、`[drm] Found bochs VGA` 等全部清晰。

**两个验证工具坑（都不是被测代码的问题）**

1. `busybox od` **不支持 `-j` 跳偏移**，静默返回空串 → 读共享文件头必须用
   `dd bs=1 skip=4 count=4`。
2. 旧进程没杀掉时，会往已被 `>` 截断的**同一日志**按旧偏移续写 → 日志出现
   NUL 空洞、`seq` 从旧值（9638）续起，看起来像「服务正常」，实际是两次运行
   混在一起。**验证前必须确认 `leftover=0`**。

**PNG color type（第三次踩）**
共享文件是 RGBA（4 字节/像素），写 PNG 时 `color type` 必须是 **6**。
用 2（RGB）会得到「Unsupported or malformed image data」，而 PNG 结构与
CRC 全部合法 —— 又一次「结构正确 ≠ 内容正确」。

### 6.23 dshgui vm-* 统一入口（AI 侧 API 的实测数据）

原先有两条割裂的路径：`dshgui` 管 X11 程序，QEMU 虚拟机要自己写 monitor
命令加 `qscreensrv`。AI 记两套容易错，而且 vm 那条路要自己算相对位移。
现已并入同一个入口，X11 与虚拟机两套环境互不干扰。

| 命令 | 实测输出 | 说明 |
|---|---|---|
| `vm-start` | `started pid=13543 monitor=127.0.0.1:4493` | 端口自动顺延（4492 可能撞 TIME_WAIT） |
| `vm-status` | `running`，`P6 1280 800 255` | PPM 头 3 行，只看前 2 行会漏分辨率 |
| `vm-capture` | `captured 10986 bytes` | 1280×800 PNG |
| `vm-key h e l l o` | 变化 **158** 像素，`x 0..47 y 594..607` | 6 格 × 8px = 48px ✓ |
| `vm-type "OK"` | 变化 **116** 像素，`x 40..63 y 594..607` | 接上 48px 之后，24px = 3 格 |
| `vm-stop` | `stopped`，`after-stop=0` | 按 PID 精确杀 |

**判据设计**：两次注入的变化区域在 x 轴上**准确衔接**（0..47 → 40..63），
这证明字符是**顺序追加**的，而不是覆盖同一格或光标闪烁。
只测「一次注入有变化」不够 —— 光标闪烁也会产生变化，且只占 1 格。

**四个设计取舍（都踩过）**

1. **取画面用 `screendump` 不用 VNC** —— VNC 在 guest 未 `drmModeSetCrtc`
   时报 `desktop 0x0`，只写出 16 字节帧头。多次实测都如此（QEMU 10 + `-vga std`）。
2. **`screendump` 的文件参数必须写相对名** —— HMP 把路径里的 `/` 当**除号**
   解析，报 `invalid char 't' in expression`。
3. **`vm-stop` 按 PID 精确杀** —— `pkill -f qemu-system` 会匹配到调用它的
   脚本自身（命令行里含该串），实测把整个 `.sh` 一起杀掉，表现为「脚本无输出」。
4. **`vm-click` 用「先大力回左上角再相对移动」模拟绝对定位** ——
   HMP 的 `mouse_move` 本身只有相对位移，没有绝对定位命令。

**仍未验证**：`vm-click` 在 fbcon 下**没有可观测判据**（guest 没有图形桌面，
鼠标事件无人消费）。要等 guest 里跑起 X/Wayland 才能证明点击真的生效。
这一条明确标注为未验证，不算成果。

### 6.24 vm-click 打通：四层障碍与 PS/2 包级判据

这一项之前被我标成「未验证」，理由是「fbcon 下没有可观测判据」。**那个判断只对了一半** ——
真正的问题是有四层障碍没排掉，而不是没有判据。

**四层障碍（依次踩穿）**

| 层 | 症状 | 根因 | 修法 |
|---|---|---|---|
| 1 | guest 里没有鼠标设备 | QEMU 默认不给 | `-device usb-tablet` |
| 2 | 加了 usb-tablet 还是没有 | `qemu-xhci` 在 TCG+SeaBIOS 下**内核枚举不到**（`usbcore: registered new interface driver usbhid` 有，但设备列表空） | 改 `-device usb-ehci` |
| 3 | usb-ehci 还是没有 | 这套内核的 USB/输入栈**全是 `=m`**：`ehci-hcd` `ehci-pci` `hid` `usbhid` `hid-generic` `mousedev` `evdev` | initrd 里按顺序 insmod |
| 4 | 设备都在，读不到事件 | **读错了文件**：`/dev/input/event1`（evdev）在这套配置下不产事件 | 读 `/dev/input/mice`（PS/2 协议） |

**判据：guest 侧 PS/2 包级解码**

单次左键点击后，guest 的 `/dev/input/mice` 读出：

```
原始字节序列: 9 0 0 8 0 0
→ 2 packets: [9:L,0,0] [8:release,0,0]
RESULT: VMCLICK_VERIFY_OK packets=2
```

PS/2 鼠标包 3 字节，`byte0` 位含义：
- `bit0` 左键、`bit1` 右键、`bit2` 中键
- `bit3` **固定同步位**（恒为 1，用来识别包边界）

所以 `0x09 = 0b1001` = 同步位 + 左键按下；`0x08 = 0b1000` = 同步位 + 全部松开。
dx/dy 都是 0 —— `usb-tablet` 是绝对坐标设备，不发相对位移。

**判据强度**：单次点击**恰好 2 个包**，不是 1 个也不是 3 个。
这个计数精确性才是判据，比「读到非零字节」强得多。

**★一个假绿教训**

上一版判据写成 `EV=$(...| tr -d ' \n')`，把 timeout 打印的 `Terminated`
也算成事件字节，得到 `event_bytes=11` **看起来像成功**。
改成只认数字并剔除 `od` 的偏移列之后，非零项计数才是真事件。
**凡是拿文本输出当数值判据，都必须显式剔除非数值字符。**

### 6.25 端到端验收：AI 在手机容器内完成完整工作流

这一节回答「三项目标能不能串起来用」，而不是分别证明三项能力存在。

**流程与产物**

| 步 | 动作 | 结果 |
|---|---|---|
| 1 | 写 Xlib 程序 | 2077 字节源码（400×220 窗口，160×50 按钮） |
| 2 | 容器内 gcc 编译 | **19112 字节二进制**（musl + libX11） |
| 3 | `dshgui start 800x600x24` | `started :99`，`800 600` |
| 4 | 跑程序 | `dshgui windows` → `WID=2097153 name=[E2E] WINDOW=2097153 X=100 Y=100 WIDTH=400 HEIGHT=220` |
| 5 | `dshgui capture` 截图 A | 793 字节 PNG |
| 6 | `dshgui click 200 215` | `clicked 200,215 x1` |
| 7 | `dshgui capture` 截图 B | 776 字节 PNG |
| 8 | 像素比对 | 变化 **7754** 像素，区域 `x 122..280 y 133..240` |

**判据强度：颜色值在源码里写死，不靠人眼**

```c
XSetForeground(d, gc, clicked ? 0x00CC44 : 0xCC2222);   // 绿 / 红
```

按钮中心 `(200,215)` 的实测：

```
before RGB = (204, 34, 34)   ← 0xCC2222
after  RGB = (0, 204, 68)    ← 0x00CC44
```

三个值（`204,34,34` / `0,204,68`）与源码写死的十六进制**逐位对应**，
不可能碰巧相等。加上变化区域严格覆盖按钮矩形（屏幕 `120..280 / 190..240`），
这是「点击确实被 guest 消费并触发了程序逻辑」的完整证据。

区域上沿到 133（不是 190）是同时变化了 `state=0 → state=1` 标签文字，
属预期，不是干扰。

**为什么用 raw Xlib 而不是 GTK**

`gtk3` 没装，装它要一百多 MB；`libx11-dev` 只有几 MB 且 `libX11` 随 xvfb 已在。
更关键的是 raw Xlib 几十行就够，按钮颜色固定，diff 区域能**精确对上**按钮矩形 ——
判据更干净，不依赖主题/字体/抗锯齿的随机性。

**这条链路对「手机端工作站」的意义**

用户/用户委托的 AI 可以在手机上完成：写代码 → 编译 → 运行带界面的程序 →
看到画面 → 点按钮 → 确认程序响应。全程不需要电脑，也不需要 root。

### 6.22 容器内跑真实桌面应用：NetSurf 浏览器 + Geany IDE

前面的 xterm / xclock / xcalc / xeyes 是**最小 X11 客户端** —— 它们只能
证明 X 协议通、能画窗口、能收点击。要做到「手机端工作站」，必须证明
**真实桌面应用**能跑，因为它们的依赖面完全不同：

| 依赖 | 最小 X11 客户端 | 真实桌面应用 |
|---|---|---|
| X11 协议 | ✅ 需要 | ✅ 需要 |
| 字体栈（fontconfig + FreeType + 字体文件） | 部分（内嵌位图字体） | ✅ 必须 |
| GTK/主题引擎 + 图标主题 | ❌ 不需要 | ✅ 必须 |
| 文件选择器 / GIO | ❌ | ✅ 必须 |
| 会话/配置目录 | ❌ | ✅ 必须 |
| 剪贴板 | ❌ | ✅ 必须 |

**实测结果**

| 软件 | 体积增量 | 窗口 | 渲染证据 |
|---|---|---|---|
| `netsurf`（自带 HTML/CSS 引擎的浏览器） | +11 MiB（已在 2167 MiB GTK 基础上） | `[Netsurf on Android - NetSurf]` 1000×700 | 截图 37157 B：蓝色 40px 标题、带 2px 描边和 10px 圆角的卡片、粉色加粗行内文字、带表头底色的表格、地址栏与 `Done (0.0s)` 状态栏 |
| `geany`（GTK 代码编辑器/IDE） | +21 MiB | `[demo.c - /tmp - Geany]` 900×600 | 截图 50425 B：9 项菜单栏、左侧 Symbols 树解析出 `fib [4]` / `main [8]`、行号列、C 语法高亮（`#include` 蓝、字符串橙、注释红）、行内光标、底部 Message 面板 `File /tmp/demo.c opened (1).` |

**为什么 chromium 不行（诚实记录）**

`chromium` 装好了（2167 MiB / 414 包，`rc=0`），`--version` 能输出
`Chromium 142.0.7444.59`，窗口也能创建（`[Untitled - Chromium]` 1279×799），
但**渲染不出内容**：

```
LaunchProcess: failed to execvp:
/proc/self/exe
[pid:tid:...:ERROR:content/browser/network_service_instance_impl.cc:613]
    Network service crashed, restarting service.
```

根因是 chromium 的多进程架构用 `/proc/self/exe` 拉起 network service 子进程，
而容器里 `/proc` 整棵前缀是**透传宿主**的（路径翻译层的设计，使 realpath /
inotify 能在容器语义下工作），`/proc/self/exe` 解析出的目标不可 exec。
`--no-zygote --single-process` 也无法绕过（network service 仍独立 spawn）。

这不是依赖缺失，是**多进程 + 容器路径语义**的架构冲突。要解决需要给
chromium 提供它期望的进程启动路径（例如把 `/proc/self/exe` 在容器内映射成
真实可执行文件），属于专门适配，不在本轮范围。

**AI 入口**

```sh
dshgui demo-list        # 列出已装/未装的图形软件
dshgui demo netsurf     # 浏览器渲染本地页面
dshgui demo geany       # IDE 打开 demo.c
```

`cmd_demo` 的三个设计约束（探针 `scripts/probes/_test_dshgui_demo.py` 22 条断言固定）：
1. **未安装时给可操作提示**（`apk add <name>`）并返回退出码 3，不是静默失败
2. **Xvfb 没起时自动拉起**，不要求调用方先手动 `dshgui start`
3. 前置文件（`/tmp/site/index.html`、`/tmp/demo.c`）**只在不存在时创建**，
   绝不覆盖用户改过的内容

---

## 17. 容器内 QEMU 引导 Android-x86：里程碑与耗时

在 dsh-linux-runtime 容器里（Alpine 3.22 x86_64，seccomp 容器）用
`qemu-system-x86_64 10.0.0` 引导 Android-x86 9.0-r2，走 `TCG`（无 KVM）。

### 环境

| 项 | 值 |
|---|---|
| 设备 | 模拟器 `emulator-5554`，AVD dsh34，Android 14，x86_64，API 34 |
| 设备内存 | 7812 MB（AVD 从 1536M 调到 8192M 后生效，`/proc/meminfo` 实测） |
| guest 内存 | 4096 MB（`-m 4096`） |
| vCPU | 2（`-smp 2 -cpu qemu64`） |
| 加速 | **无**（`/dev/kvm` 不存在，`cpuinfo` 无 `vmx`） |
| ISO | `android-x86_64-9.0-r2.iso`，965738496 B |
| QEMU | `apk add qemu-system-x86_64` → 10.0.0-r1，81 MiB / 63 packages |

### 里程碑时间（串口 `console=ttyS0` + VGA `console=tty0`）

| 秒 | 事件 |
|---|---|
| 0.00 | `Linux version 4.19.110-android-x86_64-g066cc1d` |
| 0.86 | `ata2.00: ATAPI: QEMU DVD-ROM` / `sr0` 识别 |
| 1.00 | `Run /init as init process` |
| ~11 | `input: AT Translated Set 2 keyboard` |
| ~60.2 | `e1000: Intel(R) PRO/1000 Network Driver` / `eth0` |
| ~60.7 | `input: PC Speaker` |
| ~62.0 | `input: ImExPS/2 Generic Explorer Mouse` |
| ~62+ | 分辨率 720×400(文本) → 1024×768(图形)，`switch_root /android /init` |

之后 Android init 清屏，CPU 保持 2 vCPU 满载（实测每 60 秒推进 ~27 秒
utime+stime），符合首启 `dex2oat` 特征，**未等到桌面画面**。

### 内存对引导的影响（同一命令只改 `-m`）

| guest 内存 | 现象 |
|---|---|
| 1024 MB | 能引导到 `found at /dev/sr0`，但挂载 942 MB `system.sfs` 极慢（page cache 反复回收） |
| 4096 MB + `-smp 4` | **`Kernel panic - not syncing: IO-APIC + timer doesn't work!`** |
| 4096 MB + `-smp 2` + `noapic` | 正常引导，硬件全识别，进图形模式 |

### 宿主内存的真实数字（澄清一个常见误读）

```
宿主物理内存:              31.8 GB
AVD config.ini 原始:       hw.ramSize = 1536M   ← 只用了 4.8%
改后:                      hw.ramSize = 8192M
guest 内 /proc/meminfo:    MemTotal = 8138296 kB = 7.8 GB   ✓ 生效
宿主任务管理器:
  emulator.exe             0.01 GB   ← 只是外壳
  qemu-system-x86_64       6.82 GB WorkingSet / 2.00 GB Private
```

任务管理器里看到「1.3 GB」是因为 `emulator.exe`（外壳）与
`qemu-system-x86_64-headless`（真正干活）是**两个进程**，且「内存」列
显示的是私有工作集而非提交量。判断内存是否生效**以 guest 内
`/proc/meminfo` 为准**。

### 取画面的两种方式

| 方式 | 产物 | 适用 |
|---|---|---|
| `screendump` (monitor) | PPM，9 ms/帧 | 常态；文本/图形模式都能取 |
| `pmemsave` (monitor) | 裸 framebuffer | 需要绕过 VGA 时；**路径必须相对**（HMP 把 `/` 当除号） |

`-display none` 下 `screendump` 仍可用，无需 VNC。

### 结论

容器内引导 Android-x86 的**链路是通的**（内核、设备、显示、移交 Android
全部实证）。剩余「看到桌面」这一步的瓶颈是 **TCG 纯软件模拟下的
`dex2oat`**，不是参数或配置问题。droideum-termux 在同类环境下对这一步
的结论是 `the tool works; this host is too small to finish the last step`。

要在手机上获得可用的 Android-in-Android 体验，正确路径是 **arm64 真机
直接跑**（零套娃），而非在模拟器里再套一层。

---

## 18. 触摸回传：容器画面可控（XTEST 长连接，p50 = 1.3 ms）

让 App 画面上「点一下」真的作用到容器里的程序。两条 compositor 路线都实测
否证后，最终落定 X11 + XTEST。

### 为什么不是 Wayland

生产画面路径原本是 sway（Wayland）+ wlshot，逐个试过三条注入路，全部失败：

| 方案 | 结果 | 根因 |
|---|---|---|
| `wlrctl`（wlroots 官方 virtual-pointer/keyboard） | `rc=0` **静默成功但事件不到** | 需要 wlroots 虚拟设备协议参与握手，且多次 `Assertion failed: state.display (../main.c: main: 177)` 后 `Aborted` |
| `uinput`（`/dev/uinput`，`CONFIG_INPUT_UINPUT=y`） | 设备创建成功，容器内 `/dev/input/event13 = dsh-touch` 真实出现，**但 `swaymsg -t get_inputs` 返回 `[]`** | ① `WLR_BACKENDS=headless` **只创建输出、不扫描任何输入设备**（`sway -d` 日志里 libinput 出现 0 次）② 换 `WLR_BACKENDS=libinput` 起不来 |
| 给 sway 加 seat | 起不来 | `libseat`：`Could not connect to socket /run/seatd.sock` / `Backend 'seatd' failed to open seat` / `Backend 'logind' failed to open seat` / `No backend was able to open a seat` —— **libinput 后端必须先有 seat，seat 只能来自 seatd 或 logind**，容器里两者都没有 |

附带收获：uinput 创建的设备**不会被 Android 宿主看到**（`dumpsys input` 里
`grep -c dsh-touch` = 0、`getevent` 设备数不变 13），所以这条路即便走通也
不会污染宿主输入。

另注：`WLR_LIBINPUT_NO_DEVICES=1` 是「无输入设备也能起 headless sway」用的，
设了它等于**主动让 sway 忽略所有输入设备**，排查时容易误判成「设备没被认到」。

### X11 路线：两个程序就够

| 程序 | 作用 | 产物 |
|---|---|---|
| `bench/x11frsrv.c` | `XShmGetImage`（回落 `XGetImage`）连续抓 X 根窗口 → 写共享文件 | 19504 B |
| `bench/inputd.c` | 常驻 XTEST 长连接，轮询共享文件 → 注入 motion/button | 19248 B |

两者的共享文件协议与既有 `framesrv.c` **完全一致**（`magic=0x4D415246` /
seq / w / h / RGBA8888），所以 App 侧 `LinuxFrameSurface` 一行不用改。

`inputd` 的协议是另一份（`magic=0x54504E49 "INPT"`）：头 32 字节
（magic/head/tail/capacity/flags）+ 每条 16 字节（type/x/y/buttons）。
`head`/`tail` **单调递增不取模**，取模只用于定位条目 —— 满/空只看减法，
不会因 32 位回绕判错；队列满时覆盖最旧的（生产者永不阻塞 UI）。

### 实测

**抓帧延迟**（1280×720 / 60fps 目标，5 秒采 seq 增量）：

```
5s 内 seq: 111 -> 390  实际 fps=55
share=3686416 期望=16+1280*720*4=3686416
```

无 XShm 也能到 55 fps（`shm=0`，Xvfb 的 MIT-SHM 用不上，走 `XGetImage`）。

**注入延迟**（30 次，写事件 → 目标程序收到）：

```
LAT n=30  min=853us  p50=1339us  max=4592us
```

**中位 1.3 毫秒**，比每次起 `xdotool`（实测约 200ms）快约 150 倍。

**坐标精确性**（内容窗口在 (10,10)）：

```
写入：MOVE(200,150) → DOWN(200,150) → MOVE(250,180) → UP
收到：MOVE x=190 y=140 / PRESS x=190 y=140 btn=1 / MOVE x=240 y=170 / RELEASE x=240 y=170
```

**端到端像素证据** —— App 会看到的帧里（放大后清晰可读）：

```
CONTAINER READY
x86_64
3.22.2
~ # AI-TOUCHED-THIS-WINDOW
/bin/sh: AI-TOUCHED-THIS-WINDOW: not found     ← 程序真的执行了
~ # echo FRAME-SYNC-OK
FRAME-SYNC-OK                                   ← 输出回到帧里
~ #
```

### 三个实现要点（都是踩过才知道）

1. **alpha 必须显式写 255**。Xvfb 24bpp 是 BGRX 排布，第 4 字节是 `unused`
   不是 alpha，实测可能全 0 —— App 侧 `ARGB_8888` 会当真读，整帧被判全透明，
   用户看到一片空白且**不报任何错**（这个坑在 wlshot 上踩过一次）。
2. **seq 最后推进**。先推 seq 会让 App 读到半帧。
3. **Xvfb 的 abstract socket 残留**。上一实例没退干净时新实例报
   `Fatal server error: Cannot establish any listening sockets`，而此时
   `/tmp/.X11-unix/` 目录看起来是空的 —— 必须 `rm -rf /tmp/.X11-unix` 重建，
   换 display 号只是绕过、不是修复。

### 相关

- 新增 `app/src/main/java/app/dsh/mobile/engine/LinuxInputChannel.kt`（协议实现
  + 坐标换算）、`LinuxActivity` 的触摸开关、`LinuxEnvironmentManager` 的
  `installLinuxTool()` 部署（幂等戳带字节数）
- 同族：pitfalls #30（软链绝对路径）、#64（Android-x86 引导）
