// node --test web/launcher/tests - the web launcher's import rules.
import { test } from "node:test";
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import {
  wildcardMatch, excluded, planImport, sha256Hex, Sha256, crc32, ZipReader, zipWrite,
  importGame, gameStatus, humanBytes, MANIFEST, STAMP,
} from "../core.js";

const enc = new TextEncoder();
const EXE = enc.encode("MZ fake game executable\n");
const game = {
  id: "test", title: "Test Game", executable: "GAME.EXE",
  sha256: createHash("sha256").update(EXE).digest("hex"),
  requiredDirs: ["data", "levels"], exclude: ["__redist", "*.dll"], minFreeMb: 0,
};

class MemoryStore {
  constructor() { this.files = new Map(); }
  async readText(rel) { return this.files.has(rel) ? new TextDecoder().decode(this.files.get(rel)) : null; }
  async writeText(rel, text) { this.files.set(rel, enc.encode(text)); }
  async remove(rel) { this.files.delete(rel); }
  async hash(rel) { return sha256Hex(this.files.get(rel)); }
  async createWriter(rel) {
    const chunks = [];
    const files = this.files;
    return {
      async write(b) { chunks.push(new Uint8Array(b)); },
      async close() { files.set(rel, Buffer.concat(chunks)); },
      async abort() {},
    };
  }
}

// A source over {path: bytes}.
function memorySource(tree) {
  const entries = [];
  const dirs = new Set();
  for (const [path, data] of Object.entries(tree)) {
    entries.push({ path, size: data.length, mtime: 1000, isDir: false, data });
    const parts = path.split("/");
    for (let i = 1; i < parts.length; i++) dirs.add(parts.slice(0, i).join("/"));
  }
  for (const d of dirs) entries.push({ path: d, size: 0, mtime: 0, isDir: true });
  return {
    reads: 0,
    async list() { return entries; },
    async stream(e) { this.reads++; return new Blob([e.data]).stream(); },
  };
}

const install = (prefix = "") => ({
  [prefix + "game.exe"]: EXE,
  [prefix + "DATA/sprites.bin"]: enc.encode("s".repeat(10000)),
  [prefix + "levels/one.lvl"]: enc.encode("level one"),
  [prefix + "__redist/vc.exe"]: enc.encode("skip"),
  [prefix + "bink.dll"]: enc.encode("skip"),
  [prefix + "readme.txt"]: enc.encode("keep"),
});

test("wildcards and exclusion", () => {
  assert.ok(wildcardMatch("*.dll", "BINKW32.DLL"));
  assert.ok(wildcardMatch("Setup?.txt", "setup1.TXT"));
  assert.ok(!wildcardMatch("Setup?.txt", "Setup10.txt"));
  assert.ok(wildcardMatch("[a-c]x", "Bx"));
  assert.ok(!wildcardMatch("[!a-c]x", "bx"));
  assert.ok(wildcardMatch("*", ""));
  assert.ok(excluded("__redist/x/y.exe", ["__redist"]));
  assert.ok(!excluded("data/__redist/x.exe", ["__redist"]));
});

test("sha256 and crc32 match the platform", () => {
  for (const size of [0, 3, 55, 56, 63, 64, 65, 1000, 200000]) {
    const bytes = new Uint8Array(size).map((_, i) => (i * 7) & 255);
    assert.equal(sha256Hex(bytes), createHash("sha256").update(bytes).digest("hex"));
    const s = new Sha256();
    for (let i = 0; i < bytes.length; i += 17) s.update(bytes.subarray(i, i + 17));
    assert.equal(s.hex(), createHash("sha256").update(bytes).digest("hex"));
  }
  assert.equal(crc32(enc.encode("123456789")), 0xcbf43926);
});

test("plan: shallowest executable, exclusions, required folders", () => {
  const src = memorySource({ ...install("GOG/Test Game/"), "GOG/Test Game/backup/game.exe": EXE });
  return src.list().then((entries) => {
    const plan = planImport(game, entries);
    assert.equal(plan.base, "GOG/Test Game");
    assert.deepEqual(plan.files.map((f) => f.relative).sort(),
                     ["DATA/sprites.bin", "backup/game.exe", "game.exe", "levels/one.lvl", "readme.txt"]);
    assert.deepEqual(plan.missing, []);
    assert.equal(plan.exe, "game.exe");
    const bare = planImport(game, entries.filter((e) => !e.path.includes("levels")));
    assert.deepEqual(bare.missing, ["levels"]);
    assert.equal(planImport(game, [{ path: "a.txt", size: 1 }]).error, "noExecutable");
    assert.equal(planImport(game, [{ path: "../game.exe", size: 1 }]).error, "noExecutable");
  });
});

test("plan: the folder above a bin/ executable when it holds the required folders", async () => {
  const gog = {
    "Game/bin/game.exe": EXE, "Game/bin/helper.dll": enc.encode("skip"), "Game/bin/run.cfg": enc.encode("cfg"),
    "Game/DATA/sprites.bin": enc.encode("s"), "Game/levels/one.lvl": enc.encode("level one"),
  };
  const plan = planImport(game, await memorySource(gog).list());
  assert.equal(plan.base, "Game");
  assert.equal(plan.exe, "bin/game.exe");
  assert.deepEqual(plan.missing, []);
  assert.deepEqual(plan.files.map((f) => f.relative).sort(),
                   ["DATA/sprites.bin", "bin/game.exe", "bin/run.cfg", "levels/one.lvl"]);
  // Between two as shallow, the one in a required folder (an expansion's EXE beside it).
  const both = planImport({ ...game, requiredDirs: ["bin", "data"] },
                          await memorySource({ "G/addon/game.exe": EXE, "G/bin/game.exe": EXE, "G/data/x": EXE }).list());
  assert.equal(both.exe, "bin/game.exe");
  assert.equal(both.base, "G");
  // The executable's own folder wins when it holds them itself.
  const own = await memorySource({ ...gog, "Game/bin/data/x": EXE, "Game/bin/levels/y": EXE }).list();
  assert.equal(planImport(game, own).base, "Game/bin");
  // Neither folder holds them: the executable's is reported incomplete.
  const bare = planImport(game, await memorySource({ "Game/bin/game.exe": EXE, "Game/data/x": EXE }).list());
  assert.equal(bare.base, "Game/bin");
  assert.deepEqual(bare.missing, ["data", "levels"]);

  const store = new MemoryStore();
  assert.equal((await importGame(game, memorySource(gog), store)).result, "done");
  assert.ok(store.files.has("bin/game.exe"));
  const status = await gameStatus(game, store);
  assert.equal(status.state, "ready");
  assert.equal(status.exe, "bin/game.exe");
});

test("import, status, resume and wrong versions", async () => {
  const store = new MemoryStore();
  assert.equal((await gameStatus(game, store)).state, "notFound");
  const src = memorySource(install("Test Game/"));
  const seen = [];
  const out = await importGame(game, src, store, { onProgress: (p) => seen.push(p) });
  assert.equal(out.result, "done");
  assert.equal(out.copied, 4);
  assert.ok(store.files.has("DATA/sprites.bin"));
  assert.ok(!store.files.has("bink.dll"));
  assert.equal(new TextDecoder().decode(store.files.get(STAMP)), game.sha256 + "\n");
  const last = seen[seen.length - 1];
  assert.equal(last.filesDone, 4);
  assert.equal(last.bytesDone, last.bytesTotal);
  const status = await gameStatus(game, store);
  assert.equal(status.state, "ready");
  assert.equal(status.files, 4);

  // Again: nothing is read again.
  const again = memorySource(install("Test Game/"));
  const second = await importGame(game, again, store);
  assert.equal(second.result, "done");
  assert.equal(second.copied, 0);
  assert.equal(again.reads, 0);

  // Cancelled part way: no stamp, and the next import continues.
  const fresh = new MemoryStore();
  const controller = new AbortController();
  const cancelled = await importGame(game, memorySource(install()), fresh, {
    signal: controller.signal,
    onProgress: (p) => { if (p.filesDone === 2) controller.abort(); },
  });
  assert.equal(cancelled.result, "cancelled");
  assert.ok(!fresh.files.has(STAMP));
  assert.equal((await gameStatus(game, fresh)).state !== "ready", true);
  const resumed = await importGame(game, memorySource(install()), fresh);
  assert.equal(resumed.result, "done");
  assert.equal(resumed.copied + resumed.skipped, 4);
  assert.ok(resumed.skipped >= 1);

  // A different executable is copied but never stamped.
  const wrong = new MemoryStore();
  const bad = await importGame(game, memorySource({ ...install(), "game.exe": enc.encode("other") }), wrong);
  assert.equal(bad.result, "wrongVersion");
  assert.ok(!wrong.files.has(STAMP));

  // Missing folders: nothing is written.
  const partial = new MemoryStore();
  const tree = install();
  delete tree["levels/one.lvl"];
  const inc = await importGame(game, memorySource(tree), partial);
  assert.equal(inc.result, "incomplete");
  assert.deepEqual(inc.missing, ["levels"]);
  assert.equal(partial.files.size, 0);

  // Not enough room.
  const tight = await importGame({ ...game, minFreeMb: 1 }, memorySource(install()), new MemoryStore(), { freeBytes: 1000 });
  assert.equal(tight.result, "noSpace");
  assert.ok(tight.needed > tight.free);
});

test("zip read (stored and deflate) and write", async () => {
  const { deflateRawSync } = await import("node:zlib");
  // A deflated archive built by hand, beside zipWrite's stored ones.
  const files = [
    { name: "Test Game/GAME.EXE", data: EXE, mtime: 1700000000 },
    { name: "Test Game/data/a.bin", data: enc.encode("a".repeat(5000)), mtime: 1700000000 },
    { name: "Test Game/levels/l.lvl", data: enc.encode("l"), mtime: 1700000000 },
  ];
  const stored = zipWrite(files);
  const reader = await ZipReader.open(stored);
  assert.equal(reader.entries().length, 3);
  const e = reader.entries().find((x) => x.path.endsWith("a.bin"));
  const bytes = new Uint8Array(await new Response(await reader.stream(e)).arrayBuffer());
  assert.equal(bytes.length, 5000);
  assert.equal(e.mtime, 1700000000);

  // Deflate: rewrite one entry's data compressed.
  const raw = deflateRawSync(Buffer.from("b".repeat(4000)));
  const name = enc.encode("x.bin");
  const local = Buffer.alloc(30);
  local.writeUInt32LE(0x04034b50, 0); local.writeUInt16LE(8, 8);
  local.writeUInt32LE(crc32(enc.encode("b".repeat(4000))), 14);
  local.writeUInt32LE(raw.length, 18); local.writeUInt32LE(4000, 22); local.writeUInt16LE(name.length, 26);
  const central = Buffer.alloc(46);
  central.writeUInt32LE(0x02014b50, 0); central.writeUInt16LE(8, 10);
  central.writeUInt32LE(raw.length, 20); central.writeUInt32LE(4000, 24); central.writeUInt16LE(name.length, 28);
  const end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50, 0); end.writeUInt16LE(1, 8); end.writeUInt16LE(1, 10);
  end.writeUInt32LE(46 + name.length, 12); end.writeUInt32LE(30 + name.length + raw.length, 16);
  const deflated = new Blob([local, name, raw, central, name, end]);
  const dz = await ZipReader.open(deflated);
  const out = new Uint8Array(await new Response(await dz.stream(dz.entries()[0])).arrayBuffer());
  assert.equal(new TextDecoder().decode(out), "b".repeat(4000));

  // A ZIP imports like a folder.
  const store = new MemoryStore();
  const source = { list: async () => reader.entries(), stream: (x) => reader.stream(x) };
  const result = await importGame(game, source, store);
  assert.equal(result.result, "done");
  assert.equal((await gameStatus(game, store)).state, "ready");

  await assert.rejects(ZipReader.open(new Blob([enc.encode("not a zip at all")])));
});

test("human sizes", () => {
  assert.equal(humanBytes(3 * 2 ** 30), "3.0 GB");
  assert.equal(humanBytes(5 * 2 ** 20), "5 MB");
  assert.equal(humanBytes(2048), "2 KB");
});
