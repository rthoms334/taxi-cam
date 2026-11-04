// Taxi Cam panel marks. Loaded by the Taxi Cam copy of VCockpit.html (one
// page per cockpit panel). When the panel loads it paints one small,
// near-black block in the top-right corner of each gauge naming the panel and
// the gauge, so Taxi Cam can tell which display texture is which. The blocks
// go as soon as Taxi Cam has read them, and after MAX_SHOW_MS of running
// simulation (HARD_SHOW_MS of any time) at the latest. Taxi Cam talks to the
// pages through L:TAXICAM_MARKS: a new positive value means "read, take them
// down", a new negative value "show them again" (a display texture it could
// not read). 0 is ignored: a flight load may reset the L:var. The format is
// src/shared/panel_marks.hpp;
// keep the two in step. Never touches a gauge's top-left corner, and never
// stops the page from loading: every step is guarded.
(function () {
    "use strict";
    var COLUMNS = 16, DATA_COLUMNS = 15, BITS = 3, ROW_BITS = DATA_COLUMNS * BITS, VERSION = 1, MAX_PAYLOAD = 160;
    var FINDER = [7, 0, 7, 0, 4, 2, 1, 6, 3, 5, 0, 7, 4, 0, 1, 7];
    // A 1 bit is LEVEL in its channel, a 0 bit 0: near-black, one texture
    // pixel per page pixel, so CELL page pixels a cell.
    var LEVEL = 56, CELL = 2;
    var COLOURS = [0, 1, 2, 3, 4, 5, 6, 7].map(function (c) {
        return "rgb(" + (c & 4 ? LEVEL : 0) + "," + (c & 2 ? LEVEL : 0) + "," + (c & 1 ? LEVEL : 0) + ")";
    });
    var READ_VAR = "L:TAXICAM_MARKS", POLL_MS = 250, IDLE_POLL_MS = 1000, START_MS = 500, MAX_SHOW_MS = 20000, HARD_SHOW_MS = 120000;

    function crc16(bytes) {
        var crc = 0xffff;
        for (var i = 0; i < bytes.length; i++) {
            crc ^= bytes[i] << 8;
            for (var b = 0; b < 8; b++)
                crc = (crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1) & 0xffff;
        }
        return crc;
    }
    function rowsFor(payload) {
        return 1 + Math.ceil(((payload + 2) * 8) / ROW_BITS);
    }
    function ascii(s, limit) {
        var out = [];
        s = s == null ? "" : String(s);
        for (var i = 0; i < s.length && out.length < limit; i++) {
            var c = s.charCodeAt(i);
            out.push(c >= 32 && c <= 126 ? c : 63);
        }
        return out;
    }
    // mark: {kind, cell, gauge, x, y, width, height, panelWidth, panelHeight, name, gaugeName, info}
    function encodePayload(m) {
        var out = [VERSION, 0, m.kind, m.cell, m.gauge];
        [m.x, m.y, m.width, m.height, m.panelWidth, m.panelHeight].forEach(function (v) {
            v = Math.max(0, Math.min(65535, Math.round(v)));
            out.push(v >> 8, v & 255);
        });
        function text(s, limit) {
            var bytes = ascii(s, limit);
            out.push(bytes.length);
            Array.prototype.push.apply(out, bytes);
        }
        text(m.name, 47);
        text(m.gaugeName, 47);
        var room = Math.max(0, MAX_PAYLOAD - out.length - 1);
        text(m.info, Math.min(room, 111));
        out[1] = out.length - 2;
        return out;
    }
    function encodeCells(payload) {
        var crc = crc16(payload), bytes = payload.concat([crc >> 8, crc & 255]);
        var rows = rowsFor(payload.length), cells = [], bit = 0;
        for (var c = 0; c < COLUMNS; c++)
            cells.push(FINDER[c]);
        for (var r = 1; r < rows; r++) {
            cells.push(r & 1 ? 0 : 7);
            for (c = 1; c < COLUMNS; c++) {
                var v = 0;
                for (var k = 0; k < BITS; k++, bit++)
                    v = (v << 1) | ((bit >> 3) < bytes.length ? (bytes[bit >> 3] >> (7 - (bit & 7))) & 1 : 0);
                cells.push(v);
            }
        }
        return { rows: rows, cells: cells };
    }
    if (typeof module !== "undefined" && module.exports) {
        module.exports = { encodePayload: encodePayload, encodeCells: encodeCells, crc16: crc16 };
        return;
    }

    // The gauge name: wasm_gauge for WASM gauges, else the page's file name.
    function gaugeName(url) {
        var match = /[?&]wasm_gauge=([^&]*)/.exec(url);
        if (match)
            return match[1];
        var path = url.split("?")[0], slash = path.lastIndexOf("/");
        return path.substring(slash + 1).replace(/\.html?$/i, "");
    }
    // Section names repeat across 2024 attachments (each has its own
    // panel.cfg): a short hash of the panel.cfg path tells them apart.
    function configTag(data) {
        var s = String(data.sConfigFile || "").toLowerCase(), h = 0x811c9dc5;
        for (var i = 0; i < s.length; i++)
            h = Math.imul(h ^ s.charCodeAt(i), 0x01000193) >>> 0;
        return "c=" + ("000" + ((h ^ (h >>> 16)) & 0xffff).toString(16)).slice(-4);
    }
    function blockSize(rows) {
        return { width: (COLUMNS + 2) * CELL, height: (rows + 2) * CELL };
    }
    function drawBlock(encoded, left, top) {
        var size = blockSize(encoded.rows), canvas = document.createElement("canvas");
        canvas.width = size.width;
        canvas.height = size.height;
        canvas.style.cssText = "position:absolute;pointer-events:none;z-index:2147483647;left:" + left + "px;top:" + top + "px;width:" +
            size.width + "px;height:" + size.height + "px";
        var g = canvas.getContext("2d");
        g.fillStyle = COLOURS[0];
        g.fillRect(0, 0, size.width, size.height);
        for (var i = 0; i < encoded.cells.length; i++) {
            g.fillStyle = COLOURS[encoded.cells[i]];
            g.fillRect((1 + (i % COLUMNS)) * CELL, (1 + Math.floor(i / COLUMNS)) * CELL, CELL, CELL);
        }
        document.body.appendChild(canvas);
        return canvas;
    }
    function panelData() {
        return typeof globalPanelData !== "undefined" ? globalPanelData : null;
    }
    function readValue() {
        return typeof SimVar !== "undefined" && SimVar.GetSimVarValue ? SimVar.GetSimVarValue(READ_VAR, "number") : null;
    }
    // The simulation clock: it stands still while the flight loads, so the
    // cap counts only time the cockpit can be seen. null when unreadable.
    function simTime() {
        try {
            return SimVar.GetSimVarValue("E:SIMULATION TIME", "seconds");
        } catch (e) {
            return null;
        }
    }
    var shown = [], showing = false, shownAt = 0, runMs = 0, lastPoll = 0, lastSim = null;
    function hide() {
        showing = false;
        shown.forEach(function (c) {
            if (c.parentNode)
                c.parentNode.removeChild(c);
        });
        shown = [];
    }
    // One block per gauge, in its top-right corner a cell in from the edges:
    // far from the top-left corner other tools sample.
    function show() {
        hide();
        var data = panelData();
        if (!data || !data.daInstruments || !data.vDisplaySize || !data.vLogicalSize || !document.body)
            return;
        var pw = data.vDisplaySize.x, ph = data.vDisplaySize.y, name = data.sName || "", tag = configTag(data);
        var rx = pw / data.vLogicalSize.x, ry = ph / data.vLogicalSize.y;
        data.daInstruments.forEach(function (instrument, index) {
            var p = instrument.vPosAndSize || {};
            // setupInstrument's rounding.
            var x = Math.round(p.x * rx), y = Math.round(p.y * ry), w = Math.round(p.z * rx), h = Math.round(p.w * ry);
            var encoded = encodeCells(encodePayload({ kind: 0, cell: CELL, gauge: index & 255, x: x, y: y, width: w, height: h, panelWidth: pw,
                                                      panelHeight: ph, name: name, gaugeName: gaugeName(instrument.sUrl || ""), info: tag }));
            var size = blockSize(encoded.rows);
            if (w < size.width + 2 * CELL || h < size.height + 2 * CELL)
                return;
            shown.push(drawBlock(encoded, x + w - size.width - CELL, y + CELL));
        });
        if (shown.length) {
            showing = true;
            shownAt = lastPoll = Date.now();
            runMs = 0;
            lastSim = simTime();
        }
    }
    function guarded(f) {
        return function () {
            try {
                f.apply(null, arguments);
            } catch (e) {
                hide();
            }
        };
    }
    // The value when the page loaded: only a later change is for this page
    // (an L:var outlives the flight).
    var baseline = null;
    var poll = guarded(function () {
        var now = Date.now(), value = readValue();
        if (baseline === null)
            baseline = value;
        else if (value && value !== baseline) {
            baseline = value;
            if (value > 0)
                hide();
            else
                show();
        }
        if (showing) {
            var sim = simTime();
            if (sim === null || lastSim === null || sim !== lastSim)
                runMs += now - lastPoll;
            lastSim = sim;
            lastPoll = now;
            if (runMs > MAX_SHOW_MS || now - shownAt > HARD_SHOW_MS)
                hide();
        }
    });
    function loop() {
        poll();
        setTimeout(loop, showing ? POLL_MS : IDLE_POLL_MS);
    }
    var start = guarded(function () {
        if (!panelData() || !document.getElementById("panel")) {
            setTimeout(start, POLL_MS);
            return;
        }
        show();
        loop();
    });
    guarded(function () {
        baseline = readValue();
        setTimeout(start, START_MS);
    })();
})();
