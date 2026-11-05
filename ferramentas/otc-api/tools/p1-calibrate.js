"use strict";

const net = require("net");

function hasFlag(name) {
  return process.argv.indexOf("--" + name) >= 0;
}

function num(name, aliases) {
  const keys = [name].concat(aliases || []);
  for (const k of keys) {
    const i = process.argv.indexOf("--" + k);
    if (i >= 0 && i + 1 < process.argv.length) return Number(process.argv[i + 1]);
  }
  return undefined;
}

const refine = hasFlag("refine");
const wantsInventory = hasFlag("inventory") || hasFlag("slots");
const hints = { action: "calibration", cmd: "hints" };
const health = num("health");
const level = num("level");

if (!refine && (health == null || level == null)) {
  console.error(
    "usage:\n" +
      "  node tools/p1-calibrate.js --health 225 --level 1 [--mana 125 --exp 43 --cap 377]\n" +
      "  node tools/p1-calibrate.js --refine --z 7 [--minX 700 --maxX 900 --minY 1750 --maxY 2000]\n" +
      "    [--battle] [--inventory] [--id 2854] [--slot 3]\n"
  );
  process.exit(1);
}

if (health != null) {
  hints.health = health;
  hints.maxHealth = num("maxHealth") ?? health;
}
if (level != null) hints.level = level;
const mana = num("mana");
if (mana != null) {
  hints.mana = mana;
  hints.maxMana = num("maxMana") ?? mana;
}
const exp = num("exp", ["experience"]);
if (exp != null) hints.experience = exp;
const cap = num("cap", ["capacity"]);
if (cap != null) hints.capacity = cap;
const x = num("x");
const y = num("y");
const z = num("z");
const minX = num("minX", ["min-x"]);
const maxX = num("maxX", ["max-x"]);
const minY = num("minY", ["min-y"]);
const maxY = num("maxY", ["max-y"]);
const bounds = [minX, maxX, minY, maxY];
const hasAnyBound = bounds.some((v) => v != null);
const hasAllBounds = bounds.every((v) => v != null);
if (hasAnyBound && !hasAllBounds) {
  console.error("position range requires --minX, --maxX, --minY and --maxY together");
  process.exit(1);
}
if (hasAllBounds && !refine) {
  console.error("position range is only valid with --refine");
  process.exit(1);
}
if (hasAllBounds && z == null) {
  console.error("position range requires --z <0-15>");
  process.exit(1);
}
if (hasAllBounds && (x != null || y != null)) {
  console.error("use either exact --x/--y/--z or a position range, not both");
  process.exit(1);
}
if (
  hasAllBounds &&
  (!bounds.every((v) => Number.isInteger(v) && v >= 0 && v <= 65535) ||
    minX > maxX ||
    minY > maxY)
) {
  console.error("position range must contain uint16 bounds (min <= max, max <= 65535)");
  process.exit(1);
}
if (z != null && (!Number.isInteger(z) || z < 0 || z > 15)) {
  console.error("--z must be an integer between 0 and 15");
  process.exit(1);
}
if ((hasFlag("moved") || hasFlag("moving")) && z == null) {
  console.error("--moved/--moving requires --z <0-15>");
  process.exit(1);
}
if (z != null) hints.z = z;
if (hasFlag("moved")) hints.moved = true;
if (hasFlag("moving")) hints.moving = true;
if (x != null && y != null && z != null) {
  hints.x = x;
  hints.y = y;
}
if (hasAllBounds) {
  hints.minX = minX;
  hints.maxX = maxX;
  hints.minY = minY;
  hints.maxY = maxY;
}
if (hasFlag("battle") || hasFlag("swords")) hints.battle = true;
if (wantsInventory) hints.inventory = true;
const itemId = num("id", ["itemId"]);
if (itemId != null) hints.itemId = itemId;
const slot = num("slot");
if (slot != null) hints.slot = slot;

const PIPE = "\\\\.\\pipe\\OTCApi";
let step = refine ? "refine" : "scan";
let lastInventory = null;

const sock = net.connect(PIPE, () => {
  console.log("connected");
  sock.write(JSON.stringify(hints) + "\n");
  sock.write(
    JSON.stringify({ action: "calibration", cmd: refine ? "refine" : "scan" }) + "\n"
  );
});

sock.setEncoding("utf8");
let buf = "";
sock.on("data", (chunk) => {
  buf += chunk;
  for (;;) {
    const n = buf.indexOf("\n");
    if (n < 0) break;
    const line = buf.slice(0, n);
    buf = buf.slice(n + 1);
    if (!line) continue;
    console.log(line);
    let msg;
    try {
      msg = JSON.parse(line);
    } catch {
      continue;
    }
    if (msg.event === "inventory" && Array.isArray(msg.data)) lastInventory = msg.data;
    if (msg.event !== "calibration") continue;
    if ((step === "scan" || step === "refine") && msg.data && msg.data.ok) {
      if (!refine && msg.data.chainSteps === 0) {
        console.error(
          "scan locked a stats copy (chainSteps=0, not LocalPlayer). Not saving. Stay on the map and retry scan."
        );
        process.exit(1);
      }
      if (refine && itemId != null) {
        if (msg.data.hasInventory === false) {
          console.error(
            "inventory not found — keep the bag equipped and retry --refine --inventory --id " +
              itemId +
              (slot != null ? " --slot " + slot : "")
          );
          process.exit(1);
        }
        const wantSlot = slot != null && slot >= 1 && slot <= 10 ? slot : null;
        const cells = lastInventory || [];
        const hit = wantSlot
          ? cells.find((c) => c && c.slot === wantSlot && c.id === itemId)
          : cells.find((c) => c && c.id === itemId);
        if (!hit) {
          console.error(
            wantSlot
              ? "inventory refine missed slot " +
                  wantSlot +
                  " id " +
                  itemId +
                  " — not saving. Keep the bag equipped and retry --refine --inventory."
              : "inventory refine missed id " + itemId + " — not saving."
          );
          process.exit(1);
        }
      }
      step = "save";
      sock.write(JSON.stringify({ action: "calibration", cmd: "save" }) + "\n");
    } else if (step === "save" && msg.data && msg.data.ok) {
      if (refine && (wantsInventory || itemId != null) && msg.data.hasInventory === false) {
        console.error("inventory not found — equip a backpack or weapon and retry --refine --inventory");
      }
      console.log("profile saved");
      process.exit(0);
    } else if (step === "save" && msg.data && msg.data.ok === false) {
      console.error("save failed (stats not live). profile not written.");
      process.exit(1);
    } else if ((step === "scan" || step === "refine") && msg.data && msg.data.ok === false) {
      console.error(refine ? "refine failed" : "scan failed");
      process.exit(1);
    }
  }
});
sock.on("error", (e) => {
  console.error(e.message);
  process.exitCode = 1;
});

