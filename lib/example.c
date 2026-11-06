/*
 * example.c —— dsh_gl 的最小使用示例（也是它的入库测试）
 *
 * 编译（必须 -ldl，不能链 libEGL/libGLESv2）：
 *
 *   aarch64-linux-android28-clang -O2 -o example example.c dsh_gl.c -ldl
 *   x86_64-linux-android28-clang  -O2 -o example example.c dsh_gl.c -ldl
 *
 * 在容器里跑（★必须经 sh -c，否则按 musl 装载会 __libc_init 失败）：
 *
 *   dsh_alpine <rootfs> <interp> /bin/sh -c \
 *     '/example 1280 720 300 /data/data/<pkg>/files/dshframe.bin'
 *
 * 画面内容：一个绕 Z 轴旋转的彩色三角形，背景是随时间推移的渐变。
 * 它同时验证了着色器编译、VBO 上传、draw call、glReadPixels 全链路。
 */
#include "dsh_gl.h"

#include <dlfcn.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

/* GLES 常量（不引系统头，跨架构通用） */
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_TRIANGLES 0x0004
#define GL_FLOAT 0x1406
#define GL_ARRAY_BUFFER 0x8892
#define GL_STATIC_DRAW 0x88E4
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82

static const char *VS =
    "attribute vec2 p;\n"
    "uniform float a;\n"
    "void main(){\n"
    "  float c=cos(a), s=sin(a);\n"
    "  vec2 q=vec2(p.x*c - p.y*s, p.x*s + p.y*c);\n"
    "  gl_Position=vec4(q, 0.0, 1.0);\n"
    "}\n";

static const char *FS =
    "precision mediump float;\n"
    "uniform float t;\n"
    "void main(){ gl_FragColor=vec4(0.25+0.5*t, 0.95-0.5*t, 0.35, 1.0); }\n";

int main(int argc, char **argv) {
    int w = (argc > 1) ? atoi(argv[1]) : 1280;
    int h = (argc > 2) ? atoi(argv[2]) : 720;
    int frames = (argc > 3) ? atoi(argv[3]) : 300;
    const char *share = (argc > 4) ? argv[4] : NULL;

    if (!share) {
        dsh_write("usage: example [w] [h] [frames] <sharepath>\n");
        return 2;
    }

    dsh_frame f;
    int rc = dsh_frame_open(&f, w, h, share);
    if (rc != 0) {
        dsh_printf("dsh_frame_open failed rc=%d: %s\n", rc, dsh_gl_error());
        return 3;
    }

    /* ---- 建 GL 管线 ---- */
    unsigned (*pCreateProgram)(void) = dlsym(f.gl_lib, "glCreateProgram");
    unsigned (*pCreateShader)(unsigned) = dlsym(f.gl_lib, "glCreateShader");
    void (*pShaderSource)(unsigned, int, const char *const *, const int *) =
        dlsym(f.gl_lib, "glShaderSource");
    void (*pCompileShader)(unsigned) = dlsym(f.gl_lib, "glCompileShader");
    void (*pAttachShader)(unsigned, unsigned) = dlsym(f.gl_lib, "glAttachShader");
    void (*pLinkProgram)(unsigned) = dlsym(f.gl_lib, "glLinkProgram");
    void (*pUseProgram)(unsigned) = dlsym(f.gl_lib, "glUseProgram");
    void (*pGetShaderiv)(unsigned, unsigned, int *) = dlsym(f.gl_lib, "glGetShaderiv");
    void (*pGetProgramiv)(unsigned, unsigned, int *) = dlsym(f.gl_lib, "glGetProgramiv");
    void (*pUniform1f)(int, float) = dlsym(f.gl_lib, "glUniform1f");
    int (*pGetUniformLocation)(unsigned, const char *) =
        dlsym(f.gl_lib, "glGetUniformLocation");
    void (*pGenBuffers)(int, unsigned *) = dlsym(f.gl_lib, "glGenBuffers");
    void (*pBindBuffer)(int, unsigned) = dlsym(f.gl_lib, "glBindBuffer");
    void (*pBufferData)(int, long, const void *, unsigned) =
        dlsym(f.gl_lib, "glBufferData");
    void (*pVertexAttribPointer)(unsigned, int, unsigned, unsigned char, int, const void *) =
        dlsym(f.gl_lib, "glVertexAttribPointer");
    void (*pEnableVertexAttribArray)(unsigned) =
        dlsym(f.gl_lib, "glEnableVertexAttribArray");
    void (*pDrawArrays)(int, int, unsigned) = dlsym(f.gl_lib, "glDrawArrays");
    void (*pClearColor)(float, float, float, float) = dlsym(f.gl_lib, "glClearColor");
    void (*pClear)(unsigned) = dlsym(f.gl_lib, "glClear");
    void (*pViewport)(int, int, int, int) = dlsym(f.gl_lib, "glViewport");

    if (!pCreateProgram || !pCreateShader || !pDrawArrays || !pClear) {
        dsh_write("GL entrypoints missing\n");
        dsh_frame_close(&f);
        return 4;
    }

    int ok = 0;
    unsigned vs = pCreateShader(GL_VERTEX_SHADER);
    { int L = (int)strlen(VS); pShaderSource(vs, 1, &VS, &L); }
    pCompileShader(vs);
    pGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
    if (!ok) { dsh_write("vertex shader compile failed\n"); dsh_frame_close(&f); return 5; }

    unsigned fs = pCreateShader(GL_FRAGMENT_SHADER);
    { int L = (int)strlen(FS); pShaderSource(fs, 1, &FS, &L); }
    pCompileShader(fs);
    pGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) { dsh_write("fragment shader compile failed\n"); dsh_frame_close(&f); return 6; }

    unsigned prog = pCreateProgram();
    pAttachShader(prog, vs);
    pAttachShader(prog, fs);
    pLinkProgram(prog);
    pGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) { dsh_write("program link failed\n"); dsh_frame_close(&f); return 7; }

    /* ★必须先 glGenBuffers 拿到名字：vbo 初值 0 是 GL 的「默认缓冲」，
       对它 glBufferData 是 INVALID_OPERATION 且静默失败，顶点数据根本不会
       上传，表现为「program 链接成功但画面只有清屏色」。 */
    static const float verts[6] = { 0.0f, 0.7f, -0.62f, -0.35f, 0.62f, -0.35f };
    unsigned vbo = 0;
    if (pGenBuffers) pGenBuffers(1, &vbo);
    pBindBuffer(GL_ARRAY_BUFFER, vbo);
    pBufferData(GL_ARRAY_BUFFER, (long)sizeof(verts), verts, GL_STATIC_DRAW);

    pViewport(0, 0, w, h);
    int locA = pGetUniformLocation(prog, "a");
    int locT = pGetUniformLocation(prog, "t");

    for (int i = 0; i < frames; i++) {
        float t = (float)(i % 120) / 120.0f;
        pClearColor(t * 0.5f, (1.0f - t) * 0.5f, 0.3f, 1.0f);
        pClear(GL_COLOR_BUFFER_BIT);
        pUseProgram(prog);
        pBindBuffer(GL_ARRAY_BUFFER, vbo);
        pEnableVertexAttribArray(0);
        pVertexAttribPointer(0, 2, GL_FLOAT, 0, 0, 0);
        pUniform1f(locA, (float)i * 0.04f);
        pUniform1f(locT, t);
        pDrawArrays(GL_TRIANGLES, 0, 3);
        if (dsh_frame_present(&f) != 0) {
            dsh_printf("present failed: %s\n", dsh_gl_error());
            break;
        }
    }

    dsh_printf("done frames=%d seq=%u\n", frames, f.seq);
    dsh_frame_close(&f);
    return 0;
}
