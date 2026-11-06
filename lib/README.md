# lib/dsh_gl —— 在免 root 容器里用宿主 GPU 渲染

一个 3 文件的小库，让任何 C/C++ 程序在 DSH 的免 root Linux 容器里用上
手机的硬件 GPU，并把画面送回 Android App 显示。

## 为什么需要它

容器里跑图形程序的难点不在「能调 GL」，而在三件容易被踩死的事：

1. bionic 非 zygote 进程**不能静态链接 libEGL、不能用 printf**，否则
   `FORTIFY: pthread_mutex_lock called on a destroyed mutex` 直接 abort；
2. 容器程序要把画面交给 App，**不能用 memfd**（`/proc/<pid>/fd` 只有 root 可读）；
3. `vbo` 初值 0 是 GL 的「默认缓冲」，`glBufferData` 对它**静默失败**，
   表现为「程序链接成功但画面只有清屏色」。

这个库把这三条都封在里面了。

## 用法

```c
#include "dsh_gl.h"

dsh_frame f;
if (dsh_frame_open(&f, 1280, 720, "/data/data/<pkg>/files/dshframe.bin") != 0) {
    dsh_printf("open failed: %s\n", dsh_gl_error());
    return 1;
}
while (running) {
    /* 你的 GL 代码，画在默认 framebuffer 上 */
    glClearColor(...); glClear(...); glDrawArrays(...);
    dsh_frame_present(&f);      /* glFinish + glReadPixels + 推进 seq */
}
dsh_frame_close(&f);
```

## 编译

```sh
aarch64-linux-android28-clang -O2 -o example example.c dsh_gl.c -ldl
x86_64-linux-android28-clang  -O2 -o example example.c dsh_gl.c -ldl
```

**必须 `-ldl`，不能加 `-lEGL -lGLESv2`**（原因见上第 1 条）。

## 在容器里运行

```sh
dsh_alpine <rootfs> <interp> /bin/sh -c \
  '/example 1280 720 300 /data/data/<pkg>/files/dshframe.bin'
```

**必须经 `sh -c`**：直接 `dsh_alpine <rootfs> <interp> /example` 会按 musl
装载，报 `Error relocating /example: __libc_init: symbol not found`。只有让
容器内再发一次 `execve`，才会命中 `guest_is_bionic()` 走 cleaner 代跑。

程序要放在 rootfs 内（如 `/example`）或 bind 进来的目录。放
`/data/local/tmp` 不行——容器看不到那个路径。

## 共享文件格式

```
offset 0   u32 magic = 0x4D415246（小端落盘即 "FRAM"）
offset 4   u32 seq     每帧 +1，宿主靠它判「有没有新帧」
offset 8   u32 w
offset 12  u32 h
offset 16  起为像素，RGBA8888，w*h*4 字节
```

App 侧由 `dsh-android/app/src/main/java/app/dsh/mobile/engine/LinuxFrameSurface.kt`
解析，改格式必须两侧同步（主仓 `scripts/probes/_test_linux_frame_surface.py`
的 38 条断言会拦）。

## 实测

模拟器 dsh34（x86_64 / SwiftShader）：

```
DSH_FRAME_GL w=1280 h=720 renderer=Android Emulator OpenGL ES Translator (Google SwiftShader)
done frames=30 seq=30
共享文件 3686416 B = 16 + 1280*720*4 ✓
回读帧中心像素 (95,211,89)，三角形区域纯色、背景渐变 —— 视觉确认全链路正确
```

真机对应 `Adreno (TM) 840`，同一条通路。
