// core.js - the web launcher's import logic, the same rules as
// host/launcher/launcher.cpp: find the folder holding the executable, skip
// what [bundle] exclude names, copy with resume, stamp last. One addition: the
// folder above the executable's is the game's when it, not the executable's,
// holds the required folders (a GOG install keeps its EXE in bin/ beside
// data/), the layout host/sdl/main.cpp's apply_game_root starts such a game
// from. No DOM: the page, its worker and the Node tests all load it.

export function wildcardMatch(pattern, name) {
  const p = pattern.toLowerCase();
  const n = name.toLowerCase();
  const match = (pi, ni) => {
    for (; pi < p.length; pi++, ni++) {
      const c = p[pi];
      if (c === "*") {
        while (p[pi + 1] === "*") pi++;
        if (pi + 1 === p.length) return true;
        for (let k = ni; k <= n.length; k++) if (match(pi + 1, k)) return true;
        return false;
      }
      if (ni >= n.length) return false;
      if (c === "?") continue;
      if (c === "[") {
        let q = pi + 1;
        const negate = p[q] === "!";
        if (negate) q++;
        let hit = false;
        for (; q < p.length && p[q] !== "]"; q++) {
          if (p[q + 1] === "-" && q + 2 < p.length && p[q + 2] !== "]") {
            if (n[ni] >= p[q] && n[ni] <= p[q + 2]) hit = true;
            q += 2;
          } else if (p[q] === n[ni]) hit = true;
        }
        if (q >= p.length || hit === negate) return false;
        pi = q;
        continue;
      }
      if (c !== n[ni]) return false;
    }
    return ni === n.length;
  };
  return match(0, 0);
}

export function excluded(relative, patterns) {
  const parts = relative.split("/");
  const top = parts[0];
  const name = parts[parts.length - 1];
  return patterns.some((p) => wildcardMatch(p, top) || wildcardMatch(p, name));
}

function junk(relative) {
  const name = relative.split("/").pop();
  return relative === "__MACOSX" || relative.startsWith("__MACOSX/") || name === ".DS_Store" ||
    name.startsWith("._") || name.toLowerCase() === "thumbs.db" || name === ".stamp" ||
    name === ".manifest.json";
}

function unsafe(relative) {
  return relative.startsWith("/") || relative.includes(":") ||
    relative.split("/").some((part) => part === "..");
}

function parentOf(path) {
  return path.includes("/") ? path.slice(0, path.lastIndexOf("/")) : "";
}

// The first-level folders under `base` that the import would keep, lowercased.
function topFolders(game, entries, base) {
  const prefix = base ? base + "/" : "";
  const tops = new Set();
  for (const e of entries) {
    if (junk(e.path) || unsafe(e.path) || !e.path.startsWith(prefix)) continue;
    const relative = e.path.slice(prefix.length);
    if (!relative || excluded(relative, game.exclude || [])) continue;
    if (e.isDir || relative.includes("/")) tops.add(relative.split("/")[0].toLowerCase());
  }
  return tops;
}

function missingDirs(game, tops) {
  return (game.requiredDirs || []).filter((d) => !tops.has(d.toLowerCase()));
}

// Which of the source's entries are the game: {base, files, missing, exe}.
// `entries` are {path, size, mtime, isDir}; paths use "/". `exe` is the
// executable's path under `base`. The shallowest executable is the game's;
// between two as shallow, one in a folder the game requires (an install with
// the game in bin/ and its expansion's same-named EXE in bina1/).
export function planImport(game, entries) {
  const exeName = game.executable.toLowerCase();
  const required = new Set((game.requiredDirs || []).map((d) => d.toLowerCase()));
  let exe = null;
  let exeRank = Infinity;
  for (const e of entries) {
    if (e.isDir || junk(e.path) || unsafe(e.path)) continue;
    const parts = e.path.split("/");
    if (parts[parts.length - 1].toLowerCase() !== exeName) continue;
    const depth = parts.length - 1;
    const inRequired = depth > 0 && required.has(parts[depth - 1].toLowerCase());
    const rank = depth * 2 + (inRequired ? 0 : 1);
    if (rank < exeRank && depth <= 3) {
      exe = e;
      exeRank = rank;
    }
  }
  if (!exe) return { error: "noExecutable" };
  let base = parentOf(exe.path);
  let missing = missingDirs(game, topFolders(game, entries, base));
  if (missing.length && base) {
    const up = parentOf(base);
    const above = missingDirs(game, topFolders(game, entries, up));
    if (!above.length) {
      base = up;
      missing = above;
    }
  }
  const prefix = base ? base + "/" : "";
  const files = [];
  let exeRelative = null;
  for (const e of entries) {
    if (e.isDir || junk(e.path) || unsafe(e.path) || !e.path.startsWith(prefix)) continue;
    const relative = e.path.slice(prefix.length);
    if (!relative || excluded(relative, game.exclude || [])) continue;
    if (e === exe) exeRelative = relative;
    files.push({ entry: e, relative, size: e.size, mtime: e.mtime || 0 });
  }
  return { base, files, missing, exe: exeRelative };
}

// ---------------------------------------------------------------------------
// SHA-256 over a stream of chunks (the browser's digest needs one buffer, so
// a small streaming implementation keeps large files out of memory).
// ---------------------------------------------------------------------------
const K = new Uint32Array([
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
]);

export class Sha256 {
  constructor() {
    this.h = new Uint32Array([0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                              0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19]);
    this.block = new Uint8Array(64);
    this.used = 0;
    this.length = 0;
    this.w = new Uint32Array(64);
  }
  compress(b, off) {
    const w = this.w;
    for (let i = 0; i < 16; i++)
      w[i] = (b[off + i * 4] << 24) | (b[off + i * 4 + 1] << 16) | (b[off + i * 4 + 2] << 8) | b[off + i * 4 + 3];
    for (let i = 16; i < 64; i++) {
      const x = w[i - 15], y = w[i - 2];
      const s0 = ((x >>> 7) | (x << 25)) ^ ((x >>> 18) | (x << 14)) ^ (x >>> 3);
      const s1 = ((y >>> 17) | (y << 15)) ^ ((y >>> 19) | (y << 13)) ^ (y >>> 10);
      w[i] = (w[i - 16] + s0 + w[i - 7] + s1) | 0;
    }
    let [a, bb, c, d, e, f, g, h] = this.h;
    for (let i = 0; i < 64; i++) {
      const t1 = (h + (((e >>> 6) | (e << 26)) ^ ((e >>> 11) | (e << 21)) ^ ((e >>> 25) | (e << 7))) +
                  ((e & f) ^ (~e & g)) + K[i] + w[i]) | 0;
      const t2 = ((((a >>> 2) | (a << 30)) ^ ((a >>> 13) | (a << 19)) ^ ((a >>> 22) | (a << 10))) +
                  ((a & bb) ^ (a & c) ^ (bb & c))) | 0;
      h = g; g = f; f = e; e = (d + t1) | 0; d = c; c = bb; bb = a; a = (t1 + t2) | 0;
    }
    const H = this.h;
    H[0] += a; H[1] += bb; H[2] += c; H[3] += d; H[4] += e; H[5] += f; H[6] += g; H[7] += h;
  }
  update(bytes) {
    let i = 0;
    this.length += bytes.length;
    if (this.used) {
      const take = Math.min(64 - this.used, bytes.length);
      this.block.set(bytes.subarray(0, take), this.used);
      this.used += take;
      i = take;
      if (this.used === 64) {
        this.compress(this.block, 0);
        this.used = 0;
      }
    }
    for (; i + 64 <= bytes.length; i += 64) this.compress(bytes, i);
    if (i < bytes.length) {
      this.block.set(bytes.subarray(i), 0);
      this.used = bytes.length - i;
    }
  }
  hex() {
    const bits = this.length * 8;
    const tail = new Uint8Array(((this.used < 56 ? 56 : 120) - this.used) + 8);
    tail[0] = 0x80;
    const view = new DataView(tail.buffer);
    view.setUint32(tail.length - 8, Math.floor(bits / 2 ** 32));
    view.setUint32(tail.length - 4, bits >>> 0);
    this.update(tail);
    return Array.from(this.h, (x) => x.toString(16).padStart(8, "0")).join("");
  }
}

export function sha256Hex(bytes) {
  const s = new Sha256();
  s.update(bytes);
  return s.hex();
}

// ---------------------------------------------------------------------------
// ZIP: read (stored and deflate) and write (stored)
// ---------------------------------------------------------------------------
const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();

export function crc32(bytes, crc = 0) {
  let c = ~crc >>> 0;
  for (let i = 0; i < bytes.length; i++) c = CRC_TABLE[(c ^ bytes[i]) & 0xff] ^ (c >>> 8);
  return ~c >>> 0;
}

function dosTime(d) {
  const date = new Date(d * 1000);
  return {
    time: (date.getHours() << 11) | (date.getMinutes() << 5) | (date.getSeconds() >> 1),
    date: ((date.getFullYear() - 1980) << 9) | ((date.getMonth() + 1) << 5) | date.getDate(),
  };
}

function fromDos(time, date) {
  const d = new Date(1980 + (date >> 9), ((date >> 5) & 15) - 1, date & 31,
                     time >> 11, (time >> 5) & 63, (time & 31) * 2);
  return Math.floor(d.getTime() / 1000);
}

// `blob` is anything with size and slice(start, end).arrayBuffer().
export class ZipReader {
  static async open(blob) {
    const tailSize = Math.min(blob.size, 66000);
    const tail = new Uint8Array(await blob.slice(blob.size - tailSize, blob.size).arrayBuffer());
    let eocd = -1;
    for (let i = tail.length - 22; i >= 0; i--)
      if (tail[i] === 0x50 && tail[i + 1] === 0x4b && tail[i + 2] === 5 && tail[i + 3] === 6) {
        eocd = i;
        break;
      }
    if (eocd < 0) throw new Error("not a ZIP archive");
    const v = new DataView(tail.buffer, eocd);
    let count = v.getUint16(10, true);
    let size = v.getUint32(12, true);
    let offset = v.getUint32(16, true);
    // ZIP64: the real values live in the ZIP64 end record.
    if (offset === 0xffffffff || size === 0xffffffff || count === 0xffff) {
      const loc = eocd - 20;
      if (loc >= 0 && new DataView(tail.buffer, loc).getUint32(0, true) === 0x07064b50) {
        const recOff = Number(new DataView(tail.buffer, loc).getBigUint64(8, true));
        const rec = new DataView(await blob.slice(recOff, recOff + 56).arrayBuffer());
        count = Number(rec.getBigUint64(32, true));
        size = Number(rec.getBigUint64(40, true));
        offset = Number(rec.getBigUint64(48, true));
      }
    }
    const dir = new Uint8Array(await blob.slice(offset, offset + size).arrayBuffer());
    const dv = new DataView(dir.buffer);
    const entries = [];
    const decoder = new TextDecoder();
    let p = 0;
    for (let i = 0; i < count && p + 46 <= dir.length; i++) {
      if (dv.getUint32(p, true) !== 0x02014b50) throw new Error("damaged ZIP directory");
      const method = dv.getUint16(p + 10, true);
      const mtime = fromDos(dv.getUint16(p + 12, true), dv.getUint16(p + 14, true));
      let csize = dv.getUint32(p + 20, true);
      let usize = dv.getUint32(p + 24, true);
      const nameLen = dv.getUint16(p + 28, true);
      const extraLen = dv.getUint16(p + 30, true);
      const commentLen = dv.getUint16(p + 32, true);
      let local = dv.getUint32(p + 42, true);
      const name = decoder.decode(dir.subarray(p + 46, p + 46 + nameLen)).replaceAll("\\", "/");
      // ZIP64 extra field.
      let x = p + 46 + nameLen;
      const xEnd = x + extraLen;
      while (x + 4 <= xEnd) {
        const id = dv.getUint16(x, true), len = dv.getUint16(x + 2, true);
        if (id === 1) {
          let q = x + 4;
          if (usize === 0xffffffff) { usize = Number(dv.getBigUint64(q, true)); q += 8; }
          if (csize === 0xffffffff) { csize = Number(dv.getBigUint64(q, true)); q += 8; }
          if (local === 0xffffffff) { local = Number(dv.getBigUint64(q, true)); }
        }
        x += 4 + len;
      }
      entries.push({ path: name.replace(/\/$/, ""), isDir: name.endsWith("/"), size: usize,
                     csize, method, local, mtime });
      p += 46 + nameLen + extraLen + commentLen;
    }
    const r = new ZipReader();
    r.blob = blob;
    r.list = entries;
    return r;
  }
  entries() {
    return this.list;
  }
  // A ReadableStream of the entry's bytes.
  async stream(entry) {
    const head = new DataView(await this.blob.slice(entry.local, entry.local + 30).arrayBuffer());
    if (head.getUint32(0, true) !== 0x04034b50) throw new Error("damaged ZIP entry " + entry.path);
    const start = entry.local + 30 + head.getUint16(26, true) + head.getUint16(28, true);
    const raw = this.blob.slice(start, start + entry.csize);
    const body = raw.stream ? raw.stream() : new Blob([await raw.arrayBuffer()]).stream();
    if (entry.method === 0) return body;
    if (entry.method === 8) return body.pipeThrough(new DecompressionStream("deflate-raw"));
    throw new Error("unsupported compression in " + entry.path);
  }
}

// A stored (uncompressed) ZIP of {name, data, mtime} files, as a Blob.
export function zipWrite(files) {
  const parts = [];
  const central = [];
  const encoder = new TextEncoder();
  let offset = 0;
  for (const f of files) {
    const name = encoder.encode(f.name);
    const crc = crc32(f.data);
    const { time, date } = dosTime(f.mtime || Math.floor(Date.now() / 1000));
    const local = new DataView(new ArrayBuffer(30));
    local.setUint32(0, 0x04034b50, true);
    local.setUint16(4, 20, true);
    local.setUint16(10, time, true);
    local.setUint16(12, date, true);
    local.setUint32(14, crc, true);
    local.setUint32(18, f.data.length, true);
    local.setUint32(22, f.data.length, true);
    local.setUint16(26, name.length, true);
    parts.push(new Uint8Array(local.buffer), name, f.data);
    const c = new DataView(new ArrayBuffer(46));
    c.setUint32(0, 0x02014b50, true);
    c.setUint16(4, 20, true);
    c.setUint16(6, 20, true);
    c.setUint16(12, time, true);
    c.setUint16(14, date, true);
    c.setUint32(16, crc, true);
    c.setUint32(20, f.data.length, true);
    c.setUint32(24, f.data.length, true);
    c.setUint16(28, name.length, true);
    c.setUint32(42, offset, true);
    central.push(new Uint8Array(c.buffer), name);
    offset += 30 + name.length + f.data.length;
  }
  const centralSize = central.reduce((n, a) => n + a.length, 0);
  const end = new DataView(new ArrayBuffer(22));
  end.setUint32(0, 0x06054b50, true);
  end.setUint16(8, files.length, true);
  end.setUint16(10, files.length, true);
  end.setUint32(12, centralSize, true);
  end.setUint32(16, offset, true);
  return new Blob([...parts, ...central, new Uint8Array(end.buffer)], { type: "application/zip" });
}

// ---------------------------------------------------------------------------
// Import and status over a store:
//   store.readText(rel) -> string|null     store.writeText(rel, text)
//   store.createWriter(rel) -> {write(bytes), close()}  (atomic on close)
//   store.remove(rel)                      store.list() -> [rel]
// and a source:
//   source.list() -> entries      source.stream(entry) -> ReadableStream
// ---------------------------------------------------------------------------
export const MANIFEST = ".manifest.json";
export const STAMP = ".stamp";

export async function importGame(game, source, store, { onProgress, signal, freeBytes } = {}) {
  const entries = await source.list();
  const plan = planImport(game, entries);
  if (plan.error) return { result: "noExecutable" };
  if (plan.missing.length) return { result: "incomplete", missing: plan.missing };
  await store.remove(STAMP);
  const manifest = JSON.parse((await store.readText(MANIFEST)) || '{"files":{}}');
  manifest.exe = plan.exe;
  const progress = { filesDone: 0, filesTotal: plan.files.length, bytesDone: 0, bytesTotal: 0, current: "" };
  let needed = 0;
  for (const f of plan.files) {
    progress.bytesTotal += f.size;
    const have = manifest.files[f.relative];
    f.complete = !!have && have.size === f.size && have.mtime === f.mtime;
    if (!f.complete) needed += f.size;
  }
  const minFree = (game.minFreeMb || 0) * 1024 * 1024;
  if (freeBytes !== undefined && needed + minFree > freeBytes)
    return { result: "noSpace", needed: needed + minFree, free: freeBytes };
  let copied = 0, skipped = 0, lastSave = 0;
  const exeHash = new Sha256();
  for (const f of plan.files) {
    if (signal && signal.aborted) {
      await store.writeText(MANIFEST, JSON.stringify(manifest));
      return { result: "cancelled", copied, skipped };
    }
    progress.current = f.relative;
    const isExe = f.relative === plan.exe;
    if (f.complete && !isExe) {
      skipped++;
      progress.bytesDone += f.size;
      progress.filesDone++;
      onProgress && onProgress({ ...progress });
      continue;
    }
    if (f.complete && isExe) {
      // Already here: hash the stored copy below, without copying it again.
      skipped++;
      progress.bytesDone += f.size;
      progress.filesDone++;
      continue;
    }
    const writer = await store.createWriter(f.relative);
    const reader = (await source.stream(f.entry)).getReader();
    try {
      for (;;) {
        const { done, value } = await reader.read();
        if (done) break;
        if (signal && signal.aborted) {
          await writer.abort();
          await store.writeText(MANIFEST, JSON.stringify(manifest));
          return { result: "cancelled", copied, skipped };
        }
        await writer.write(value);
        if (isExe) exeHash.update(value);
        progress.bytesDone += value.length;
        onProgress && onProgress({ ...progress });
      }
    } catch (e) {
      await writer.abort();
      await store.writeText(MANIFEST, JSON.stringify(manifest));
      const full = e && (e.name === "QuotaExceededError" || /quota/i.test(String(e.message)));
      return { result: full ? "noSpace" : "failed", error: String(e && e.message || e) };
    }
    await writer.close();
    manifest.files[f.relative] = { size: f.size, mtime: f.mtime };
    copied++;
    progress.filesDone++;
    onProgress && onProgress({ ...progress });
    if (copied - lastSave >= 50) {
      lastSave = copied;
      await store.writeText(MANIFEST, JSON.stringify(manifest));
    }
  }
  await store.writeText(MANIFEST, JSON.stringify(manifest));
  let digest;
  const exeEntry = plan.files.find((f) => f.relative === plan.exe);
  if (exeEntry.complete) digest = await store.hash(plan.exe);
  else digest = exeHash.hex();
  if (digest !== game.sha256) return { result: "wrongVersion", digest };
  await store.writeText(STAMP, digest + "\n");
  return { result: "done", copied, skipped };
}

// {state, files, bytes, exe}: `exe` is the executable's path in the store
// (bin/H5_Game.exe when the game's folder is the one above it).
export async function gameStatus(game, store) {
  const stamp = ((await store.readText(STAMP)) || "").trim();
  const manifest = JSON.parse((await store.readText(MANIFEST)) || '{"files":{}}');
  const names = Object.keys(manifest.files);
  const exe = manifest.exe && manifest.files[manifest.exe] ? manifest.exe
    : names.find((n) => n.toLowerCase() === game.executable.toLowerCase());
  if (!exe) return { state: "notFound" };
  const tops = new Set(names.filter((n) => n.includes("/")).map((n) => n.split("/")[0].toLowerCase()));
  const missing = (game.requiredDirs || []).filter((d) => !tops.has(d.toLowerCase()));
  if (stamp !== game.sha256) return { state: stamp ? "wrongVersion" : "incomplete", missing };
  if (missing.length) return { state: "incomplete", missing };
  const bytes = Object.values(manifest.files).reduce((n, f) => n + f.size, 0);
  return { state: "ready", files: names.length, bytes, exe };
}

export function humanBytes(n) {
  if (n >= 2 ** 30) return (n / 2 ** 30).toFixed(1) + " GB";
  if (n >= 2 ** 20) return Math.round(n / 2 ** 20) + " MB";
  return Math.round(n / 1024) + " KB";
}
