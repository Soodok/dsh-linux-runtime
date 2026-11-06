/*
 * dsh_gl_pixmode.c —— 验证 dsh_gl 库三条取像素路径（rpix / ahb / auto）
 *
 * 这个程序存在的意义：产线 `framesrv.c` 已经能用 AHB，但那是**一个程序**的
 * 实现。`lib/dsh_gl` 是给容器内任意 GL 程序用的库，它支持 AHB 才算真正
 * 「容器里所有 GPU 程序都能吃到零拷贝收益」。本探针验证库这一层。
 *
 * 用法（必须经 sh -c，让容器内再发一次 execve 命中 cleaner）：
 *   /dsh_gl_pixmode <sharepath> <rpix|ahb|auto> [w] [h] [frames]
 *
 * 输出契约（供脚本断言）：
 *   DSH_GL pixmode=<rpix|ahb> req=<mode> w=<w> h=<h>
 *   DSH_GL present frames=<n> ms=<总毫秒> fps=<x>
 *   DSH_GL center=<r,g,b,a>
 *   RESULT: DSH_GL_PIXMODE_OK
 *
 * 期望：
 *   - req=rpix  -> 实现必为 rpix
 *   - req=ahb   -> 实现可能为 ahb（有 gralloc）或 rpix（自动退回，不算失败）
 *   - req=auto  -> 同 ahb
 *   - 三种模式写到共享文件的像素必须一致（同一场景、同一帧数）
 */
#include "dsh_gl.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1e6;
}

int main(int argc, char **argv) {
    const char *share = (argc > 1) ? argv[1] : "/data/data/app.dsh.mobile/files/glpix.bin";
    const char *mode = (argc > 2) ? argv[2] : "auto";
    int w = (argc > 3) ? atoi(argv[3]) : 640;
    int h = (argc > 4) ? atoi(argv[4]) : 360;
    int frames = (argc > 5) ? atoi(argv[5]) : 60;

    int pixmode = DSH_PIX_AUTO;
    if (strcmp(mode, "rpix") == 0) pixmode = DSH_PIX_RPIX;
    else if (strcmp(mode, "ahb") == 0) pixmode = DSH_PIX_AHB;

    dsh_frame f;
    int rc = dsh_frame_open_ex(&f, w, h, share, pixmode);
    if (rc != 0) {
        dsh_printf("FAIL dsh_frame_open_ex rc=%d err=%s\n", rc, dsh_gl_error());
        dsh_write("RESULT: DSH_GL_PIXMODE_FAIL open\n");
        return 3;
    }

    dsh_printf("DSH_GL pixmode=%s req=%s w=%d h=%d stride=%u\n",
               dsh_frame_pixmode_name(&f), mode, w, h, f.ahb_stride);

    void *gl = f.gl_lib;
    void (*pViewport)(int, int, int, int) = dlsym(gl, "glViewport");
    void (*pClearColor)(float, float, float, float) = dlsym(gl, "glClearColor");
    void (*pClear)(unsigned) = dlsym(gl, "glClear");

    /* 固定场景：纯色清屏。三种模式必须写出完全相同的像素，
       这样「快」才不是以「画面不同」换来的。 */
    pViewport(0, 0, w, h);

    double t0 = now_ms();
    int i;
    for (i = 0; i < frames; i++) {
        pClearColor(1.0f, 0.35f, 0.1f, 1.0f);
        pClear(0x4000);
        if (dsh_frame_present(&f) != 0) {
            dsh_printf("FAIL present at frame %d: %s\n", i, dsh_gl_error());
            dsh_frame_close(&f);
            dsh_write("RESULT: DSH_GL_PIXMODE_FAIL present\n");
            return 4;
        }
    }
    double el = now_ms() - t0;

    dsh_printf("DSH_GL present frames=%d ms=%.0f fps=%.1f\n",
               frames, el, frames * 1000.0 / el);

    /* 中心像素：橙色 (255, 89, 26, 255) */
    {
        unsigned char *px = f.pixels;
        unsigned off = (unsigned)(h / 2) * (unsigned)w * 4u + (unsigned)(w / 2) * 4u;
        dsh_printf("DSH_GL center=%u,%u,%u,%u\n", px[off], px[off + 1], px[off + 2], px[off + 3]);
        if (!(px[off] > 200 && px[off + 1] > 60 && px[off + 1] < 120 && px[off + 3] > 200)) {
            dsh_frame_close(&f);
            dsh_write("RESULT: DSH_GL_PIXMODE_FAIL pixel\n");
            return 5;
        }
    }

    dsh_frame_close(&f);
    dsh_write("RESULT: DSH_GL_PIXMODE_OK\n");
    return 0;
}
