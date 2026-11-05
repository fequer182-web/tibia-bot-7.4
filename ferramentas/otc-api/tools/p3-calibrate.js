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

const name = arg("name", ["creatureName"]);
if (!name) {
  console.error(
    "usage:\n" +
      "  node tools/p3-calibrate.js --name Rat [--z 7] [--hpperc 47] [--id 12345]\n" +
      "Log in on the map with that creature visible (same floor, on screen).\n" +
      "Needs P1 ready (player position). --name is case-insensitive and matches a substring.\n" +
      "If the bar is not 100%, pass --hpperc (finds the HP field more reliably)."
  );
  process.exit(1);
}

const hints = {
  action: "calibration",
  cmd: "hints",
  creatureName: name,
};
const z = num("z");
if (z != null) {
  if (!Number.isInteger(z) || z < 0 || z > 15) {
    console.error("--z must be an integer from 0 to 15");
    process.exit(1);
  }
  hints.z = z;
}
const hpperc = num("hpperc", ["healthPercent", "hp"]);
if (hpperc != null && (!Number.isInteger(hpperc) || hpperc < 0 || hpperc > 100)) {
  console.error("--hpperc must be an integer from 0 to 100");
  process.exit(1);
}
if (hpperc != null) hints.hpperc = hpperc;
const creatureId = num("id", ["creatureId"]);
if (creatureId != null) hints.creatureId = creatureId;

const PIPE = "\\\\.\\pipe\\OTCApi";
let step = "scan";

const sock = net.connect(PIPE, () => {
  console.log("connected");
  sock.write(JSON.stringify(hints) + "\n");
  sock.write(JSON.stringify({ action: "calibration", cmd: "scanCreatures" }) + "\n");
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
      step = "save";
      sock.write(JSON.stringify({ action: "calibration", cmd: "save" }) + "\n");
      continue;
    }
    if (msg.event !== "calibration") continue;
    if (step === "scan" && msg.data && msg.data.ok) {
      step = "state";
      sock.write(JSON.stringify({ action: "getState", what: "creatures", requestId: "p3" }) + "\n");
    } else if (step === "save" && msg.data && msg.data.ok) {
      if (msg.data.hasCreatures === false) {
        console.error("creatures not in profile");
      }
      console.log("profile saved");
      process.exit(0);
    } else if (step === "scan" && msg.data && msg.data.ok === false) {
      console.error(msg.data.message || "creature scan failed");
      process.exit(1);
    }
  }
});
sock.on("error", (e) => {
  console.error(e.message);
  process.exitCode = 1;
});

