"use strict";

const net = require("net");

const PIPE = "\\\\.\\pipe\\OTCApi";

function arg(name) {
  const i = process.argv.indexOf("--" + name);
  if (i >= 0 && i + 1 < process.argv.length) return process.argv[i + 1];
  return undefined;
}

const only = arg("only") || (process.argv.indexOf("--containers") >= 0 ? "containers" : "") ||
  (process.argv.indexOf("--creatures") >= 0 || process.argv.indexOf("--battle") >= 0
    ? "creatures"
    : "") ||
  (process.argv.indexOf("--map") >= 0 || process.argv.indexOf("--tiles") >= 0 ? "map" : "") ||
  (process.argv.indexOf("--look") >= 0 ? "look" : "");

let input;
let corpseId;
let pendingWatch;
let requestSequence = 0;
function send(command) {
  sock.write(JSON.stringify(command) + "\n");
}

function corpseCommand(line) {
  const [command, ...args] = line.trim().split(/\s+/);
  if (!command) return;
  if (command === "state" && !args.length) {
    send({action: "getState", what: "corpse"});
  } else if (command === "watch") {
    const ids = args.length ? args.join(",").split(",").map(Number) : [];
    if (ids.length > 32 || ids.some(id => !Number.isInteger(id) || id < 1 || id > 65535)) {
      console.error("Use watch <itemId> [itemId ...], IDs inteiros de 1 a 65535 (max. 32).");
      return;
    }
    if (pendingWatch) {
      console.error("Aguarde a consulta do alvo atual.");
      return;
    }
    pendingWatch = {ids, requestId: "corpse-target-" + ++requestSequence};
    send({action: "getState", what: "player", requestId: pendingWatch.requestId});
  } else if (command === "open" && args.length <= 1) {
    const id = args.length ? Number(args[0]) : corpseId;
    if (!Number.isSafeInteger(id) || id <= 0) {
      console.error("Nenhum corpseId recebido. Use state ou open <corpseId>.");
      return;
    }
    send({action: "openCorpse", corpseId: id, requestId: "corpse-open-" + ++requestSequence});
  } else {
    console.log("Comandos: watch [itemId ...] | open [corpseId] | state | quit");
    if (command === "quit") { input.close(); sock.end(); }
  }
}

function consumeCorpseMessage(msg) {
  if (only !== "corpse") return;
  const data = msg.data || {};
  if (msg.event === "state" && pendingWatch && data.requestId === pendingWatch.requestId) {
    const watch = pendingWatch;
    pendingWatch = undefined;
    const id = data.data && data.data.targetCreatureId;
    if (!Number.isInteger(id) || id <= 0) {
      console.error("Sem alvo atual. Comece a atacar e repita watch antes da morte.");
    } else {
      send({action: "watchCorpse", creatureId: id,
        ...(watch.ids.length ? {corpseItemIds: watch.ids} : {}),
        requestId: "corpse-watch-" + ++requestSequence});
    }
  }
  const state = msg.event === "state" && data.what === "corpse" ? data.data :
    msg.event === "corpse" ? data : null;
  if (state) corpseId = Number.isSafeInteger(state.corpseId) ? state.corpseId : undefined;
}

function keep(msg) {
  if (!only) return true;
  const ev = msg.event || "";
  if (only === "deaths") {
    return ev === "monsterEnd" || ev === "dllStatus" ||
      (ev === "state" && msg.data && ["monsterEnd", "creatures"].includes(msg.data.what));
  }
  if (only === "corpse") {
    return ["monsterEnd", "corpse", "dllStatus", "containerOpen", "look", "attackTarget"].includes(ev) ||
      (ev === "state" && msg.data && msg.data.what === "corpse") ||
      (ev === "action" && msg.data && ["watchCorpse", "openCorpse"].includes(msg.data.action));
  }
  if (only === "containers" || only === "container") {
    if (ev.indexOf("container") === 0) return true;
    if (ev === "state" && msg.data && msg.data.what === "containers") return true;
    return false;
  }
  if (only === "creatures" || only === "creature" || only === "battle") {
    if (ev.indexOf("creature") === 0) return true;
    if (ev === "state" && msg.data &&
        (msg.data.what === "creatures" || msg.data.what === "battle"))
      return true;
    return false;
  }
  if (only === "map" || only === "tiles" || only === "tile") {
    if (ev === "state" && msg.data &&
        (msg.data.what === "map" || msg.data.what === "tiles" || msg.data.what === "tile"))
      return true;
    return false;
  }
  if (only === "look") {
    if (ev === "look") return true;
    if (ev === "state" && msg.data && msg.data.what === "look") return true;
    if (ev === "action" && msg.data && msg.data.action === "look") return true;
    return false;
  }
  return ev === only;
}

const sock = net.connect(PIPE, () => {
  console.log("connected", PIPE, only ? "(only " + only + ")" : "");
  sock.write('{"action":"ping"}\n');
  if (only === "deaths") {
    send({action: "getState", what: "monsterEnd", requestId: "last-monster"});
    send({action: "getState", what: "creatures", requestId: "visible-monsters"});
    console.log("Acompanhando monstros visiveis automaticamente, sem watch. Mate um monstro na tela.");
    console.log("monsterEnd: deathObserved=true indica HP 0; false indica apenas desaparecimento/ultima posicao.");
  } else if (only === "corpse") {
    send({action: "getState", what: "corpse", requestId: "watch"});
    console.log("Corpos: watch [itemId ...] | open [corpseId] | state | quit");
    console.log("Coordenadas monsterEnd aparecem automaticamente, sem watch. Modo simples: --only deaths.");
    console.log("watch sem ID identifica o corpo do alvo atual. open aproxima; repita ao chegar para usar.");
    input = require("readline").createInterface({input: process.stdin, terminal: false});
    input.on("line", corpseCommand);
  } else if (only === "containers" || only === "container") {
    sock.write('{"action":"getState","what":"containers","requestId":"watch"}\n');
  } else if (only === "creatures" || only === "creature" || only === "battle") {
    sock.write('{"action":"getState","what":"creatures","requestId":"watch"}\n');
  } else if (only === "map" || only === "tiles" || only === "tile") {
    sock.write('{"action":"getState","what":"map","requestId":"watch"}\n');
  } else if (only === "look") {
    sock.write('{"action":"getState","what":"look","requestId":"watch"}\n');
    console.log("Shift+click an item or SQM in the client — look events print here");
  }
});

sock.setEncoding("utf8");
let buf = "";
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
      if (!only) console.log(line);
      continue;
    }
    consumeCorpseMessage(msg);
    if (!keep(msg)) continue;
    console.log(JSON.stringify(msg, null, 2));
  }
});
sock.on("error", (err) => {
  console.error("pipe error:", err.message);
  process.exitCode = 1;
});
sock.on("close", () => {
  if (input) input.close();
  console.log("disconnected");
});

