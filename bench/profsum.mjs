// profsum.mjs —— 汇总 .cpuprofile：按「归谁」分类耗时
//
// 与之前那次 profile 汇总的区别：这里**不截断**（--version 场景本来就短，
// 整个 profile 都是有效样本），并且把目录名也纳入分类，这样能区分
// 「node 自己的 internal/*」和「某个 dsh 插件」。
//
// 用法：node profsum.mjs <file.cpuprofile> [readyUs]
import { readFileSync } from 'node:fs';

const file = process.argv[2];
const readyUs = process.argv[3] ? Number(process.argv[3]) : Infinity;

const prof = JSON.parse(readFileSync(file, 'utf8'));
const byId = new Map();
for (const n of prof.nodes) byId.set(n.id, n);

// 用 timeDeltas 累加到目标时间点（微秒）
const samples = prof.samples || [];
const deltas = prof.timeDeltas || [];
const selfByNode = new Map();
let t = 0;
for (let i = 0; i < samples.length; i++) {
  const d = deltas[i] || 0;
  t += d;
  if (t > readyUs) break;
  const id = samples[i];
  selfByNode.set(id, (selfByNode.get(id) || 0) + d);
}

const totalUs = [...selfByNode.values()].reduce((a, b) => a + b, 0);

function bucketOf(node) {
  const cf = node.callFrame || {};
  const url = cf.url || '';
  const fn = cf.functionName || '(anonymous)';
  if (fn === '(idle)' || fn === '(program)' || fn === '(garbage collector)') return fn;
  if (url.startsWith('node:')) return 'node: builtin';
  if (url.includes('/internal/')) return 'node: internal';
  const m = url.match(/node_modules\/((?:@[^/]+\/)?[^/]+)/);
  if (m) return 'pkg: ' + m[1];
  if (url === '') return 'native/unknown';
  return 'other: ' + url.split('/').slice(-2).join('/');
}

const agg = new Map();
for (const [id, us] of selfByNode) {
  const node = byId.get(id);
  if (!node) continue;
  const b = bucketOf(node);
  agg.set(b, (agg.get(b) || 0) + us);
}

const rows = [...agg.entries()].sort((a, b) => b[1] - a[1]);
console.log(`total=${(totalUs / 1000).toFixed(1)}ms samples=${samples.length}`);
console.log('--- top 18 ---');
for (const [k, us] of rows.slice(0, 18)) {
  const ms = us / 1000;
  const pct = totalUs ? (us / totalUs * 100) : 0;
  console.log(`${ms.toFixed(1).padStart(8)} ms  ${pct.toFixed(1).padStart(5)}%  ${k}`);
}
