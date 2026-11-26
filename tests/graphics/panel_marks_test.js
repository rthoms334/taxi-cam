"use strict";
const assert = require("assert");
const fs = require("fs");
const path = require("path");
const vm = require("vm");

const helperPath = path.resolve(__dirname, "../../src/panel_marks/taxi-cam-marks.js");
const marks = require(helperPath);

let checks = 0;
function check(cond, msg) {
  checks++;
  assert.ok(cond, msg);
}

const CELL = 2;
const size = marks.blockSize(10); // { width: 36, height: 24 }
check(size.width === 36 && size.height === 24, "blockSize(10) is 36x24");

// 1. Single gauge places in top-right corner, 1 cell in from edges.
const p0 = marks.placeBlock(0, 0, 1644, 1024, size, []);
check(p0 && p0.left === 1644 - 36 - CELL && p0.top === CELL, "first block at top-right");

// 2. Stacked gauge steps left past the earlier block.
const placed = [p0];
const p1 = marks.placeBlock(0, 0, 1644, 1024, size, placed);
check(p1 && p1.left === p0.left - 36 - CELL && p1.top === CELL, "stacked block steps left");

// 3. Narrow gauge steps down when stepping left cannot fit.
const narrow0 = marks.placeBlock(0, 0, 60, 100, size, []);
check(narrow0 && narrow0.left === 60 - 36 - CELL && narrow0.top === CELL, "narrow first block at top-right");
const narrow1 = marks.placeBlock(0, 0, 60, 100, size, [narrow0]);
check(narrow1 && narrow1.left === narrow0.left && narrow1.top === narrow0.top + size.height + CELL,
      "narrow stacked block steps down");

// 4. Moving left avoids another gauge's top-left corner pixel (finding 3).
// Gauge 0: x=1594, y=10, w=50, h=50 -> block at (1606, 12), corner at (1594, 10).
// Gauge 1: x=0, y=0, w=1644, h=1024 -> top-right (1606, 2) hits block 0;
// stepping left to 1568 would span [1568, 1604) x [2, 26) and cover (1594, 10),
// so placeBlock must step left past the corner to 1594 - 36 - 2 = 1556.
const g0Corner = marks.gaugeCorner(1594, 10);
const g1Corner = marks.gaugeCorner(0, 0);
const corners = [g0Corner, g1Corner];
const smallBox = marks.placeBlock(1594, 10, 50, 50, size, [], corners);
check(smallBox && smallBox.left === 1606 && smallBox.top === 12, "small flush-right gauge places at top-right");
const fullBox = marks.placeBlock(0, 0, 1644, 1024, size, [smallBox], corners);
check(fullBox && fullBox.left === 1594 - size.width - CELL && fullBox.top === CELL,
      "full-panel block clears small gauge top-left corner");
check(fullBox.left + fullBox.width <= 1594 || fullBox.top >= 10 + CELL,
      "full-panel block does not overlap small gauge corner");

// 5. Fallback when no free spot exists: falls back to top-right when clear of corners,
// and returns null if fallback would hit another gauge's top-left corner.
const crowded = [narrow0, narrow1];
const narrowSmallH = marks.placeBlock(0, 0, 60, 48, size, [narrow0]);
check(narrowSmallH && narrowSmallH.left === narrow0.left && narrowSmallH.top === narrow0.top,
      "crowded gauge with no corners falls back to top-right");
const cornerAtTopRight = [marks.gaugeCorner(24, 4)];
const blockedByCorner = marks.placeBlock(0, 0, 60, 48, size, [narrow0], cornerAtTopRight);
check(blockedByCorner === null, "crowded fallback refuses rather than touching a gauge corner");

// 6. End-to-end show() via VM stub with a tiny corner-only gauge that fits no block.
function runShow(panelData) {
  const canvases = [];
  const timers = [];
  const ctx = {
    console, Math, JSON, Object, Array, String, RegExp, Error, Date,
    globalPanelData: panelData,
    SimVar: { GetSimVarValue: () => 0 },
    setTimeout: (f) => { timers.push(f); return timers.length; },
    setInterval: () => 1,
    document: {
      body: { appendChild: (c) => canvases.push(c) },
      getElementById: () => ({ children: [] }),
      createElement: () => ({
        style: {},
        parentNode: null,
        getContext: () => ({ set fillStyle(_) {}, fillRect() {} }),
      }),
    },
  };
  vm.createContext(ctx);
  vm.runInContext(fs.readFileSync(helperPath, "utf8"), ctx);
  for (let i = 0; i < 10 && timers.length; i++) timers.shift()();
  return canvases.map((c) => ({
    left: Number(/left:(\d+)px/.exec(c.style.cssText)[1]),
    top: Number(/top:(\d+)px/.exec(c.style.cssText)[1]),
    width: c.width,
    height: c.height,
  }));
}

const tinyCornerPanel = {
  sName: "VCockpit06",
  sConfigFile: "panel.xml",
  vLogicalSize: { x: 1644, y: 1024 },
  vDisplaySize: { x: 1644, y: 1024 },
  daInstruments: [
    { iGUId: 0, sUrl: "WasmInstrument/WasmInstrument.html?wasm_gauge=Tiny", vPosAndSize: { x: 1620, y: 10, z: 20, w: 20 } },
    { iGUId: 1, sUrl: "WasmInstrument/WasmInstrument.html?wasm_gauge=Full", vPosAndSize: { x: 0, y: 0, z: 1644, w: 1024 } },
  ],
};
const tinyBoxes = runShow(tinyCornerPanel);
check(tinyBoxes.length === 1, "only the full gauge fits a block");
check(tinyBoxes[0].left + tinyBoxes[0].width <= 1620 && tinyBoxes[0].top === CELL,
      "show() steps left past top-left corner of a tiny gauge without a block");

console.log(`panel-marks-js: ${checks} checks passed`);
