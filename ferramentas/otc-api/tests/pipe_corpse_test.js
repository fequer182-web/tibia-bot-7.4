"use strict";
const assert = require("node:assert/strict");
const fs = require("node:fs");
const vm = require("node:vm");
const {EventEmitter} = require("node:events");
const path = require("node:path");
const writes = [];
const socket = new EventEmitter();
socket.write = line => writes.push(JSON.parse(line));
socket.setEncoding = () => {};
socket.end = () => {};
const input = new EventEmitter(); input.close = () => {};
let connected;
vm.runInNewContext(fs.readFileSync(path.join(__dirname, "../tools/pipe_test.js"), "utf8"), {
  require(name) {
    if (name === "net") return {connect: (_, cb) => { connected = cb; return socket; }};
    if (name === "readline") return {createInterface: () => input};
    throw new Error(name);
  },
  process: {argv: ["node", "pipe_test.js", "--only", "corpse"], stdin: {}},
  console: {log() {}, error() {}},
});
connected();
function replyTarget(id) {
  const request = writes.at(-1);
  assert.equal(request.what, "player");
  const wire = JSON.stringify({event: "state", data: {
    what: "player", requestId: request.requestId, data: {targetCreatureId: id},
  }}) + "\n";
  socket.emit("data", wire.slice(0, 17)); socket.emit("data", wire.slice(17));
}
input.emit("line", "watch"); replyTarget(1073793728);
assert.equal(writes.at(-1).action, "watchCorpse");
assert.equal(writes.at(-1).creatureId, 1073793728);
assert.equal("corpseItemIds" in writes.at(-1), false);
input.emit("line", "watch 1234 5678"); replyTarget(1073793728);
assert.deepEqual(writes.at(-1).corpseItemIds, [1234, 5678]);
input.emit("line", "watch"); const before = writes.length; replyTarget(0);
assert.equal(writes.length, before);
input.emit("line", "watch -1"); assert.equal(writes.length, before);
socket.emit("data", JSON.stringify({event: "corpse", data: {corpseId: 7, status: "available"}}) + "\n");
input.emit("line", "open"); assert.equal(writes.at(-1).corpseId, 7);
assert.equal(writes.at(-1).action, "openCorpse");
console.log("pipe corpse commands passed");

const passiveWrites = [], printed = [];
const passiveSocket = new EventEmitter();
passiveSocket.write = line => passiveWrites.push(JSON.parse(line));
passiveSocket.setEncoding = () => {};
let passiveConnected;
vm.runInNewContext(fs.readFileSync(path.join(__dirname, "../tools/pipe_test.js"), "utf8"), {
  require(name) {
    assert.equal(name, "net", "passive mode must not create interactive input");
    return {connect: (_, cb) => { passiveConnected = cb; return passiveSocket; }};
  },
  process: {argv: ["node", "pipe_test.js", "--only", "deaths"]},
  console: {log(value) { printed.push(value); }, error() {}},
});
passiveConnected();
assert.deepEqual(passiveWrites.map(r => r.action), ["ping", "getState", "getState"]);
assert.equal(passiveWrites[1].what, "monsterEnd");
assert.equal(passiveWrites[2].what, "creatures");
const wireEnd = JSON.stringify({event: "monsterEnd", data: {creatureId: 42, x: 10, y: 20, z: 7,
  deathObserved: false, reason: "disappeared"}}) + "\n";
passiveSocket.emit("data", wireEnd.slice(0, 9)); passiveSocket.emit("data", wireEnd.slice(9));
assert.equal(JSON.parse(printed.at(-1)).data.deathObserved, false);
assert.equal(passiveWrites.length, 3, "events must not trigger actions");
console.log("passive monster coordinates passed");

