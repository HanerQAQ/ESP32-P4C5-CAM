#pragma once

// Single-file UI served by the device. Everything is local; the page requires
// no internet access after joining the SoftAP.
static const char WHO_WEB_INDEX_HTML[] = R"WEBUI(<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ESP32-P4C5-CAM 视觉回传</title>
<style>
  :root {
    --bg: #071018;
    --panel: rgba(14, 27, 39, .82);
    --panel-2: rgba(20, 38, 54, .72);
    --line: rgba(130, 196, 220, .16);
    --text: #e7f4f8;
    --muted: #8aa8b5;
    --accent: #4be1a0;
    --accent-2: #42a5f5;
    --warn: #ffcb6b;
    --shadow: 0 22px 70px rgba(0, 0, 0, .38);
    --radius: 18px;
  }
  * { box-sizing: border-box; }
  html, body { margin: 0; min-height: 100%; }
  body {
    color: var(--text);
    background:
      radial-gradient(1000px 600px at 15% -10%, rgba(42, 149, 160, .28), transparent 55%),
      radial-gradient(900px 550px at 100% 0%, rgba(33, 102, 185, .20), transparent 55%),
      linear-gradient(160deg, #050b11, var(--bg) 48%, #0a1721);
    font-family: ui-sans-serif, -apple-system, BlinkMacSystemFont, "Segoe UI",
                 "PingFang SC", "Microsoft YaHei", sans-serif;
    padding: 22px;
  }
  .shell { max-width: 1280px; margin: 0 auto; }
  header {
    display: flex; align-items: center; justify-content: space-between;
    gap: 18px; margin-bottom: 18px;
  }
  .brand { display: flex; align-items: center; gap: 13px; min-width: 0; }
  .mark {
    width: 44px; height: 44px; border-radius: 14px; flex: 0 0 auto;
    background: linear-gradient(145deg, #42e0aa, #2c7ef3);
    box-shadow: 0 10px 28px rgba(66, 150, 220, .25);
    display: grid; place-items: center; color: #04131a; font-weight: 900; font-size: 15px;
  }
  h1 { margin: 0; font-size: clamp(19px, 2.6vw, 27px); letter-spacing: .2px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .sub { color: var(--muted); font-size: 12px; margin-top: 4px; letter-spacing: .5px; }
  .status {
    display: inline-flex; align-items: center; gap: 9px; padding: 9px 13px;
    border: 1px solid var(--line); border-radius: 999px; background: rgba(8, 22, 31, .7);
    color: var(--muted); font-size: 12px; white-space: nowrap;
  }
  .dot { width: 8px; height: 8px; border-radius: 50%; background: var(--warn); box-shadow: 0 0 12px currentColor; }
  .dot.ok { background: var(--accent); color: var(--accent); animation: pulse 1.7s infinite; }
  .dot.bad { background: #ff6b6b; color: #ff6b6b; }
  @keyframes pulse { 50% { opacity: .42; } }
  main { display: grid; grid-template-columns: minmax(0, 1fr) 330px; gap: 18px; align-items: start; }
  .card {
    border: 1px solid var(--line); border-radius: var(--radius); background: var(--panel);
    box-shadow: var(--shadow); backdrop-filter: blur(14px); overflow: hidden;
  }
  .stage-card { padding: 10px; }
  .stage { position: relative; width: 100%; border-radius: 12px; overflow: hidden; background: #000; line-height: 0; }
  #video { display: block; width: 100%; height: auto; aspect-ratio: 4 / 3; object-fit: contain; background: #000; }
  #overlay { position: absolute; inset: 0; width: 100%; height: 100%; pointer-events: none; }
  .scanline {
    position: absolute; inset: 0; pointer-events: none; opacity: .14;
    background: repeating-linear-gradient(to bottom, transparent 0 3px, rgba(255,255,255,.055) 3px 4px);
    mix-blend-mode: screen;
  }
  .toolbar { display: flex; flex-wrap: wrap; gap: 8px; padding: 13px 4px 2px; }
  button, .btn {
    appearance: none; border: 1px solid var(--line); background: var(--panel-2); color: var(--text);
    border-radius: 10px; padding: 9px 12px; font-size: 12px; font-weight: 600; cursor: pointer;
    text-decoration: none; transition: transform .15s ease, border-color .15s ease, background .15s ease;
  }
  button:hover, .btn:hover { transform: translateY(-1px); border-color: rgba(75, 225, 160, .48); background: rgba(35, 62, 78, .85); }
  button.on { border-color: rgba(75, 225, 160, .65); color: var(--accent); }
  aside { display: grid; gap: 14px; }
  .side-card { padding: 16px; }
  .eyebrow { color: var(--muted); font-size: 10px; font-weight: 800; letter-spacing: 1.6px; text-transform: uppercase; margin: 0 0 11px; }
  .metrics { display: grid; grid-template-columns: 1fr 1fr; gap: 9px; }
  .metric { border: 1px solid var(--line); border-radius: 12px; padding: 11px 12px; background: rgba(4, 13, 19, .34); }
  .metric .k { color: var(--muted); font-size: 10px; margin-bottom: 4px; }
  .metric .v { font-size: 17px; font-weight: 800; font-variant-numeric: tabular-nums; }
  .metric .v small { color: var(--muted); font-size: 10px; font-weight: 600; margin-left: 3px; }
  .result-list { display: grid; gap: 7px; max-height: 310px; overflow: auto; }
  .result-row {
    border: 1px solid var(--line); border-left: 3px solid var(--accent); border-radius: 9px;
    padding: 8px 10px; background: rgba(6, 17, 24, .45); font-size: 12px;
  }
  .result-row strong { display: flex; justify-content: space-between; gap: 10px; font-weight: 700; }
  .result-row span { color: var(--muted); font-size: 10px; }
  .empty { color: var(--muted); font-size: 12px; padding: 16px 4px; text-align: center; }
  .message {
    border: 1px dashed rgba(255, 203, 107, .35); background: rgba(255, 203, 107, .06);
    color: #d9c18d; border-radius: 11px; padding: 10px 12px; font-size: 11px; line-height: 1.55; margin-top: 10px;
  }
  footer { color: #66818d; font-size: 11px; text-align: center; margin-top: 18px; }
  code { color: #9bd8c2; }
  @media (max-width: 900px) {
    body { padding: 14px; }
    main { grid-template-columns: 1fr; }
    aside { grid-template-columns: 1fr 1fr; align-items: start; }
    .status { display: none; }
  }
  @media (max-width: 560px) {
    aside { grid-template-columns: 1fr; }
    h1 { font-size: 18px; }
  }
</style>
</head>
<body>
<div class="shell">
  <header>
    <div class="brand">
      <div class="mark">LLM</div>
      <div style="min-width:0">
        <h1 id="page-title">ESP32-P4C5-CAM 视觉回传</h1>
        <div class="sub" id="page-sub">ESP32-P4 + ESP32-C5 · Wi-Fi Web Preview</div>
      </div>
    </div>
    <div class="status"><i class="dot" id="live-dot"></i><span id="live-text">连接中</span></div>
  </header>

  <main>
    <section class="card stage-card">
      <div class="stage">
        <img id="video" alt="camera stream" />
        <canvas id="overlay"></canvas>
        <div class="scanline"></div>
      </div>
      <div class="toolbar">
        <button id="overlay-toggle" class="on" type="button">叠加框：开</button>
        <a id="snapshot" class="btn" href="/api/frame.jpg" download="esp32-p4c5-cam-capture.jpg">保存 JPEG</a>
        <button id="reload-stream" type="button">重连视频</button>
        <a class="btn" href="/api/detections" target="_blank" rel="noreferrer">结果 JSON</a>
      </div>
      <div class="message" id="hint">正在连接设备视频流，请确认电脑或手机已连接开发板热点。</div>
    </section>

    <aside>
      <div class="card side-card">
        <p class="eyebrow">Stream</p>
        <div class="metrics">
          <div class="metric"><div class="k">分辨率</div><div class="v" id="m-resolution">--</div></div>
          <div class="metric"><div class="k">服务端帧率</div><div class="v" id="m-fps">--<small>fps</small></div></div>
          <div class="metric"><div class="k">目标数量</div><div class="v" id="m-count">0</div></div>
          <div class="metric"><div class="k">结果更新</div><div class="v" id="m-age">--<small>ms</small></div></div>
        </div>
      </div>

      <div class="card side-card">
        <p class="eyebrow">Detections</p>
        <div class="result-list" id="result-list"><div class="empty">等待识别结果…</div></div>
      </div>

      <div class="card side-card">
        <p class="eyebrow">Access</p>
        <div class="sub" style="line-height:1.8">
          页面：<code>http://192.168.5.1</code><br>
          视频：<code>:81/stream</code><br>
          结果：<code>/api/detections</code>
        </div>
      </div>
    </aside>
  </main>
  <footer>ESP32-P4C5-CAM · Web 回传 · 数据仅在本机局域网内传输</footer>
</div>

<script>
(() => {
  "use strict";
  const $ = (id) => document.getElementById(id);
  const img = $("video");
  const canvas = $("overlay");
  const ctx = canvas.getContext("2d");
  const palette = ["#4be1a0", "#57b6ff", "#ffcb6b", "#ff8a80", "#c792ea", "#80cbc4", "#f78fb3"];
  const poseLinks = [[5,7],[7,9],[6,8],[8,10],[5,6],[5,11],[6,12],[11,12],[11,13],[13,15],[12,14],[14,16],[0,1],[0,2],[1,3],[2,4]];
  const handLinks = [[0,1],[1,2],[2,3],[3,4],[0,5],[5,6],[6,7],[7,8],[5,9],[9,10],[10,11],[11,12],[9,13],[13,14],[14,15],[15,16],[13,17],[17,18],[18,19],[19,20],[0,17]];
  let state = { objects: [], width: 320, height: 240, type: "", overlay: true, streamStarted: false, streamPort: 81 };
  let lastResultAt = performance.now();
  let retryTimer = 0;

  function setLive(kind, text) {
    $("live-dot").className = "dot " + kind;
    $("live-text").textContent = text;
  }

  function objectLabel(obj, idx) {
    if (obj.text) return String(obj.text).slice(0, 38);
    if (obj.label) return String(obj.label);
    if (obj.id !== undefined) return "ID " + obj.id;
    return "目标 " + (idx + 1);
  }

  function drawLabel(text, x, y, color) {
    if (!text) return;
    const max = 38;
    const tiny = text.length > max ? text.slice(0, max - 1) + "…" : text;
    ctx.font = "600 11px ui-sans-serif, system-ui, sans-serif";
    const w = Math.min(ctx.measureText(tiny).width + 10, state.width - Math.max(0, x));
    const boxX = Math.max(0, Math.min(x, state.width - w));
    const boxY = y - 17 < 0 ? y + 3 : y - 17;
    ctx.fillStyle = "rgba(3, 12, 17, .80)";
    ctx.fillRect(boxX, boxY, w, 15);
    ctx.fillStyle = color;
    ctx.fillText(tiny, boxX + 5, boxY + 11);
  }

  function draw() {
    if (!state.overlay) {
      ctx.clearRect(0, 0, canvas.width, canvas.height);
      return;
    }
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    const objs = state.objects || [];
    objs.forEach((obj, i) => {
      const color = (typeof obj.color === "string" && obj.color.startsWith("#")) ? obj.color : palette[i % palette.length];
      ctx.lineWidth = 2;
      ctx.strokeStyle = color;
      ctx.fillStyle = color;

      if (Array.isArray(obj.box) && obj.box.length >= 4) {
        const [x, y, w, h] = obj.box;
        ctx.strokeRect(x + 1, y + 1, Math.max(1, w - 2), Math.max(1, h - 2));
      }

      if (Array.isArray(obj.line) && obj.line.length >= 4) {
        ctx.lineWidth = 4;
        ctx.beginPath();
        ctx.moveTo(obj.line[0], obj.line[1]);
        ctx.lineTo(obj.line[2], obj.line[3]);
        ctx.stroke();
      }

      if (Array.isArray(obj.points) && obj.points.length >= 2) {
        ctx.beginPath();
        obj.points.forEach((p, n) => n ? ctx.lineTo(p[0], p[1]) : ctx.moveTo(p[0], p[1]));
        if (obj.points.length >= 4) ctx.closePath();
        ctx.stroke();
      }

      const kpts = Array.isArray(obj.keypoints) ? obj.keypoints : [];
      if (kpts.length) {
        const links = state.type === "coco_pose" ? poseLinks : (state.type === "hand_gesture" ? handLinks : []);
        ctx.lineWidth = 2;
        links.forEach(([a, b]) => {
          if (kpts[a] && kpts[b]) {
            ctx.beginPath();
            ctx.moveTo(kpts[a][0], kpts[a][1]);
            ctx.lineTo(kpts[b][0], kpts[b][1]);
            ctx.stroke();
          }
        });
        kpts.forEach((p) => {
          if (!p) return;
          ctx.beginPath();
          ctx.arc(p[0], p[1], state.type === "human_face" ? 2.2 : 2.8, 0, Math.PI * 2);
          ctx.fill();
        });
      }

      let caption = objectLabel(obj, i);
      if (typeof obj.score === "number" && isFinite(obj.score)) caption += "  " + obj.score.toFixed(2);
      if (obj.id !== undefined) caption += "  ID " + obj.id;
      if (obj.pose && typeof obj.pose.z === "number") caption += "  z " + obj.pose.z.toFixed(2) + "m";
      const anchorX = obj.box ? obj.box[0] : (obj.line ? obj.line[0] : (kpts[0] ? kpts[0][0] : 8));
      const anchorY = obj.box ? obj.box[1] : (obj.line ? obj.line[1] : (kpts[0] ? kpts[0][1] : 20));
      drawLabel(caption, anchorX + 2, anchorY - 2, color);
    });
  }

  function renderList(objects) {
    const list = $("result-list");
    list.replaceChildren();
    if (!objects || !objects.length) {
      const empty = document.createElement("div");
      empty.className = "empty";
      empty.textContent = "当前画面没有识别结果";
      list.appendChild(empty);
      return;
    }
    objects.forEach((obj, i) => {
      const row = document.createElement("div");
      row.className = "result-row";
      const rowColor = (typeof obj.color === "string" && obj.color.startsWith("#")) ? obj.color : palette[i % palette.length];
      row.style.borderLeftColor = rowColor;
      const head = document.createElement("strong");
      const label = document.createElement("span");
      label.textContent = objectLabel(obj, i);
      label.style.color = "var(--text)";
      label.style.fontSize = "12px";
      head.appendChild(label);
      const meta = document.createElement("span");
      if (typeof obj.score === "number") meta.textContent = obj.score.toFixed(2);
      else if (obj.id !== undefined) meta.textContent = "ID " + obj.id;
      else meta.textContent = "#" + (i + 1);
      head.appendChild(meta);
      row.appendChild(head);
      if (obj.text) {
        const text = document.createElement("div");
        text.className = "sub";
        text.style.whiteSpace = "pre-wrap";
        text.style.wordBreak = "break-all";
        text.textContent = obj.text;
        row.appendChild(text);
      }
      list.appendChild(row);
    });
  }

  function applyResult(data) {
    const first = !state.streamStarted;
    state.type = data.type || "";
    state.width = data.width || 320;
    state.height = data.height || 240;
    state.objects = Array.isArray(data.objects) ? data.objects : [];
    if (canvas.width !== state.width || canvas.height !== state.height) {
      canvas.width = state.width;
      canvas.height = state.height;
    }
    $("page-title").textContent = data.title || "ESP32-P4C5-CAM 视觉回传";
    $("page-sub").textContent = (data.type || "camera") + " · " +
      state.width + "×" + state.height + " · ESP32-P4 + ESP32-C5";
    $("m-resolution").textContent = state.width + "×" + state.height;
    const fps = Number(data.streamFps);
    $("m-fps").textContent = "";
    $("m-fps").append(document.createTextNode(Number.isFinite(fps) ? fps.toFixed(1) : "--"));
    const small = document.createElement("small"); small.textContent = "fps"; $("m-fps").appendChild(small);
    $("m-count").textContent = state.objects.length;
    const age = Math.max(0, Number(data.resultAgeMs) || 0);
    $("m-age").textContent = "";
    $("m-age").append(document.createTextNode(String(Math.round(age))));
    const ageSmall = document.createElement("small"); ageSmall.textContent = "ms"; $("m-age").appendChild(ageSmall);
    renderList(state.objects);
    draw();
    lastResultAt = performance.now();

    const streamPort = Number(data.streamPort) || 81;
    state.streamPort = streamPort;
    if (!state.streamStarted) {
      startStream(streamPort);
    }
  }

  function startStream(port) {
    state.streamStarted = true;
    state.streamPort = port || 81;
    clearTimeout(retryTimer);
    const protocol = location.protocol === "https:" ? "https:" : "http:";
    const url = protocol + "//" + location.hostname + ":" + state.streamPort + "/stream?t=" + Date.now();
    img.onerror = () => {
      setLive("bad", "视频断线，重连中");
      $("hint").textContent = "视频流连接中断，正在自动重连。请只保留一个预览页面，并确认已连接设备热点。";
      clearTimeout(retryTimer);
      retryTimer = setTimeout(() => startStream(state.streamPort), 1600);
    };
    img.onload = () => {
      setLive("ok", "视频已连接");
      $("hint").textContent = "视频来自设备实时 JPEG 流；彩色框由浏览器根据识别结果叠加，不会写回设备。";
    };
    img.src = url;
  }

  async function poll() {
    try {
      const res = await fetch("/api/detections?t=" + Date.now(), { cache: "no-store" });
      if (!res.ok) throw new Error("HTTP " + res.status);
      applyResult(await res.json());
      setLive("ok", "视频已连接");
    } catch (err) {
      if (performance.now() - lastResultAt > 2200) {
        setLive("bad", "等待设备");
        $("hint").textContent = "无法读取设备状态。请确认页面地址为 http://192.168.5.1，且设备与终端仍在同一热点。";
      }
    } finally {
      setTimeout(poll, 350);
    }
  }

  $("overlay-toggle").addEventListener("click", () => {
    state.overlay = !state.overlay;
    $("overlay-toggle").classList.toggle("on", state.overlay);
    $("overlay-toggle").textContent = "叠加框：" + (state.overlay ? "开" : "关");
    draw();
  });
  $("reload-stream").addEventListener("click", () => startStream(state.streamPort));
  img.addEventListener("load", () => {
    setLive("ok", "视频已连接");
  });
  poll();
})();
</script>
</body>
</html>)WEBUI";
