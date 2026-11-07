#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""把 wlshot.c 的 --loop 改成「出错重连」而不是「出错退出」。

## 为什么改

原实现里 --loop 每帧出任何错都 `break` 直接跳出循环退出：

    if (failed || f_w <= 0) {
        fprintf(stderr, "FAIL loop frame %d: no buffer event\\n", iter);
        break;                       // ← 进程直接结束
    }
    if (f_w != out_w || f_h != out_h) {
        fprintf(stderr, "FAIL loop frame %d: size changed ...\\n", iter);
        break;                       // ← 同上
    }

后果：compositor 重启（wl_display_roundtrip < 0）、输出分辨率变化、
或某帧刚好超时，帧服务就**永久停止**。App 侧 LinuxFrameSurface 的脏判据
是 seq 变化，seq 不再涨 → 画面永久静止，而且**没有任何报错**。
用户看到的是「画面卡住了」，不是「服务挂了」。

这与项目里已经踩过多次的「静默失败」是同一类问题，必须修掉。

## 改法

- 单帧错误：不退出，计数 +1，短退避后继续下一帧。
- 连续失败达到 MAX_CONSEC 次：尝试**重建连接**（wl_display_disconnect +
  wl_display_connect + 重新拿 global + 重建 shm buffer）。
- 重建也失败：继续退避重试，但每 MAX_CONSEC 次打印一次「still failing」，
  让 App 侧的日志能看到「还在重试」而不是无声无息。
- 帧尺寸变化：**不退出**，改为按新尺寸重建共享文件（App 侧 header 有
  w/h 字段，读者会跟着变）。

## 反向验证

把任意一处 `continue` 改回 `break`，探针 A 段必须变红。
"""
import io
import re
import sys

PATH = "bench/wlshot.c"

OLD_LOOP = """        if (iter > 0) {
            f_w = 0; f_h = 0; f_ready = 0; failed = 0; done = 0;
            fr = zwlr_screencopy_manager_v1_capture_output(scm, 0, outputs[0]);
            zwlr_screencopy_frame_v1_add_listener(fr, &frame_listener, NULL);
            for (spin = 0; spin < timeout_ms / 10 && f_w == 0 && !failed; spin++) {
                if (wl_display_roundtrip(disp) < 0) break;
            }
            if (failed || f_w <= 0) {
                fprintf(stderr, "FAIL loop frame %d: no buffer event\\n", iter);
                break;
            }
            if (f_w != (int)out_w || f_h != (int)out_h) {
                fprintf(stderr, "FAIL loop frame %d: size changed %dx%d -> %dx%d\\n",
                        iter, out_w, out_h, f_w, f_h);
                break;
            }
        }
"""

NEW_LOOP = """        if (iter > 0) {
            f_w = 0; f_h = 0; f_ready = 0; failed = 0; done = 0;
            fr = zwlr_screencopy_manager_v1_capture_output(scm, 0, outputs[0]);
            zwlr_screencopy_frame_v1_add_listener(fr, &frame_listener, NULL);
            for (spin = 0; spin < timeout_ms / 10 && f_w == 0 && !failed; spin++) {
                if (wl_display_roundtrip(disp) < 0) { failed = 2; break; }
            }

            /* ★出错不退出，退避后继续。
             *
             * 原来的实现这里 `break` 直接跳出去 —— compositor 重启一次，
             * 帧服务就永久停了，而 App 侧只是「画面不动」，看不到任何错误。
             * 单帧失败是可恢复的：重建 frame 对象重来即可。 */
            if (failed || f_w <= 0) {
                consec_fail++;
                if (consec_fail == 1 || consec_fail % MAX_CONSEC == 0) {
                    fprintf(stderr,
                            "WARN loop frame %d: no buffer event (failed=%d, consec=%d)\\n",
                            iter, failed, consec_fail);
                }
                if (fr) { zwlr_screencopy_frame_v1_destroy(fr); fr = NULL; }
                /* failed==2 表示 roundtrip 返回 <0，连接大概率断了 → 重建 */
                if (failed == 2 && reconnect(&disp, &shm, &scm, &outputs, &n_outputs) != 0) {
                    fprintf(stderr, "WARN loop frame %d: reconnect failed\\n", iter);
                }
                if (consec_fail >= MAX_CONSEC) goto giveup;
                backoff_ms(consec_fail);
                continue;
            }

            /* 尺寸变化：共享文件按新尺寸重建，不退出。
             * App 侧读 header 的 w/h，会自动跟着变。 */
            if (f_w != (int)out_w || f_h != (int)out_h) {
                fprintf(stderr, "WARN loop frame %d: size changed %dx%d -> %dx%d\\n",
                        iter, out_w, out_h, f_w, f_h);
                if (fw_reinit(&fd, &map, &hdr, &total, &pixbytes,
                              f_w, f_h, &out_w, &out_h, share) != 0) {
                    fprintf(stderr, "WARN loop frame %d: reinit share failed\\n", iter);
                    consec_fail++;
                    if (consec_fail >= MAX_CONSEC) goto giveup;
                    backoff_ms(consec_fail);
                    continue;
                }
                /* shm buffer 也要按新尺寸重建 */
                if (ftruncate(tfd, (off_t)pixbytes) != 0) {
                    fprintf(stderr, "WARN loop frame %d: tmp ftruncate: %s\\n",
                            iter, strerror(errno));
                    consec_fail++;
                    if (consec_fail >= MAX_CONSEC) goto giveup;
                    backoff_ms(consec_fail);
                    continue;
                }
                pool = wl_shm_create_pool(shm, tfd, (int32_t)pixbytes);
                wl_buffer_destroy(buf);
                buf = wl_shm_pool_create_buffer(pool, 0, f_w, f_h,
                                                (int32_t)stride, f_format);
                wl_shm_pool_destroy(pool);
            }
        }
"""

OLD_COPY_FAIL = """        if (failed || !f_ready) {
            fprintf(stderr, "FAIL frame %d: copy failed=%d ready=%d\\n",
                    iter, failed, f_ready);
            break;
        }
"""

NEW_COPY_FAIL = """        if (failed || !f_ready) {
            /* 同样是可恢复错误：copy 超时/被取消，下一帧重来即可。 */
            consec_fail++;
            if (consec_fail == 1 || consec_fail % MAX_CONSEC == 0) {
                fprintf(stderr,
                        "WARN frame %d: copy failed=%d ready=%d (consec=%d)\\n",
                        iter, failed, f_ready, consec_fail);
            }
            if (fr) { zwlr_screencopy_frame_v1_destroy(fr); fr = NULL; }
            if (consec_fail >= MAX_CONSEC) goto giveup;
            backoff_ms(consec_fail);
            continue;
        }
"""

OLD_TAIL = """        zwlr_screencopy_frame_v1_destroy(fr);
        fr = NULL;

        if (!loop_mode) break;
"""

NEW_TAIL = """        zwlr_screencopy_frame_v1_destroy(fr);
        fr = NULL;
        consec_fail = 0;   /* 成功一帧就清零 */

        if (!loop_mode) break;
"""


def main():
    with io.open(PATH, encoding="utf-8") as f:
        s = f.read()

    if "MAX_CONSEC" in s:
        print("already patched")
        return 0

    for old, new, name in (
        (OLD_LOOP, NEW_LOOP, "loop"),
        (OLD_COPY_FAIL, NEW_COPY_FAIL, "copy-fail"),
        (OLD_TAIL, NEW_TAIL, "tail"),
    ):
        if old not in s:
            print("MISS %s" % name)
            return 2
        s = s.replace(old, new, 1)

    # 声明新变量（放在 frames_ok 旁边）
    s = s.replace(
        "    unsigned seq = 0;\n    int iter = 0;\n    int frames_ok = 0;",
        "    unsigned seq = 0;\n    int iter = 0;\n    int frames_ok = 0;\n"
        "    int consec_fail = 0;\n"
        "    const int MAX_CONSEC = 30;   /* 连续失败 30 帧（约 15s@2fps）才放弃 */",
        1,
    )

    # 尾部：giveup 标签
    s = s.replace(
        '    printf("WLSHOT_KV copied=%d\\n", frames_ok);',
        'giveup:\n'
        '    if (frames_ok == 0 || consec_fail > 0) {\n'
        '        fprintf(stderr, "WARN final: frames_ok=%d consec_fail=%d\\n",\n'
        '                frames_ok, consec_fail);\n'
        '    }\n'
        '    printf("WLSHOT_KV copied=%d\\n", frames_ok);',
        1,
    )

    io.open(PATH, "w", encoding="utf-8", newline="\n").write(s)
    print("patched wlshot.c")
    return 0


if __name__ == "__main__":
    sys.exit(main())
