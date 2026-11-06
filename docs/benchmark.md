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
