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
