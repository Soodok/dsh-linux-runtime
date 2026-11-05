# 性能测量方法与数据

## 测量纪律（这几条都是踩出来的）

1. **在设备内计时**，不要用 adb 往返 —— 协议开销会淹没毫秒级差异。
2. **不要用 shell 循环计时**。`/proc/uptime` 精度受 `HZ=100` 限制（10ms 粒度），
   容器内 busybox 的 `date` **不支持 `%N`**（`date +%s%N` 只输出秒）。
   正确做法：**批 N 次求总时再平均**，或程序内 `clock_gettime(CLOCK_MONOTONIC)`。
3. **基线必须验证「真的跑成功」**。我曾用「宿主直接跑 `cd $R && ./bin/busybox`」
   当基线得 11ms，宣称「慢 7~9 倍」—— 那个 baseline 是假的：bionic 链接器跑不了
   musl 二进制，11ms 只是失败命令的耗时。
4. **异常好的结果先当可疑**。首次测 PRoot 启动得 2~3ms，差点当成「PRoot 更快」，
   加退出码检查发现全是 `rc=127`（exec 失败）。
5. **交替轮次取最小值/中位数**，不要所有 A 跑完再所有 B —— 设备会热、会限频。
6. **杀净旧进程**。`pidof app.dsh.mobile` 杀不死，需反复 `kill -9` 到 `ps` 为空；
   端口判就绪只认 `LISTEN` 且核对监听者 PID（`netstat | grep 3080` 会匹配到
   TIME_WAIT 残留行，给出假数据）。

## 基准代码

进程内自计时，输出 `key=value` 便于脚本解析：

```c
// bench_full.c 摘要
#define CLOCK CLOCK_MONOTONIC
static double now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
// exec:   fork + execv("/bin/true") + waitpid
// io:     open+read+close，300 轮
// stat:   600 轮
// getpid: 6000 轮
// cpu:    2000 万次 sqrt 累加（零 syscall）
// dir:    readdir 遍历 /usr/bin
// write:  write 到 /dev/null
```

## 原始数据

设备：OPPO PLZ110 / Android 16 / 骁龙 8 Elite / 8 核（厂商限频在 ~55%，
所以绝对值偏保守；同一轮内三方对比仍然有效）。

### syscall 级（µs/次，5 轮交替取中位）

| 操作 | 原生 chroot | dsh_alpine | PRoot |
|---|---|---|---|
| getpid | 0.002* | 0.27 | 0.39 |
| stat | 1.9~2.4 | 2.7~3.6 | 3.2~4.4 |
| open+read+close | — | 11.6 | 7.1 |
| readdir（603 项） | — | 285.7 | 257.7 |

\* 原生无过滤器，getpid 直接进内核。

### 批量（ms）

| 操作 | 原生 chroot | dsh_alpine | PRoot |
|---|---|---|---|
| fork+exec+wait ×60 | 0.65~0.79 | 0.97~1.26 | **6.28~8.69** |
| 文件 IO ×20000 | 62 | 116~164 | **4104~4920** |
| CPU（2000 万次 sqrt） | 101~110 | 124~133 | 105~133 |

### 真实负载

| 负载 | dsh_alpine | PRoot | 倍数 |
|---|---|---|---|
| SQLite 3.54.1 `-O2` 编译 | **38.6 s** | 49.2 s | 1.27× |
| SQLite 全量链接 | **47.7 s** | 51.3 s | 1.08× |
| 30 个 .c 编译+链接 | **1.27~1.40 s** | 2.50~4.23 s | 1.8~3.3× |
| git 40 次 commit | **1.17 s** | 3.20 s | 2.7× |
| tar 打包 300 文件 | **0.05 s** | 0.23 s | 4.6× |
| cp 300 文件 | **0.65 s** | 1.80 s | 2.8× |

**产物一致性**：SQLite `-O0` / `-O2` 产物 sha256 与 chroot 侧完全相同
（`7a6bace197c4` / `515bba8fdb83`）—— 编译结果可复现。

### 启动

| 阶段 | 耗时 |
|---|---|
| 进容器执行一条空命令 | 2.4 ms（chroot 1.6 ms，PRoot 19~33 ms） |
| `node -e 0` | 74 ms（容器层） |
| 完整引擎 web 就绪 | ≈1430 ms（其中容器层仅 5%，86% 是引擎加载插件） |

### 内存

| 对象 | VmRSS |
|---|---|
| 容器 loader 进程 | 1836 kB |
| 宿主原生 `sh` | 5396 kB |
| 容器内 Python 启动 | 8172 kB（chroot 7224 kB，差 +948 kB） |
| 容器内 DSH 引擎 | 171 MB（原生模式 235 MB） |

**容器层固定开销 ≈ 0.8~1.0 MB**；分配 200MB 大内存时容器与 chroot 差 +820 kB，
**无放大**。`VmSize` 里 2GB+ 是固定基址的保留映射，不是实占。

### 硬件 GPU（bionic 进程级桥）

同一程序、同参数（10000 三角形 ×20 帧 @1024²）：

| 后端 | 每帧 |
|---|---|
| Adreno 840（硬件） | **0.74~1.76 ms** |
| llvmpipe（软件） | 26.3~30.5 ms |

差 **35×**。`glReadPixels` 1024²×10：硬件 1.4~2.2 ms（1792~2862 MB/s）。

## 怎么复现

```bash
# 设备侧
adb push bench_full.c /data/local/tmp/
adb shell "su -c 'chmod 644 /data/local/tmp/bench_full.c'"
# 容器内编译
dsh_alpine --dsh-reexec-v1 --rootfs $R --interp /lib/ld-musl-aarch64.so.1 \
  --guest /usr/bin/gcc /usr/bin/gcc -O2 -o /tmp/bench /tmp/bench_full.c
# 跑（rounds 越大越稳）
dsh_alpine ... --guest /tmp/bench /tmp/bench 300
```
