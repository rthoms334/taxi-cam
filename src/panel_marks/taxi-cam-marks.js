// Taxi Cam panel marks. Loaded by the Taxi Cam copy of VCockpit.html (one
// page per cockpit panel). On request it paints one small block per gauge
// naming the panel and the gauge, so Taxi Cam can tell which display texture
// is which. The format is src/shared/panel_marks.hpp; keep the two in step.
// Draws nothing unless asked, never touches a gauge's top-left corner, and
// never stops the page from loading: every step is guarded.
(function () {
    "use strict";
    var COLUMNS = 16, DATA_COLUMNS = 15, BITS = 3, ROW_BITS = DATA_COLUMNS * BITS, VERSION = 1, MAX_PAYLOAD = 160;
    var FINDER = [7, 0, 7, 0, 4, 2, 1, 6, 3, 5, 0, 7, 4, 0, 1, 7];
    var COLOURS = ["#000000", "#0000ff", "#00ff00", "#00ffff", "#ff0000", "#ff00ff", "#ffff00", "#ffffff"];
    // Requests: L:TAXICAM_MARKS changing to a new non-zero value shows the
    // marks for SHOW_MS. -1 turns them off for the page's life.
    var REQUEST_VAR = "L:TAXICAM_MARKS", POLL_MS = 500, SHOW_MS = 4000;
    // Experiment builds also show them once after the panel loads.
    var LOAD_SHOW_MS = 0, LOAD_DELAY_MS = 3000;

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
    function query(url) {
        var at = url.indexOf("?");
        return at < 0 ? "" : url.substring(at + 1).replace(/(^|&)wasm_(module|gauge)=[^&]*/g, "").replace(/^&+/, "");
    }
    // A gauge's text fields other than its URL (panel.cfg's parameters after
    // the rectangle, if the page gets them), for the experiment.
    function gaugeInfo(instrument) {
        var url = instrument.sUrl || "", parts = [query(url)];
        Object.keys(instrument).forEach(function (key) {
            if (key !== "sUrl" && key !== "templateName" && typeof instrument[key] === "string" && instrument[key])
                parts.push(key + "=" + instrument[key]);
        });
        return parts.join("|");
    }
    // What the page knows about its panel, for the experiment: the panel
    // data's keys and a gauge's keys first (the block keeps about 110
    // characters), then every field as key=value, an array as its length and
    // its first element's keys. Also logged for the Coherent debugger.
    function panelInfo(data) {
        var first = data.daInstruments && data.daInstruments[0];
        var parts = ["K=" + Object.keys(data).join(","), "I=" + (first ? Object.keys(first).join(",") : "")];
        Object.keys(data).forEach(function (key) {
            var v = data[key];
            if (Array.isArray(v))
                parts.push(key + "[" + v.length + "]" + (v[0] && typeof v[0] === "object" ? "{" + Object.keys(v[0]).join(",") + "}" : ""));
            else if (v && typeof v === "object")
                parts.push(key + "=" + Object.keys(v).map(function (k) { return v[k]; }).join("x"));
            else
                parts.push(key + "=" + v);
        });
        return parts.join(";");
    }
    function cellSize(width, height) {
        return Math.max(4, Math.min(10, Math.round(Math.min(width, height) / 200)));
    }
    function blockSize(rows, cell) {
        return { width: (COLUMNS + 2) * cell, height: (rows + 2) * cell };
    }
    function drawBlock(mark, left, top) {
        var encoded = encodeCells(encodePayload(mark)), size = blockSize(encoded.rows, mark.cell);
        var canvas = document.createElement("canvas");
        canvas.width = size.width;
        canvas.height = size.height;
        canvas.style.cssText = "position:absolute;pointer-events:none;z-index:2147483647;left:" + left + "px;top:" + top + "px;width:" +
            size.width + "px;height:" + size.height + "px";
        var g = canvas.getContext("2d");
        g.fillStyle = COLOURS[0];
        g.fillRect(0, 0, size.width, size.height);
        for (var i = 0; i < encoded.cells.length; i++) {
            g.fillStyle = COLOURS[encoded.cells[i]];
            g.fillRect((1 + (i % COLUMNS)) * mark.cell, (1 + Math.floor(i / COLUMNS)) * mark.cell, mark.cell, mark.cell);
        }
        document.body.appendChild(canvas);
        return canvas;
    }
    function panelData() {
        return typeof globalPanelData !== "undefined" ? globalPanelData : null;
    }
    // The panel's texture name as the page knows it. Which field holds it is
    // what the first experiment settles; until then the section name.
    function panelName(data) {
        return data.sTextureName || data.sTexture || data.texture || data.sName || "";
    }
    var shown = [], hideTimer = 0;
    function hide() {
        shown.forEach(function (c) {
            if (c.parentNode)
                c.parentNode.removeChild(c);
        });
        shown = [];
        hideTimer = 0;
    }
    function show(ms) {
        var data = panelData();
        if (!data || !data.daInstruments || !data.vDisplaySize || !data.vLogicalSize || !document.body)
            return;
        hide();
        var pw = data.vDisplaySize.x, ph = data.vDisplaySize.y, cell = cellSize(pw, ph), name = panelName(data), placed = [];
        var rx = pw / data.vLogicalSize.x, ry = ph / data.vLogicalSize.y;
        data.daInstruments.forEach(function (instrument, index) {
            var p = instrument.vPosAndSize || {}, url = instrument.sUrl || "";
            // setupInstrument's rounding.
            var x = Math.round(p.x * rx), y = Math.round(p.y * ry), w = Math.round(p.z * rx), h = Math.round(p.w * ry);
            var mark = { kind: 0, cell: cell, gauge: index & 255, x: x, y: y, width: w, height: h, panelWidth: pw, panelHeight: ph,
                         name: name, gaugeName: gaugeName(url), info: gaugeInfo(instrument) };
            var size = blockSize(encodeCells(encodePayload(mark)).rows, cell);
            // Centred in the gauge, with a cell of the gauge left all round:
            // far from the top-left corner other tools sample.
            if (w < size.width + 2 * cell || h < size.height + 2 * cell)
                return;
            var left = Math.round(x + (w - size.width) / 2), top = Math.round(y + (h - size.height) / 2);
            placed.push({ left: left, top: top, right: left + size.width, bottom: top + size.height });
            shown.push(drawBlock(mark, left, top));
        });
        // One panel-level block with what the page knows, bottom right, where
        // it overlaps no gauge block.
        var info = panelInfo(data);
        if (typeof console !== "undefined")
            console.log("Taxi Cam panel marks: " + info);
        var probe = { kind: 1, cell: cell, gauge: 255, x: 0, y: 0, width: pw, height: ph, panelWidth: pw, panelHeight: ph, name: name,
                      gaugeName: data.sName || "", info: info };
        var size = blockSize(encodeCells(encodePayload(probe)).rows, cell);
        var left = pw - size.width - 2 * cell, top = ph - size.height - 2 * cell;
        var clear = left > 2 * cell && top > 2 * cell && placed.every(function (b) {
            return left >= b.right || left + size.width <= b.left || top >= b.bottom || top + size.height <= b.top;
        });
        if (clear)
            shown.push(drawBlock(probe, left, top));
        if (shown.length)
            hideTimer = setTimeout(hide, ms);
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
    // The first read only notes the value: a request made before this page
    // loaded is not for it.
    var lastRequest = null, off = false;
    var poll = guarded(function () {
        if (off || typeof SimVar === "undefined" || !SimVar.GetSimVarValue)
            return;
        var request = SimVar.GetSimVarValue(REQUEST_VAR, "number");
        if (request === -1) {
            off = true;
            hide();
            return;
        }
        if (lastRequest === null) {
            lastRequest = request;
            return;
        }
        if (request && request !== lastRequest) {
            lastRequest = request;
            show(SHOW_MS);
        }
    });
    guarded(function () {
        setInterval(poll, POLL_MS);
        if (LOAD_SHOW_MS > 0) {
            var waitForPanel = guarded(function () {
                if (panelData() && document.getElementById("panel"))
                    setTimeout(guarded(function () { show(LOAD_SHOW_MS); }), LOAD_DELAY_MS);
                else
                    setTimeout(waitForPanel, POLL_MS);
            });
            waitForPanel();
        }
    })();
})();
