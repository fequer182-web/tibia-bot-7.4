"use strict";

const net = require("net");

function arg(name, aliases) {
  const keys = [name].concat(aliases || []);
  for (const k of keys) {
    const i = process.argv.indexOf("--" + k);
    if (i >= 0 && i + 1 < process.argv.length) return process.argv[i + 1];
  }
  return undefined;
}

function num(name, aliases) {
  const v = arg(name, aliases);
  return v == null ? undefined : Number(v);
}

if (process.argv.indexOf("--help") >= 0 || process.argv.indexOf("-h") >= 0) {
  console.error(
    "usage:\n" +
      "  node tools/p5-calibrate.js [--id 351]\n" +
      "Log in on the map (P1 position). Needs actions.mapRva from P4.\n" +
      "--id is optional: client id of an item on the player's tile (ground or dropped)."
  );
  process.exit(1);
}

const itemId = num("id", ["itemId", "mapItemId"]);
if (itemId != null && (!Number.isInteger(itemId) || itemId < 100 || itemId >= 60000)) {
  console.error("--id must be a client item id from 100 to 59999 (not a creature id)");
  process.exit(1);
}
const PIPE = "\\\\.\\pipe\\OTCApi";
let step = "scan";

const sock = net.connect(PIPE, () => {
  console.log("connected");
  // Always replace the previous P5 hint. Zero explicitly means no item filter.
  sock.write(
    JSON.stringify({ action: "calibration", cmd: "hints", mapItemId: itemId ?? 0 }) + "\n"
  );
  sock.write(JSON.stringify({ action: "calibration", cmd: "scanMap" }) + "\n");
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
    if (msg.event === "state" && step === "state") {
      const tiles = msg.data && msg.data.data;
      if (!Array.isArray(tiles) || tiles.length === 0) {
        console.error("getState what=map returned no tiles");
        process.exit(1);
      }
      step = "save";
      sock.write(JSON.stringify({ action: "calibration", cmd: "save" }) + "\n");
      continue;
    }
    if (msg.event !== "calibration") continue;
    if (step === "scan" && msg.data && msg.data.ok) {
      step = "state";
      sock.write(JSON.stringify({ action: "getState", what: "map", requestId: "p5" }) + "\n");
    } else if (step === "save" && msg.data && msg.data.ok) {
      if (msg.data.hasMap === false) {
        console.error("map not in profile");
      }
      console.log("profile saved");
      process.exit(0);
    } else if (step === "scan" && msg.data && msg.data.ok === false) {
      console.error(msg.data.message || "map scan failed");
      process.exit(1);
    }
  }
});
sock.on("error", (e) => {
  console.error(e.message);
  process.exitCode = 1;
});

