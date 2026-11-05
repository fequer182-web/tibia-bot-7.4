"use strict";

const net = require("net");

function restAfter(name) {
  const i = process.argv.indexOf("--" + name);
  if (i < 0) return undefined;
  const parts = [];
  for (let j = i + 1; j < process.argv.length; j++) {
    if (process.argv[j].indexOf("--") === 0) break;
    parts.push(process.argv[j]);
  }
  if (!parts.length) return undefined;
  return parts.join(" ");
}

function hasFlag(name) {
  return process.argv.indexOf("--" + name) >= 0;
}

const say = restAfter("say");
const step = restAfter("step");
const stop = hasFlag("stop");
const logout = hasFlag("logout");
const walkRaw = restAfter("walkTo");
const useRaw = restAfter("useItem");
const useInvRaw = restAfter("useInv");
const moveRaw = restAfter("moveItem");
const moveInvRaw = restAfter("moveInv");
const countRaw = restAfter("count");
const attackRaw = restAfter("attack");
const attackAt = parseWalkTo(restAfter("attackAt"));
const cancelAttack = hasFlag("cancelAttack");
const followRaw = restAfter("follow");
const cancelFollow = hasFlag("cancelFollow");
const attackFollow = hasFlag("follow");
const attackNoFollow = hasFlag("noFollow");
const lightRaw = restAfter("light");
const lightOff = hasFlag("lightOff");
const lightOn = hasFlag("light");
const lightIntensity = lightRaw == null ? 1 : Number(lightRaw);
if (lightOn && (!Number.isFinite(lightIntensity) || lightIntensity < 0 || lightIntensity > 1)) {
  console.error("--light intensity must be a number from 0 to 1");
  process.exit(1);
}

function parseWalkTo(raw) {
  if (raw == null) return undefined;
  const parts = raw.trim().split(/\s+/);
  if (parts.length < 3) return undefined;
  const x = Number(parts[0]);
  const y = Number(parts[1]);
  const z = Number(parts[2]);
  if (![x, y, z].every((n) => Number.isFinite(n))) return undefined;
  return { x: Math.trunc(x), y: Math.trunc(y), z: Math.trunc(z) };
}

function parseInts(raw, min) {
  if (raw == null) return undefined;
  const parts = raw.trim().split(/\s+/);
  if (parts.length < min) return undefined;
  const nums = parts.map((p) => Number(p));
  if (!nums.every((n) => Number.isFinite(n))) return undefined;
  return nums.map((n) => Math.trunc(n));
}

const walkTo = parseWalkTo(walkRaw);
const lookTile = parseWalkTo(restAfter("look"));
const clickTile = parseWalkTo(restAfter("click"));
const lookItem = parseInts(restAfter("lookItem"), 2);
const lookInv = parseInts(restAfter("lookInv"), 1);
const lookCreatureRaw = restAfter("lookCreature");
const lookCreatureId = (() => {
  if (lookCreatureRaw == null) return undefined;
  const n = Number(String(lookCreatureRaw).trim().split(/\s+/)[0]);
  if (!Number.isFinite(n)) return undefined;
  return Math.trunc(n) >>> 0;
})();
const useItem = parseInts(useRaw, 2);
const useInv = parseInts(useInvRaw, 1);
const moveItem = parseInts(moveRaw, 2);
const moveInv = parseInts(moveInvRaw, 1);
const moveCount = (() => {
  if (countRaw == null) return undefined;
  const n = Number(String(countRaw).trim().split(/\s+/)[0]);
  if (!Number.isFinite(n)) return undefined;
  return Math.trunc(n);
})();
const attackId = (() => {
  if (attackRaw == null) return undefined;
  const n = Number(String(attackRaw).trim().split(/\s+/)[0]);
  if (!Number.isFinite(n)) return undefined;
  return Math.trunc(n) >>> 0;
})();
const followId = (() => {
  if (followRaw == null) return undefined;
  const n = Number(String(followRaw).trim().split(/\s+/)[0]);
  if (!Number.isFinite(n)) return undefined;
  return Math.trunc(n) >>> 0;
})();
const toSelf = hasFlag("toSelf");
const toCreatureRaw = restAfter("toCreature");
const toPos = parseWalkTo(restAfter("to"));
const toItem = parseInts(restAfter("toItem"), 2);
const toInv = parseInts(restAfter("toInv"), 1);

if (say == null && step == null && !stop && !logout && !walkTo && !useItem && !useInv && !moveItem &&
    !moveInv && attackId == null && !attackAt && !cancelAttack && followId == null && !cancelFollow &&
    !lookTile && !lookItem && !lookInv && lookCreatureId == null && !clickTile && !lightOn && !lightOff) {
  console.error(
    "usage:\n" +
      "  node tools/p4-test.js --say exura\n" +
      "  node tools/p4-test.js --step north\n" +
      "  node tools/p4-test.js --logout\n" +
      "  node tools/p4-test.js --walkTo 32097 32191 8\n" +
      "  node tools/p4-test.js --useItem 0 0\n" +
      "  node tools/p4-test.js --useInv 3\n" +
      "  node tools/p4-test.js --useInv 6 --toSelf\n" +
      "  node tools/p4-test.js --useItem 0 1 --toCreature 1073793728\n" +
      "  node tools/p4-test.js --useItem 0 1 --to 32097 32191 7\n" +
      "  node tools/p4-test.js --useItem 0 1 --toItem 0 2\n" +
      "  node tools/p4-test.js --moveItem 0 0 --toItem 0 1\n" +
      "  node tools/p4-test.js --moveItem 0 0 --toInv 4\n" +
      "  node tools/p4-test.js --moveItem 0 0 --to 32493 32259 7\n" +
      "  node tools/p4-test.js --moveInv 3 --toItem 0 0 --count 1\n" +
      "  node tools/p4-test.js --attack 1073793728\n" +
      "  node tools/p4-test.js --attackAt 32034 32210 7\n" +
      "  node tools/p4-test.js --attack 1073793728 --follow\n" +
      "  node tools/p4-test.js --attack 1073793728 --noFollow\n" +
      "  node tools/p4-test.js --cancelAttack\n" +
      "  node tools/p4-test.js --follow 1073793728\n" +
      "  node tools/p4-test.js --cancelFollow\n" +
      "  node tools/p4-test.js --look 32097 32191 8\n" +
      "  node tools/p4-test.js --lookItem 0 0\n" +
      "  node tools/p4-test.js --lookInv 6\n" +
      "  node tools/p4-test.js --lookCreature 1073793728\n" +
      "  node tools/p4-test.js --click 32346 32224 7\n" +
      "  node tools/p4-test.js --light 1\n" +
      "  node tools/p4-test.js --lightOff\n" +
      "--useItem/--moveItem is window index + slot (0-based). --useInv/--moveInv is inventory 1–10.\n" +
      "--toSelf / --toCreature id / --to x y z / --toItem c s / --toInv slot = dest.\n" +
      "--count N optional on move (omit = whole stack). --attack/--follow use P3 creature id.\n" +
      "--attackAt x y z resolves the current P3 id immediately before attacking.\n" +
      "--follow on --attack turns chase on; --noFollow turns chase off. Omit = leave chase as-is.\n" +
      "--look is a map SQM. --lookItem c s is a container slot. --lookInv 1–10. Shift+click also emits look.\n" +
      "--click x y z uses the top thing on that SQM (ladder). Walk-on stairs: --walkTo the same tile."
  );
  process.exit(1);
}

function attachTo(cmd) {
  if (toSelf) cmd.to = { self: true };
  else if (toCreatureRaw != null) {
    const id = Number(toCreatureRaw.trim().split(/\s+/)[0]);
    if (!Number.isFinite(id)) {
      console.error("--toCreature needs a creature id from P3");
      process.exit(1);
    }
    cmd.to = { creatureId: Math.trunc(id) };
  } else if (toPos) cmd.to = { x: toPos.x, y: toPos.y, z: toPos.z };
  else if (toItem) cmd.to = { container: toItem[0], slot: toItem[1] };
  else if (toInv) cmd.to = { slot: toInv[0] };
}

let cmd;
if (lightOff) cmd = { action: "setLight", enabled: false, requestId: "p4" };
else if (lightOn) cmd = { action: "setLight", enabled: true, intensity: lightIntensity, requestId: "p4" };
else if (say != null) cmd = { action: "say", text: say, requestId: "p4" };
else if (step != null) cmd = { action: "step", direction: step, requestId: "p4" };
else if (logout) cmd = { action: "logout", requestId: "p4" };
else if (walkTo) cmd = { action: "walkTo", x: walkTo.x, y: walkTo.y, z: walkTo.z, requestId: "p4" };
else if (useItem) {
  cmd = { action: "useItem", container: useItem[0], slot: useItem[1], requestId: "p4" };
  if (useItem[2]) cmd.id = useItem[2];
  attachTo(cmd);
} else if (useInv) {
  cmd = { action: "useItem", slot: useInv[0], requestId: "p4" };
  if (useInv[1]) cmd.id = useInv[1];
  attachTo(cmd);
} else if (moveItem) {
  cmd = { action: "moveItem", container: moveItem[0], slot: moveItem[1], requestId: "p4" };
  if (moveItem[2]) cmd.id = moveItem[2];
  attachTo(cmd);
  if (!cmd.to) {
    console.error("moveItem needs --toItem c s, --toInv slot, or --to x y z");
    process.exit(1);
  }
  if (moveCount != null) cmd.count = moveCount;
} else if (moveInv) {
  cmd = { action: "moveItem", slot: moveInv[0], requestId: "p4" };
  if (moveInv[1]) cmd.id = moveInv[1];
  attachTo(cmd);
  if (!cmd.to) {
    console.error("moveInv needs --toItem c s, --toInv slot, or --to x y z");
    process.exit(1);
  }
  if (moveCount != null) cmd.count = moveCount;
} else if (attackAt) {
  cmd = undefined;
} else if (attackId != null) {
  cmd = { action: "attack", creatureId: attackId, requestId: "p4" };
  if (attackNoFollow) cmd.follow = false;
  if (attackFollow) cmd.follow = true;
}
else if (cancelAttack) cmd = { action: "cancelAttack", requestId: "p4" };
else if (followId != null) cmd = { action: "follow", creatureId: followId, requestId: "p4" };
else if (cancelFollow) cmd = { action: "cancelFollow", requestId: "p4" };
else if (lookTile)
  cmd = { action: "look", x: lookTile.x, y: lookTile.y, z: lookTile.z, requestId: "p4" };
else if (lookItem) {
  cmd = { action: "look", container: lookItem[0], slot: lookItem[1], requestId: "p4" };
  if (lookItem[2]) cmd.id = lookItem[2];
} else if (lookInv) {
  cmd = { action: "look", slot: lookInv[0], requestId: "p4" };
  if (lookInv[1]) cmd.id = lookInv[1];
} else if (lookCreatureId != null)
  cmd = { action: "look", creatureId: lookCreatureId, requestId: "p4" };
else if (clickTile)
  cmd = { action: "click", x: clickTile.x, y: clickTile.y, z: clickTile.z, requestId: "p4" };
else cmd = { action: "stop", requestId: "p4" };

const PIPE = "\\\\.\\pipe\\OTCApi";
const sock = net.connect(PIPE, () => {
  if (attackAt) {
    console.log("connected, resolving current creature at", attackAt);
    sock.write(JSON.stringify({action: "getState", what: "creatures", requestId: "p4-resolve"}) + "\n");
  } else {
    console.log("connected, sending", JSON.stringify(cmd));
    sock.write(JSON.stringify(cmd) + "\n");
  }
});

sock.setEncoding("utf8");
let buf = "";
let done = false;
let attackResolved = false;

const timer = setTimeout(() => {
  if (done) return;
  console.error("timeout waiting for action reply (is the DLL injected?)");
  process.exitCode = 1;
  sock.destroy();
}, 15000);

sock.on("data", (chunk) => {
  buf += chunk;
  for (;;) {
    const n = buf.indexOf("\n");
    if (n < 0) break;
    const line = buf.slice(0, n).replace(/\r$/, "");
    buf = buf.slice(n + 1);
    if (!line) continue;
    let msg;
    try {
      msg = JSON.parse(line);
    } catch {
      continue;
    }
    if (attackAt && !attackResolved && msg.event === "state" && msg.data &&
        msg.data.requestId === "p4-resolve" && msg.data.what === "creatures") {
      const creatures = Array.isArray(msg.data.data) ? msg.data.data : [];
      const matches = creatures.filter((c) => c && c.x === attackAt.x && c.y === attackAt.y &&
        c.z === attackAt.z && Number.isInteger(c.id) && c.id > 0);
      if (matches.length !== 1) {
        done = true;
        clearTimeout(timer);
        console.error(matches.length ? "more than one creature at that position" :
          "no current creature at that position");
        process.exitCode = 1;
        sock.end();
        continue;
      }
      attackResolved = true;
      cmd = {action: "attack", creatureId: matches[0].id, requestId: "p4"};
      if (attackNoFollow) cmd.follow = false;
      if (attackFollow) cmd.follow = true;
      console.log("resolved", matches[0].name, matches[0].id, "sending", JSON.stringify(cmd));
      sock.write(JSON.stringify(cmd) + "\n");
      continue;
    }
    if (msg.event !== "action") continue;
    done = true;
    clearTimeout(timer);
    console.log(JSON.stringify(msg, null, 2));
    if (!msg.data || !msg.data.ok) process.exitCode = 1;
    sock.end();
  }
});

sock.on("error", (err) => {
  clearTimeout(timer);
  console.error("pipe error:", err.message);
  process.exitCode = 1;
});
sock.on("close", () => {
  if (done) return;
  clearTimeout(timer);
  console.error("pipe closed before action reply (client crashed or DLL hung)");
  process.exitCode = 1;
});

