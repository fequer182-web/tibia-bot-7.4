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

const name = arg("name", ["containerName"]);
const capacity = num("capacity", ["containerCapacity", "cap"]);

if (!name || capacity == null) {
  console.error(
    "usage:\n" +
      "  node tools/p2-calibrate.js --name Backpack --capacity 20 [--id 3031]\n" +
      "Open that backpack window (not just equipped) with at least one item.\n" +
      "--name is case-insensitive; --capacity is window slots, not player cap."
  );
  process.exit(1);
}

const hints = {
  action: "calibration",
  cmd: "hints",
  containerName: name,
  containerCapacity: capacity,
};
const itemId = num("id", ["itemId"]);
if (itemId != null) hints.itemId = itemId;

const PIPE = "\\\\.\\pipe\\OTCApi";
let step = "scan";

const sock = net.connect(PIPE, () => {
  console.log("connected");
  sock.write(JSON.stringify(hints) + "\n");
  sock.write(JSON.stringify({ action: "calibration", cmd: "scanContainers" }) + "\n");
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
      sock.write(JSON.stringify({ action: "getState", what: "containers", requestId: "p2" }) + "\n");
    } else if (step === "save" && msg.data && msg.data.ok) {
      if (msg.data.hasContainers === false) {
        console.error("containers not in profile");
      }
      console.log("profile saved");
      process.exit(0);
    } else if (step === "scan" && msg.data && msg.data.ok === false) {
      console.error(msg.data.message || "container scan failed");
      process.exit(1);
    }
  }
});
sock.on("error", (e) => {
  console.error(e.message);
  process.exitCode = 1;
});

