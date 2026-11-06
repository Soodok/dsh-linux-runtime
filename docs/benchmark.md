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
