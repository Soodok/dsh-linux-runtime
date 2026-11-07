/*
 * vnccap.c —— 极简 VNC 客户端，只做「连上 → 握手 → 请求整屏 → 存成 raw RGB」
 *
 * 为什么自己写：容器里没有 vncsnapshot/vncdotool，装 python 包又要几十 MB。
 * VNC 的 RFB 协议在「无认证 + Raw 编码」这条最简路径上只有几十行，
 * 自己实现可以零依赖地把模拟器画面取出来。
 *
 * 输出格式（给 App/AI 用）：
 *   offset 0  u32 magic 0x4D415246 ("FRAM")
 *   offset 4  u32 seq
 *   offset 8  u32 w
 *   offset 12 u32 h
 *   offset 16 ... RGBA8888
 * 与 dsh_gl 的共享文件格式一致，App 侧不用改。
 *
 * 用法：vnccap <host> <port> <outfile>
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

static int wr(int fd, const void *b, size_t n) {
    const unsigned char *p = (const unsigned char *)b;
    while (n) {
        ssize_t r = write(fd, p, n);
        if (r <= 0) return -1;
        p += r; n -= (size_t)r;
    }
    return 0;
}

static int rd(int fd, void *b, size_t n) {
    unsigned char *p = (unsigned char *)b;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r <= 0) return -1;
        p += r; n -= (size_t)r;
    }
    return 0;
}

static void put32(unsigned char *p, unsigned v) {
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <host> <port> <outfile>\n", argv[0]);
        return 1;
    }
    const char *host = argv[1];
    int port = atoi(argv[2]);
    const char *out = argv[3];

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
        fprintf(stderr, "bad host\n"); return 1;
    }
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        perror("connect"); return 1;
    }

    /* --- 1) 版本握手 --- */
    char ver[13] = {0};
    if (rd(fd, ver, 12) != 0) { fprintf(stderr, "no server version\n"); return 1; }
    fprintf(stderr, "server=%s", ver);
    /* 回 3.8，接受最常见的无认证路径 */
    if (wr(fd, "RFB 003.008\n", 12) != 0) return 1;

    /* --- 2) 安全类型 --- */
    unsigned char ntypes = 0;
    if (rd(fd, &ntypes, 1) != 0) return 1;
    if (ntypes == 0) {
        /* 出错，后面跟长度 + 原因 */
        unsigned char l4[4]; rd(fd, l4, 4);
        unsigned len = (unsigned)l4[0] << 24 | (unsigned)l4[1] << 16 |
                       (unsigned)l4[2] << 8 | l4[3];
        if (len > 200) len = 200;
        char msg[201] = {0};
        rd(fd, msg, len);
        fprintf(stderr, "server refused: %s\n", msg);
        return 1;
    }
    unsigned char types[64];
    if (ntypes > 0 && rd(fd, types, ntypes) != 0) return 1;
    unsigned char chosen = 0;
    for (int i = 0; i < ntypes; i++) {
        /* 1 = None, 2 = VNC Auth */
        if (types[i] == 1) { chosen = 1; break; }
    }
    if (chosen == 0) {
        fprintf(stderr, "no None auth offered (types:");
        for (int i = 0; i < ntypes; i++) fprintf(stderr, " %u", types[i]);
        fprintf(stderr, ")\n");
        return 1;
    }
    if (wr(fd, &chosen, 1) != 0) return 1;

    /* --- 3) ClientInit：shared=1 --- */
    unsigned char shared = 1;
    if (wr(fd, &shared, 1) != 0) return 1;

    /* --- 4) ServerInit --- */
    unsigned char si[24];
    if (rd(fd, si, 24) != 0) return 1;
    unsigned W = (unsigned)si[0] << 8 | si[1];
    unsigned H = (unsigned)si[2] << 8 | si[3];
    char name[256] = {0};
    unsigned nlen = (unsigned)si[20] << 24 | (unsigned)si[21] << 16 |
                    (unsigned)si[22] << 8 | si[23];
    if (nlen > 255) nlen = 255;
    if (nlen) rd(fd, name, nlen);
    fprintf(stderr, "desktop %ux%u \"%s\"\n", W, H, name);

    /* --- 5) SetPixelFormat：请求 32bpp / depth24 / truecolor / RGB888 --- */
    unsigned char spf[20];
    spf[0] = 0;                      /* message-type = SetPixelFormat */
    spf[1] = 0; spf[2] = 0; spf[3] = 0;  /* padding */
    spf[4] = 32;                     /* bits-per-pixel */
    spf[5] = 24;                     /* depth */
    spf[6] = 0;                      /* big-endian-flag = 0 */
    spf[7] = 1;                      /* true-colour-flag = 1 */
    spf[8] = 0; spf[9] = 255;        /* red-max */
    spf[10] = 0; spf[11] = 255;      /* green-max */
    spf[12] = 0; spf[13] = 255;      /* blue-max */
    spf[14] = 16;                    /* red-shift */
    spf[15] = 8;                     /* green-shift */
    spf[16] = 0;                     /* blue-shift */
    spf[17] = 0; spf[18] = 0; spf[19] = 0;
    if (wr(fd, spf, 20) != 0) return 1;

    /* --- 6) SetEncodings：只要 Raw(0) --- */
    unsigned char se[8];
    se[0] = 2; se[1] = 0;            /* message-type=2, padding */
    se[2] = 0; se[3] = 1;            /* number-of-encodings = 1 */
    se[4] = 0; se[5] = 0; se[6] = 0; se[7] = 0;  /* Raw */
    if (wr(fd, se, 8) != 0) return 1;

    /* --- 7) FramebufferUpdateRequest：整屏，增量=0 --- */
    unsigned char fr[10];
    fr[0] = 3; fr[1] = 0;            /* message-type=3, incremental=0 */
    fr[2] = 0; fr[3] = 0; fr[4] = 0; fr[5] = 0;
    fr[6] = (unsigned char)(W >> 8); fr[7] = (unsigned char)(W & 0xff);
    fr[8] = (unsigned char)(H >> 8); fr[9] = (unsigned char)(H & 0xff);
    if (wr(fd, fr, 10) != 0) return 1;

    /* --- 8) 读 FramebufferUpdate --- */
    unsigned char hdr[4];
    if (rd(fd, hdr, 4) != 0) { fprintf(stderr, "no update header\n"); return 1; }
    if (hdr[0] != 0) {
        fprintf(stderr, "unexpected server msg type=%u\n", hdr[0]);
        return 1;
    }
    unsigned nrect = (unsigned)hdr[2] << 8 | hdr[3];
    fprintf(stderr, "rects=%u\n", nrect);

    size_t total = (size_t)W * H * 4 + 16;
    unsigned char *buf = (unsigned char *)calloc(1, total);
    if (!buf) return 1;
    put32(buf + 0, 0x4D415246u);   /* magic "FRAM" */
    put32(buf + 4, 1u);            /* seq */
    put32(buf + 8, W);
    put32(buf + 12, H);

    for (unsigned r = 0; r < nrect; r++) {
        unsigned char rh[12];
        if (rd(fd, rh, 12) != 0) { fprintf(stderr, "rect hdr fail\n"); return 1; }
        unsigned rx = (unsigned)rh[0] << 8 | rh[1];
        unsigned ry = (unsigned)rh[2] << 8 | rh[3];
        unsigned rw = (unsigned)rh[4] << 8 | rh[5];
        unsigned rhh = (unsigned)rh[6] << 8 | rh[7];
        long enc = (long)rh[8] << 24 | (long)rh[9] << 16 |
                   (long)rh[10] << 8 | (long)rh[11];
        if (enc != 0) {
            /* 只要了 Raw，出现别的编码说明 SetEncodings 没生效 */
            fprintf(stderr, "unexpected encoding %ld\n", enc);
            free(buf); return 1;
        }
        for (unsigned y = 0; y < rhh; y++) {
            unsigned char *row = (unsigned char *)malloc((size_t)rw * 4);
            if (!row) { free(buf); return 1; }
            if (rd(fd, row, (size_t)rw * 4) != 0) { free(row); free(buf); return 1; }
            unsigned gy = ry + y;
            if (gy >= H) { free(row); continue; }
            for (unsigned x = 0; x < rw; x++) {
                unsigned gx = rx + x;
                if (gx >= W) break;
                size_t o = 16 + ((size_t)gy * W + gx) * 4;
                /* VNC 给的是 BGRX（我们请求了小端 RGB888），重排成 RGBA 并强制 alpha=255 */
                buf[o + 0] = row[x * 4 + 2];
                buf[o + 1] = row[x * 4 + 1];
                buf[o + 2] = row[x * 4 + 0];
                buf[o + 3] = 255;
            }
            free(row);
        }
    }

    int of = open(out, 1 /*O_WRONLY*/ | 64 /*O_CREAT*/ | 512 /*O_TRUNC*/, 420 /*0644*/);
    if (of < 0) { perror("open out"); free(buf); return 1; }
    if (wr(of, buf, total) != 0) { perror("write"); close(of); free(buf); return 1; }
    close(of);
    fprintf(stderr, "wrote %s (%zu bytes, %ux%u)\n", out, total, W, H);
    printf("RESULT: VNCCAP_OK w=%u h=%u bytes=%zu\n", W, H, total);
    free(buf);
    return 0;
}
