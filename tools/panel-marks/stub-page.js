// Runs taxi-cam-marks.js against a stub DOM and rasterises what it draws
// into a texture-sized RGBA file (bilinear when scale is not 1), for
// decode_raw.exe. node stub-page.js <helper.js> <fixtures/x.json> <out.raw> [scale]
const fs = require("fs"), vm = require("vm");
const [, , helperPath, panelJson, outPath, scaleArg] = process.argv;
const data = JSON.parse(fs.readFileSync(panelJson, "utf8"));
const canvases = [];
const timers = [];
const ctx = {
  console, Math, JSON, Object, Array, String, RegExp, Error,
  globalPanelData: data,
  SimVar: { GetSimVarValue: () => 0 },
  setTimeout: (f, ms) => { timers.push([f, ms]); return timers.length; },
  setInterval: () => 1,
  document: {
    body: { appendChild: c => canvases.push(c) },
    getElementById: () => ({}),
    createElement: () => {
      const ops = [];
      const c = { style: {}, ops, parentNode: null, getContext: () => ({ set fillStyle(v) { ops.push(["s", v]); }, fillRect: (x, y, w, h) => ops.push(["r", x, y, w, h]) }) };
      return c;
    },
  },
};
vm.createContext(ctx);
vm.runInContext(fs.readFileSync(helperPath, "utf8"), ctx);
// Run timers until the load-time show has happened (not the hide).
for (let i = 0; i < 10 && timers.length; i++) { const [f, ms] = timers.shift(); if (ms > 60000) break; f(); }
const W = data.vDisplaySize.x, H = data.vDisplaySize.y, scale = Number(scaleArg || 1);
const TW = Math.round(W * scale), TH = Math.round(H * scale);
const page = Buffer.alloc(W * H * 4);
for (let i = 0; i < W * H; i++) { page[i * 4] = page[i * 4 + 1] = page[i * 4 + 2] = 25; page[i * 4 + 3] = 255; }
for (const c of canvases) {
  const left = Number(/left:(\d+)px/.exec(c.style.cssText)[1]), top = Number(/top:(\d+)px/.exec(c.style.cssText)[1]);
  let fill = [0, 0, 0];
  for (const op of c.ops) {
    if (op[0] === "s") { const h = op[1]; fill = [parseInt(h.substr(1, 2), 16), parseInt(h.substr(3, 2), 16), parseInt(h.substr(5, 2), 16)]; continue; }
    const [, x, y, w, h] = op;
    for (let yy = y; yy < y + h; yy++) for (let xx = x; xx < x + w; xx++) {
      const px = left + xx, py = top + yy; if (px >= W || py >= H) continue;
      const o = (py * W + px) * 4; page[o] = fill[0]; page[o + 1] = fill[1]; page[o + 2] = fill[2];
    }
  }
}
// Bilinear into the texture size.
const tex = Buffer.alloc(TW * TH * 4);
for (let y = 0; y < TH; y++) for (let x = 0; x < TW; x++) {
  const sx = (x + 0.5) * W / TW - 0.5, sy = (y + 0.5) * H / TH - 0.5, x0 = Math.floor(sx), y0 = Math.floor(sy), fx = sx - x0, fy = sy - y0;
  const at = (xx, yy, k) => page[((Math.min(H - 1, Math.max(0, yy))) * W + Math.min(W - 1, Math.max(0, xx))) * 4 + k];
  for (let k = 0; k < 4; k++) tex[(y * TW + x) * 4 + k] = Math.round((at(x0, y0, k) * (1 - fx) + at(x0 + 1, y0, k) * fx) * (1 - fy) + (at(x0, y0 + 1, k) * (1 - fx) + at(x0 + 1, y0 + 1, k) * fx) * fy);
}
fs.writeFileSync(outPath, tex);
console.log(`blocks drawn=${canvases.length} page=${W}x${H} texture=${TW}x${TH}`);
