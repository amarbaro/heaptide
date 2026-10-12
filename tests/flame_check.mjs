// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AMARBARO · amarbaro.org labs
// The flame page's tree logic (its "ht-core" script, run as is) on `heaptide report DIR --flame-data`:
// every view (normal, inverted, system frames hidden) keeps each metric's total; the root's children
// are the kind nodes; a named frame is present. Prints ok/FAIL lines, exits 1 on a failure.
//   node tests/flame_check.mjs report/flame.html DATA.json [FRAME_NAME]
import { readFileSync } from "node:fs";

const [html, json, want] = process.argv.slice(2);
const core = readFileSync(html, "utf8").match(/<script id="ht-core">([\s\S]*?)<\/script>/)[1];
const module = { exports: {} };
new Function("module", core)(module);
const { buildBase, derive, weigh, KINDS } = module.exports;
const data = JSON.parse(readFileSync(json, "utf8"));
let fails = 0;
const check = (ok, what) => { console.log(`${ok ? "ok  " : "FAIL"} ${what}`); if (!ok) fails++; };

for (const p of data) {
  const b = buildBase(p), all = [true, true, true, true];
  for (const [label, opts] of [["normal", {}], ["inverted", { invert: true }], ["system hidden", { system: true }], ["inverted, system hidden", { invert: true, system: true }]]) {
    const t = derive(b, { invert: false, system: false, kinds: all, ...opts });
    const sums = p.totals.map((_, m) => weigh(t, m).total[0]);
    check(sums.every((s, m) => s === p.totals[m]), `pid ${p.pid} ${label}: totals ${sums.join(",")} = ${p.totals.join(",")}`);
    const kids = [];
    for (let i = 1; i < t.n; i++) if (t.parent[i] === 0) kids.push(t.key[i]);
    check(kids.every(k => k < 0 && k >= -KINDS.length), `pid ${p.pid} ${label}: root's children are kinds (${kids.map(k => KINDS[-1 - k]).join(",")})`);
  }
  if (want) check(p.names.some(n => n.includes(want)), `pid ${p.pid}: a frame named ${want}`);
}
if (!data.length) check(false, "no process in the data");
process.exit(fails ? 1 : 0);
