const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { test } = require('node:test');

// Run the production input/physics code with a small rendering substitute.
// No DOM or WebGL is needed to test packet timing, cooldowns and control signs.
function game() {
  let now = 0;
  class Vector {
    constructor(x = 0, y = 0, z = 0) { Object.assign(this, { x, y, z }); }
    set(x, y, z) { Object.assign(this, { x, y, z }); return this; }
    clone() { return new Vector(this.x, this.y, this.z); }
    copy(v) { return this.set(v.x, v.y, v.z); }
    add() { return this; }
    addScaledVector() { return this; }
    applyQuaternion() { return this; }
    normalize() { return this; }
  }
  class Quaternion {
    setFromAxisAngle() { return this; }
    multiply() { return this; }
    normalize() { return this; }
  }
  class Mesh {
    constructor() {
      this.position = new Vector(0, 80, 0);
      this.quaternion = new Quaternion();
      this.userData = { prop: { rotation: { z: 0 } }, navigationLights: [] };
    }
    lookAt() {}
  }
  const elements = new Map();
  const element = id => {
    if (!elements.has(id)) elements.set(id, {
      style: {}, classList: { add() {}, remove() {} }, addEventListener() {},
    });
    return elements.get(id);
  };
  const packets = [], timers = [];
  const context = vm.createContext({
    THREE: {
      Quaternion, Vector3: Vector, Mesh,
      CylinderGeometry: class { rotateX() {} }, MeshBasicMaterial: class {},
    },
    renderer: { domElement: { setAttribute() {}, style: {}, addEventListener() {} } },
    document: { addEventListener() {}, getElementById: element },
    addEventListener() {}, innerWidth: 1000, innerHeight: 700,
    performance: { now: () => now },
    setInterval: fn => timers.push(fn), setTimeout() {}, requestAnimationFrame() {},
    scene: { add() {} },
    WebSocket: class { constructor() { this.readyState = 1; } send(s) { packets.push(JSON.parse(s)); } },
  });
  vm.runInContext(fs.readFileSync(path.join(__dirname, '../config.js'), 'utf8'), context);
  const source = fs.readFileSync(path.join(__dirname, '../game.js'), 'utf8');
  vm.runInContext(source.split('/* ===== Initialise ===== */')[0], context);
  vm.runInContext('ws = new WebSocket(); S.running = true; player.mesh = new THREE.Mesh();', context);
  return {
    run: code => vm.runInContext(code, context),
    packet: data => {
      context.packet = { type: 'sensor', pitch: 0.4, roll: -0.6, yaw: 0.2,
        joyX: -0.3, joyY: 0.7, tiltValid: true,
        fireSession: 1, fireCount: 0, firePending: 0, fire: false,
        ldrValid: false, ...data };
      vm.runInContext('receiveSensor(packet)', context);
    },
    time: ms => { now = ms; }, packets, timers, element,
  };
}

test('controller axes persist across frames and preserve every sign', () => {
  const g = game();
  g.packet({});
  for (const now of [0, 16, 100, 250, 300]) {
    g.time(now);
    g.run('readInputs()');
    assert.equal(g.run('S.pIn'), 0.4);
    assert.equal(g.run('S.rIn'), -0.6);
    assert.equal(g.run('S.yIn'), 0.2);
    assert.equal(g.run('S.camX'), -0.3);
    assert.equal(g.run('S.camY'), 0.7);
  }
});

test('stale/disconnected controller falls back to original keyboard controls', () => {
  const g = game();
  g.packet({});
  g.run("keys.KeyW = true; keys.KeyA = true; keys.KeyQ = true; keys.ShiftLeft = true; keys.Space = true;");
  g.time(301);
  g.run('readInputs()');
  assert.equal(g.run('S.pIn'), -1);
  assert.equal(g.run('S.rIn'), 1);
  assert.equal(g.run('S.yIn'), 1);
  assert.equal(g.run('S.tIn'), 1);
  assert.equal(g.run('S.fire'), true);
  g.packet({});
  g.run('ws.readyState = 3; readInputs()');
  assert.equal(g.run('S.pIn'), -1);
});

test('calibration/fault neutralizes tilt while camera and fire remain usable', () => {
  const g = game();
  g.packet({ tiltValid: false, fire: true, fireCount: 1, firePending: 1 });
  g.run('readInputs()');
  assert.equal(g.run('S.pIn + S.rIn + S.yIn'), 0);
  assert.equal(g.run('S.camY'), 0.7);
  assert.equal(g.run('S.fire'), true);
});

test('presses survive false packets, render delays and weapon cooldown exactly once', () => {
  const g = game();
  g.packet({ fire: true, fireCount: 2, firePending: 2 });
  g.packet({ fireCount: 2 });  // A false packet before rendering cannot erase presses.
  assert.equal(g.run('controller.pendingFire'), 2);
  g.run('S.fireCD = 5; readInputs(); updatePlayer()');
  assert.equal(g.run('controller.pendingFire'), 2);
  assert.equal(g.run('projectiles.length'), 0);
  g.run('for (let i = 0; i < 20; i++) { readInputs(); updatePlayer(); }');
  assert.equal(g.run('projectiles.length'), 2);
  assert.equal(g.run('controller.pendingFire'), 0);
  g.packet({ fire: true, fireCount: 2, firePending: 2 }); // Duplicate cannot refire.
  g.run('for (let i = 0; i < 20; i++) { readInputs(); updatePlayer(); }');
  assert.equal(g.run('projectiles.length'), 2);
  g.packet({ fireCount: 4 }); // Recover two presses from skipped sensor packets.
  assert.equal(g.run('controller.pendingFire'), 2);
});

test('button counter rollover, restart and repeated packets do not invent shots', () => {
  const g = game();
  g.packet({ fireCount: 0xffffffff });
  g.packet({ fireCount: 0, fire: true });
  assert.equal(g.run('controller.pendingFire'), 1);
  g.packet({ fireSession: 2, fireCount: 0 });
  assert.equal(g.run('controller.pendingFire'), 0);
  g.packet({ fireSession: 2, fireCount: 1, firePending: 1 });
  g.run('clearController()');
  g.packet({ fireSession: 2, fireCount: 1, firePending: 1 });
  assert.equal(g.run('controller.pendingFire'), 0);
});

test('held Space retains original repeated firing, independent of controller presses', () => {
  const g = game();
  g.run("keys.Space = true; for (let i = 0; i < 21; i++) { readInputs(); updatePlayer(); }");
  assert.equal(g.run('projectiles.length'), 3);
});

test('malformed axes do not refresh timeout and stale LDR cannot hold stealth', () => {
  const g = game();
  g.packet({ ldrValid: true, ldrRaw: 1000, dark: true });
  g.run('readInputs()');
  assert.equal(g.run('S.stealth'), true);
  g.time(301);
  g.packet({ pitch: 'bad' });
  g.run('readInputs()');
  assert.equal(g.run('controller.at'), -Infinity);
  assert.equal(g.run('S.stealth'), false);
  g.packet({ ldrValid: false, dark: true });
  g.run('readInputs()');
  assert.equal(g.run('S.stealth'), false);
});

test('state heartbeat maps hull to three gauge segments and runs after game over', () => {
  const g = game();
  for (const [health, lives] of [[100, 3], [67, 3], [66, 2], [34, 2], [33, 1], [1, 1], [0, 0]]) {
    g.run(`S.health = ${health}; sendGameState()`);
    assert.equal(g.packets.at(-1).lives, lives);
  }
  g.run('S.running = false; S.over = true; S.score = 123');
  g.timers[0]();
  assert.deepEqual(g.packets.at(-1), { type: 'game_state', lives: 0, score: 123, mode: 'OVER' });
  g.run("sendEvent('OVER', 123)");
  assert.equal(g.packets.at(-2).type, 'game_state');
  assert.equal(g.packets.at(-1).event, 'OVER');
  const before = g.packets.length;
  g.run("sendEvent('ROLL', 30); sendEvent('SCORE', 123); sendEvent('BOOST', 0)");
  assert.equal(g.packets.length, before);
});

test('WebSocket close clears axes immediately and initial open publishes state', () => {
  const g = game();
  g.element('ws-url').value = 'wss://test';
  g.run('ws = null; toggleWS(); ws.onopen()');
  assert.equal(g.packets[0].type, 'identify');
  assert.equal(g.packets[1].type, 'game_state');
  g.packet({ fire: true, fireCount: 1, firePending: 1 });
  g.run('ws.onclose(); readInputs()');
  assert.equal(g.run('S.pIn'), 0);
  assert.equal(g.run('controller.pendingFire'), 0);
});
