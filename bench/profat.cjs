// profat.cjs —— 在「引擎就绪」那一刻停止 CPU profile 并写盘
//
// 为什么不用 --cpu-prof：node 只在**正常退出**时才把 profile 落盘，
// 而引擎就绪后是常驻服务、必须 kill -9，于是文件根本不生成
// （本轮实测：`ls: prof_full.cpuprofile: No such file or directory`）。
//
// 为什么不用 v8.startProfiling：那是 node 内部 `--prof` 路径的私有接口，
// 公开的 `node:v8` 并没有这个函数（实测 TypeError: v8.startProfiling is not a function）。
// 正确途径是 inspector 协议：
//   session.post('Profiler.enable')
//   session.post('Profiler.start')
//   ... 就绪时 ...
//   session.post('Profiler.stop') -> { profile }
//
// 关键点：Profiler.stop 的 profile **只有相对时间**（timeDeltas），
// 要算「到就绪点」的构成就直接全量汇总 —— 因为我们在就绪那一刻就停了采样，
// 天然截断，不会像上一轮那样把就绪后的稳态空转算进 (idle)。
'use strict';

const inspector = require('node:inspector');
const fs = require('node:fs');
const path = require('node:path');

const outDir = process.env.PROF_DIR || '/data/data/app.dsh.mobile/files';
const outName = process.env.PROF_NAME || 'prof_at_ready.cpuprofile';
const outPath = path.join(outDir, outName);

const session = new inspector.Session();
session.connect();

const startedAt = Date.now();
session.post('Profiler.enable', () => {
  session.post('Profiler.start', (err) => {
    if (err) fs.writeSync(2, '[profast] start FAIL ' + err.message + '\n');
  });
});

let done = false;
const orig = console.log;
console.log = function (...args) {
  const first = args[0];
  if (!done && typeof first === 'string' && first.includes('dsh web:')) {
    done = true;
    const uptimeMs = Math.round(process.uptime() * 1000);
    const wallMs = Date.now() - startedAt;
    session.post('Profiler.stop', (err, res) => {
      if (err) { fs.writeSync(2, '[profast] stop FAIL ' + err.message + '\n'); return; }
      try {
        fs.writeFileSync(outPath, JSON.stringify(res.profile));
        fs.writeSync(2, `[profast] wrote ${outPath} uptime=${uptimeMs}ms wall=${wallMs}ms\n`);
      } catch (e) {
        fs.writeSync(2, `[profast] write FAIL ${e.message}\n`);
      }
    });
  }
  return orig.apply(console, args);
};
