#!/usr/bin/env node
// web_smoke.mjs - run a game of a web site (tools/web_launcher.py) in Chrome
// driven over the DevTools protocol, with nothing beyond Node 22 and Chrome:
//
//   node tools/web_smoke.mjs --site <dir> --game <id> [--import <install dir>]
//       [--out <dir>] [--seconds N] [--shots 10,20] [--keys 35:Escape,45:Space]
//       [--clicks 55:640:512] [--text 60:Player] [--query RECOMP_X=1&RECOMP_Y=2]
//       [--profile <dir>] [--port 8765] [--width 1280 --height 1111]
//       [--display :97] [--flag <chrome flag>]... [--chrome <path>] [--trace-gpu]
//
// Serves <dir> cross-origin isolated at localhost:<port>. When the game is not
// imported yet, imports <install dir> through the launcher page: the files
// are served to the page from a private route and handed over as a folder pick
// would. Then opens the game's page and records into <out>: console.log (the
// pages' and workers' consoles), screenshots at the --shots seconds, and
// metrics.json (WebGPU adapter, the module's instantiation time, the wasm
// memory, every Chrome process's resident memory per second and its peak,
// animation frames). --keys, --clicks and --text act at their second.
//
// The game's files live in the browser profile's storage for the origin, so
// keep --profile and --port the same between runs to import once.
//
// Chrome runs headless unless --display names an X display, which must be a
// virtual one (Xvfb :97), never the desktop's. Headless Chrome 151 on Linux
// cannot present a WebGPU canvas (SwiftShader's swap chain shared image is
// missing and the GPU process restarts; on NVIDIA its import fails): use
// Xvfb with --flag --enable-features=Vulkan --flag --use-angle=vulkan for the
// GPU, which presents normally there.
import { spawn } from "node:child_process";
import { createServer } from "node:http";
import fs from "node:fs";
import path from "node:path";

const args = process.argv.slice(2);
const opt = { flags: [], shots: [], seconds: 120, out: "web-smoke", chrome: "google-chrome", width: 1280,
              height: 1111, port: 8765 };
for (let i = 0; i < args.length; i++) {
  const a = args[i];
  const next = () => args[++i];
  if (a === "--site") opt.site = next();
  else if (a === "--game") opt.game = next();
  else if (a === "--import") opt.importDir = next();
  else if (a === "--seconds") opt.seconds = Number(next());
  else if (a === "--out") opt.out = next();
  else if (a === "--chrome") opt.chrome = next();
  else if (a === "--profile") opt.profile = next();
  else if (a === "--query") opt.query = next();
  else if (a === "--shots") opt.shots = next().split(",").map(Number);
  else if (a === "--flag") opt.flags.push(next());
  else if (a === "--keys") opt.keys = next();
  else if (a === "--clicks") opt.clicks = next();
  else if (a === "--text") opt.text = next();
  else if (a === "--width") opt.width = Number(next());
  else if (a === "--height") opt.height = Number(next());
  else if (a === "--port") opt.port = Number(next());
  else if (a === "--display") opt.display = next();
  else if (a === "--trace-gpu") opt.traceGpu = true;
  else throw new Error("unknown argument " + a);
}
if (!opt.site || !opt.game) throw new Error("--site and --game are required");
if (opt.display && opt.display === process.env.DISPLAY)
  throw new Error(`--display ${opt.display} is this session's own display; start a virtual one (Xvfb)`);
fs.mkdirSync(opt.out, { recursive: true });
const t0 = Date.now();
const logFile = fs.createWriteStream(path.join(opt.out, "console.log"));
function note(line) {
  const text = `[${((Date.now() - t0) / 1000).toFixed(2)}] ${line}`;
  logFile.write(text + "\n");
  if (!/^\[(page|worker)/.test(line) || /error|abort|stopped|lost|exception/i.test(line)) console.log(text);
}

// The server ---------------------------------------------------------------
const TYPES = { ".html": "text/html", ".js": "text/javascript", ".mjs": "text/javascript", ".wasm": "application/wasm",
                ".json": "application/json", ".data": "application/octet-stream", ".css": "text/css" };
function listInstall(root) {
  const out = [];
  const walk = (dir, rel) => {
    for (const name of fs.readdirSync(dir)) {
      const full = path.join(dir, name);
      const st = fs.statSync(full); // follows links, as a folder pick does
      const r = rel ? rel + "/" + name : name;
      if (st.isDirectory()) walk(full, r);
      else out.push({ path: r, size: st.size, mtime: Math.floor(st.mtimeMs) });
    }
  };
  walk(root, "");
  return out;
}
const server = createServer((req, res) => {
  res.setHeader("Cross-Origin-Opener-Policy", "same-origin");
  res.setHeader("Cross-Origin-Embedder-Policy", "require-corp");
  res.setHeader("Cross-Origin-Resource-Policy", "same-origin");
  res.setHeader("Cache-Control", "no-cache");
  let p = decodeURIComponent(new URL(req.url, "http://x").pathname);
  if (p === "/__import__/list" && opt.importDir) {
    res.setHeader("Content-Type", "application/json");
    res.end(JSON.stringify(listInstall(opt.importDir)));
    return;
  }
  let file, root;
  if (p.startsWith("/__import__/f/") && opt.importDir) {
    root = path.resolve(opt.importDir);
    file = path.join(root, p.slice("/__import__/f/".length));
  } else {
    if (p.endsWith("/")) p += "index.html";
    root = path.resolve(opt.site);
    file = path.join(root, p);
  }
  if (!file.startsWith(root + path.sep)) {
    res.statusCode = 403;
    res.end();
    return;
  }
  fs.stat(file, (err, st) => {
    if (err || !st.isFile()) {
      res.statusCode = 404;
      res.end();
      return;
    }
    res.setHeader("Content-Type", TYPES[path.extname(file)] || "application/octet-stream");
    res.setHeader("Content-Length", st.size);
    if (req.method === "HEAD") res.end();
    else fs.createReadStream(file).pipe(res);
  });
});
await new Promise((r) => server.listen(opt.port, "127.0.0.1", r));
const origin = `http://localhost:${opt.port}`;
note(`serving ${opt.site} at ${origin}`);

// Chrome -------------------------------------------------------------------
const profile = path.resolve(opt.profile || path.join(opt.out, "profile"));
const chromeArgs = [
  ...(opt.display ? [] : ["--headless=new"]), `--user-data-dir=${profile}`, "--remote-debugging-port=0",
  "--no-first-run", "--no-default-browser-check", "--disable-background-timer-throttling",
  "--disable-renderer-backgrounding", "--disable-backgrounding-occluded-windows",
  `--window-size=${opt.width},${opt.height}`, "--enable-unsafe-webgpu", ...opt.flags, "about:blank",
];
note(`chrome ${chromeArgs.join(" ")}`);
const chrome = spawn(opt.chrome, chromeArgs, {
  stdio: ["ignore", "pipe", "pipe"],
  env: { ...process.env, LD_LIBRARY_PATH: "", DISPLAY: opt.display || "", WAYLAND_DISPLAY: "" },
});
const chromeLog = fs.createWriteStream(path.join(opt.out, "chrome-stderr.log"));
chrome.stdout.pipe(chromeLog);
const wsUrl = await new Promise((resolve, reject) => {
  let buf = "";
  chrome.stderr.on("data", (d) => {
    chromeLog.write(d);
    buf += d;
    const m = buf.match(/DevTools listening on (ws:\/\/\S+)/);
    if (m) resolve(m[1]);
  });
  chrome.on("exit", (c) => reject(new Error("chrome exited " + c)));
});
let finished = false;
function shutdown(code) {
  if (finished) return;
  finished = true;
  try { chrome.kill("SIGTERM"); } catch {}
  setTimeout(() => { try { chrome.kill("SIGKILL"); } catch {} }, 3000).unref();
  server.close();
  logFile.end();
  setTimeout(() => process.exit(code), 3500);
}
process.on("SIGINT", () => shutdown(130));
process.on("SIGTERM", () => shutdown(143));

// The DevTools protocol ------------------------------------------------------
const ws = new WebSocket(wsUrl);
await new Promise((r, j) => { ws.onopen = r; ws.onerror = j; });
let nextId = 1;
const calls = new Map();
const listeners = [];
ws.onmessage = (ev) => {
  const msg = JSON.parse(ev.data);
  if (msg.id && calls.has(msg.id)) {
    const { resolve, reject } = calls.get(msg.id);
    calls.delete(msg.id);
    msg.error ? reject(new Error(msg.error.message)) : resolve(msg.result);
  } else if (msg.method) for (const l of listeners) l(msg);
};
function send(method, params = {}, sessionId) {
  const id = nextId++;
  ws.send(JSON.stringify({ id, method, params, sessionId }));
  return new Promise((resolve, reject) => calls.set(id, { resolve, reject }));
}
// The page and, attached automatically, its workers (the game's threads).
const sessions = new Map();
const describe = (a) => a.map((x) => (x.value !== undefined ? String(x.value) : x.description || x.type)).join(" ");
listeners.push((msg) => {
  const kind = sessions.get(msg.sessionId) || "?";
  if (msg.method === "Runtime.consoleAPICalled") note(`[${kind} ${msg.params.type}] ${describe(msg.params.args)}`);
  else if (msg.method === "Runtime.exceptionThrown") {
    const d = msg.params.exceptionDetails;
    note(`[${kind} exception] ${d.exception?.description || d.text}`);
  } else if (msg.method === "Log.entryAdded") note(`[${kind} log ${msg.params.entry.level}] ${msg.params.entry.text}`);
  else if (msg.method === "Target.attachedToTarget") {
    const { sessionId, targetInfo, waitingForDebugger } = msg.params;
    sessions.set(sessionId, targetInfo.type === "page" ? "page" : "worker");
    send("Runtime.enable", {}, sessionId).catch(() => {});
    if (targetInfo.type !== "page")
      send("Target.setAutoAttach", { autoAttach: true, waitForDebuggerOnStart: false, flatten: true }, sessionId)
        .catch(() => {});
    if (waitingForDebugger) send("Runtime.runIfWaitingForDebugger", {}, sessionId).catch(() => {});
  }
});
const { targetId } = await send("Target.createTarget", { url: "about:blank" });
const { sessionId: page } = await send("Target.attachToTarget", { targetId, flatten: true });
sessions.set(page, "page");
for (const domain of ["Runtime", "Log", "Page"]) await send(domain + ".enable", {}, page);
await send("Target.setAutoAttach", { autoAttach: true, waitForDebuggerOnStart: false, flatten: true }, page);

// Before any page script: the wasm memory and the module's instantiation
// time, without touching the Emscripten Module (whose getters abort in an
// assertions build), and the animation frames. --trace-gpu also logs GPU
// allocations of 1 MB and more, canvas resizes, configures and device
// destructions, each with its stack.
const NL = "String.fromCharCode(10)";
const traceGpu = `
  if (window.GPUDevice) {
    let total = 0, count = 0;
    const stack = (n) => new Error().stack.split(${NL}).slice(2, n).join(" | ");
    const big = (what, bytes) => {
      total += bytes; count++;
      if (bytes >= 1 << 20)
        console.warn("gpu-alloc " + what + " " + Math.round(bytes / 1048576) + " MB (total " +
                     Math.round(total / 1048576) + " MB in " + count + ") | " + stack(8));
    };
    const cb = GPUDevice.prototype.createBuffer;
    GPUDevice.prototype.createBuffer = function (d) { big("buffer", d.size); return cb.call(this, d); };
    const ct = GPUDevice.prototype.createTexture;
    GPUDevice.prototype.createTexture = function (d) {
      const s = d.size, w = s.width ?? s[0], h = s.height ?? s[1] ?? 1, l = s.depthOrArrayLayers ?? s[2] ?? 1;
      big("texture " + w + "x" + h + "x" + l + " " + d.format, w * h * l * 4);
      return ct.call(this, d);
    };
    const dd = GPUDevice.prototype.destroy;
    GPUDevice.prototype.destroy = function () { console.warn("gpu-device-destroy | " + stack(14)); return dd.call(this); };
    const cf = GPUCanvasContext.prototype.configure;
    GPUCanvasContext.prototype.configure = function (c) {
      console.warn("canvas-configure " + this.canvas.width + "x" + this.canvas.height + " usage=" + c.usage);
      return cf.call(this, c);
    };
    for (const prop of ["width", "height"]) {
      const d = Object.getOwnPropertyDescriptor(HTMLCanvasElement.prototype, prop);
      Object.defineProperty(HTMLCanvasElement.prototype, prop, { get: d.get, set(v) {
        if (v !== d.get.call(this)) console.warn("canvas-" + prop + " " + d.get.call(this) + " -> " + v + " | " + stack(7));
        d.set.call(this, v);
      } });
    }
  }`;
await send("Page.addScriptToEvaluateOnNewDocument", { source: `(() => {
  const t = (window.recompTiming = {});
  WebAssembly.Memory = new Proxy(WebAssembly.Memory, { construct(T, a) { const m = new T(...a); window.__mem = m; return m; } });
  for (const name of ["instantiateStreaming", "instantiate", "compileStreaming", "compile"]) {
    const f = WebAssembly[name];
    WebAssembly[name] = function (...a) {
      const s = performance.now();
      t[name + "Start"] = Math.round(s);
      return f.apply(this, a).then((r) => { t[name + "Ms"] = Math.round(performance.now() - s); return r; });
    };
  }
  ${opt.traceGpu ? traceGpu : ""}
  window.__frames = 0;
  const tick = () => { window.__frames++; requestAnimationFrame(tick); };
  requestAnimationFrame(tick);
})();` }, page);

async function evaluate(expression) {
  const r = await send("Runtime.evaluate", { expression, awaitPromise: true, returnByValue: true }, page);
  if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description || r.exceptionDetails.text);
  return r.result.value;
}
async function navigate(url) {
  const loaded = new Promise((resolve) => {
    const l = (m) => {
      if (m.method === "Page.loadEventFired" && m.sessionId === page) {
        listeners.splice(listeners.indexOf(l), 1);
        resolve();
      }
    };
    listeners.push(l);
  });
  await send("Page.navigate", { url }, page);
  await loaded;
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
async function shot(name) {
  const { data } = await send("Page.captureScreenshot", { format: "png" }, page);
  const file = path.join(opt.out, name + ".png");
  fs.writeFileSync(file, Buffer.from(data, "base64"));
  note(`screenshot ${file}`);
}
// Resident memory of this Chrome's processes (found by its profile), in MB per process type.
function chromeRss() {
  const out = {};
  for (const pid of fs.readdirSync("/proc").filter((d) => /^\d+$/.test(d))) {
    try {
      const cmd = fs.readFileSync(`/proc/${pid}/cmdline`, "utf8");
      if (!cmd.includes(profile)) continue;
      const type = (cmd.match(/--type=([a-z-]+)/) || [, "browser"])[1];
      const kb = Number((fs.readFileSync(`/proc/${pid}/status`, "utf8").match(/VmRSS:\s+(\d+)/) || [, 0])[1]);
      out[type] = (out[type] || 0) + Math.round(kb / 1024);
    } catch {}
  }
  return out;
}
const VK = { Escape: 27, Enter: 13, Space: 32, Tab: 9 };
async function press(name) {
  const key = name === "Space" ? " " : name;
  const vk = VK[name] || 0;
  await send("Input.dispatchKeyEvent", { type: "rawKeyDown", key, code: name, windowsVirtualKeyCode: vk,
                                         nativeVirtualKeyCode: vk }, page);
  await sleep(150); // held across a few of the game's input polls
  await send("Input.dispatchKeyEvent", { type: "keyUp", key, code: name, windowsVirtualKeyCode: vk,
                                         nativeVirtualKeyCode: vk }, page);
}
// Characters as key presses that carry text, which SDL turns into its text
// input (Input.insertText is IME input, which the canvas does not take).
async function type(text) {
  for (const ch of text) {
    const code = /[a-z]/i.test(ch) ? "Key" + ch.toUpperCase() : /[0-9]/.test(ch) ? "Digit" + ch : "";
    const vk = ch.toUpperCase().charCodeAt(0);
    await send("Input.dispatchKeyEvent", { type: "keyDown", key: ch, code, text: ch, windowsVirtualKeyCode: vk }, page);
    await sleep(60);
    await send("Input.dispatchKeyEvent", { type: "keyUp", key: ch, code, windowsVirtualKeyCode: vk }, page);
    await sleep(60);
  }
}
async function click(x, y) {
  await send("Input.dispatchMouseEvent", { type: "mouseMoved", x, y }, page);
  await sleep(100);
  await send("Input.dispatchMouseEvent", { type: "mousePressed", x, y, button: "left", clickCount: 1 }, page);
  await sleep(80);
  await send("Input.dispatchMouseEvent", { type: "mouseReleased", x, y, button: "left", clickCount: 1 }, page);
}
const at = (spec) => (spec || "").split(",").filter(Boolean).map((s) => s.split(":"));

const metrics = { origin, flags: opt.flags, display: opt.display || "headless" };
try {
  await navigate(`${origin}/`);
  metrics.adapter = await evaluate(`(async () => {
    const a = navigator.gpu && await navigator.gpu.requestAdapter({ powerPreference: "high-performance" });
    return a ? { vendor: a.info.vendor, architecture: a.info.architecture, description: a.info.description } : null;
  })()`);
  note(`WebGPU adapter: ${JSON.stringify(metrics.adapter)}`);
  await evaluate(`new Promise((r) => { const t = () => window.recompLauncher ? r() : setTimeout(t, 100); t(); })`);
  const game = JSON.stringify(opt.game);
  let status = await evaluate(`recompLauncher.status(${game})`);
  note(`launcher status: ${JSON.stringify(status.status)}`);
  if (status.status.state !== "ready") {
    if (!opt.importDir) throw new Error("the game is not imported and no --import was given");
    const ti = Date.now();
    note(`importing ${opt.importDir}`);
    // Real Files, as a folder pick hands over (the launcher's worker gets
    // them by structured clone); Chrome keeps large blobs on disk.
    await evaluate(`(async () => {
      const list = await fetch("/__import__/list").then((r) => r.json());
      const files = [];
      for (const e of list) {
        const url = "/__import__/f/" + e.path.split("/").map(encodeURIComponent).join("/");
        const blob = await fetch(url).then((r) => r.blob());
        files.push({ path: "install/" + e.path, file: new File([blob], e.path.split("/").pop(), { lastModified: e.mtime }) });
      }
      window.__importDone = recompLauncher.importFiles(${game}, files).then(() => "done", (e) => String(e));
    })()`);
    while (!(await evaluate(`Promise.race([window.__importDone, new Promise((r) => setTimeout(() => r(null), 5000))])`)))
      note(`import: ${await evaluate(`document.querySelector(".progress-text")?.textContent || ""`)}`);
    metrics.importSeconds = (Date.now() - ti) / 1000;
    note(`import finished in ${metrics.importSeconds}s: ${await evaluate(`recompLauncher.message(${game})`)}`);
    status = await evaluate(`recompLauncher.status(${game})`);
    note(`launcher status: ${JSON.stringify(status.status)}`);
    if (status.status.state !== "ready") throw new Error("the import did not leave the game ready");
  }
  metrics.game = status.status;

  const tg = Date.now();
  await navigate(`${origin}/${opt.game}/` + (opt.query ? "?" + opt.query : ""));
  note(`game page loaded in ${(Date.now() - tg) / 1000}s`);
  const shots = new Set(opt.shots);
  const keys = at(opt.keys), clicks = at(opt.clicks), texts = at(opt.text);
  const peak = {};
  metrics.samples = [];
  for (let s = 1; s <= opt.seconds; s++) {
    await sleep(1000 - ((Date.now() - tg) % 1000));
    const t = Math.round((Date.now() - tg) / 1000);
    const state = await evaluate(`({ status: document.getElementById("status")?.textContent || "",
      wasmMB: Math.round((window.__mem ? window.__mem.buffer.byteLength : 0) / 1048576),
      frames: window.__frames || 0, timing: window.recompTiming || null })`).catch((e) => ({ error: String(e) }));
    const rss = chromeRss();
    for (const k in rss) peak[k] = Math.max(peak[k] || 0, rss[k]);
    metrics.samples.push({ t, ...state, rss });
    if (s % 5 === 0)
      note(`t=${t}s wasm=${state.wasmMB} MB frames=${state.frames} rss=${JSON.stringify(rss)} timing=${JSON.stringify(state.timing)}`);
    if (shots.has(t)) await shot(`t${String(t).padStart(3, "0")}`);
    for (const [when, name] of keys) if (Number(when) === t) { note(`key ${name}`); await press(name); }
    for (const [when, x, y] of clicks) if (Number(when) === t) { note(`click ${x},${y}`); await click(Number(x), Number(y)); }
    for (const [when, ...words] of texts)
      if (Number(when) === t) { note(`text ${words.join(":")}`); await type(words.join(":")); }
    if (state.status && /stopped|lost|missing|has not been/i.test(state.status)) {
      note(`the page says: ${state.status}`);
      metrics.error = state.status;
      break;
    }
  }
  await shot("final");
  metrics.peakRssMB = peak;
  metrics.resources = await evaluate(`performance.getEntriesByType("resource").filter((e) => e.duration >= 0)
    .map((e) => ({ name: e.name.split("/").pop(), bytes: e.transferSize, start: Math.round(e.startTime), ms: Math.round(e.duration) }))`)
    .catch(() => null);
} catch (e) {
  note(`FAILED: ${e.stack || e}`);
  metrics.error = String(e);
  await shot("error").catch(() => {});
}
fs.writeFileSync(path.join(opt.out, "metrics.json"), JSON.stringify(metrics, null, 2));
note("done");
shutdown(metrics.error ? 1 : 0);
