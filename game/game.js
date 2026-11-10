/* ===== game.js — Player, input, camera, collisions, HUD, wave system & main loop ===== */

/* ===== Temp math objects (player / camera) ===== */
const _qP    = new THREE.Quaternion();
const _qR    = new THREE.Quaternion();
const _qY    = new THREE.Quaternion();
const _qBY   = new THREE.Quaternion();
const _axisX = new THREE.Vector3(1, 0, 0);
const _axisY = new THREE.Vector3(0, 1, 0);
const _axisZ = new THREE.Vector3(0, 0, 1);
const _fwd   = new THREE.Vector3();

/* ===== Player ===== */
const player = { mesh: null };

function initPlayer() {
  if (player.mesh) scene.remove(player.mesh);
  player.mesh = makeplane(0x2266cc, 0xffffff);
  player.mesh.position.set(0, C.START_ALT, 0);

  // Gun barrels (visual) — center nose + left/right wing
  const gunMat = new THREE.MeshPhongMaterial({ color: 0x444444, flatShading: true });
  const gunGeo = new THREE.CylinderGeometry(0.06, 0.06, 1.0, 5);
  gunGeo.rotateX(Math.PI / 2);

  const gunC = new THREE.Mesh(gunGeo, gunMat);
  gunC.position.set(0, -0.15, -2.6);
  player.mesh.add(gunC);

  const gunL = new THREE.Mesh(gunGeo, gunMat);
  gunL.position.set(-2.8, -0.12, -0.3);
  player.mesh.add(gunL);

  const gunR = new THREE.Mesh(gunGeo, gunMat);
  gunR.position.set(2.8, -0.12, -0.3);
  player.mesh.add(gunR);

  scene.add(player.mesh);
}

/* ===== Player projectile ===== */
class Projectile {
  constructor(pos, dir) {
    const g = new THREE.CylinderGeometry(0.18, 0.18, 1.4, 6);
    g.rotateX(Math.PI / 2);
    this.mesh = new THREE.Mesh(g, new THREE.MeshBasicMaterial({ color: 0xff6600 }));
    this.mesh.position.copy(pos);
    this.dir = dir.clone().normalize();
    this.mesh.lookAt(pos.clone().add(this.dir));
    this.life = 0;
    this.alive = true;
    scene.add(this.mesh);
  }
  update() {
    this.mesh.position.addScaledVector(this.dir, C.PROJ_SPEED);
    wrapPos(this.mesh.position);
    this.life++;
    if (this.life > C.PROJ_LIFE) this.alive = false;
  }
  remove() { scene.remove(this.mesh); }
}

/* ===== Particles ===== */
function spawnExplosion(pos) {
  const colors = [0xff6600, 0xffaa00, 0xff3300, 0xffcc44];
  for (let i = 0; i < 18; i++) {
    const g = new THREE.BoxGeometry(0.4, 0.4, 0.4);
    const m = new THREE.Mesh(g, new THREE.MeshBasicMaterial({ color: colors[Math.floor(Math.random() * 4)] }));
    m.position.copy(pos);
    const vel = new THREE.Vector3(
      (Math.random() - 0.5) * 3,
      (Math.random() - 0.5) * 3,
      (Math.random() - 0.5) * 3
    );
    scene.add(m);
    particles.push({ mesh: m, vel, life: 50 + Math.random() * 30 });
  }
}

function updateParticles() {
  for (let i = particles.length - 1; i >= 0; i--) {
    const p = particles[i];
    p.mesh.position.add(p.vel);
    p.vel.multiplyScalar(0.95);
    p.vel.y -= 0.02;
    p.life--;
    p.mesh.material.opacity = p.life / 60;
    p.mesh.material.transparent = true;
    const s = 0.5 + (1 - p.life / 80) * 1.5;
    p.mesh.scale.set(s, s, s);
    if (p.life <= 0) {
      scene.remove(p.mesh);
      particles.splice(i, 1);
    }
  }
}

/* ===== Input ===== */
renderer.domElement.setAttribute('tabindex', '0');
renderer.domElement.style.outline = 'none';
renderer.domElement.addEventListener('click', () => renderer.domElement.focus());

document.addEventListener('keydown', e => {
  if (e.target.tagName === 'INPUT') return;
  keys[e.code] = true;
  if (e.code === 'KeyI' && !e.repeat) {
    showInputs = !showInputs;
    document.getElementById('input-debug').style.display = showInputs ? 'block' : 'none';
  }
  if (e.code === 'KeyY' && !e.repeat) toggleYawMode();
  if (e.key === ' ' || e.keyCode === 32) keys['Space'] = true;
  e.preventDefault();
});
document.addEventListener('keyup', e => {
  if (e.target.tagName === 'INPUT') return;
  keys[e.code] = false;
  if (e.key === ' ' || e.keyCode === 32) keys['Space'] = false;
});
addEventListener('resize', () => {
  camera.aspect = innerWidth / innerHeight;
  camera.updateProjectionMatrix();
  renderer.setSize(innerWidth, innerHeight);
});

function readKeyboard() {
  S.pIn = 0; S.rIn = 0; S.yIn = 0; S.tIn = 0; S.fire = false;
  if (keys['KeyW']) S.pIn = -1;
  if (keys['KeyS']) S.pIn = 1;
  if (keys['KeyA']) S.rIn = 1;
  if (keys['KeyD']) S.rIn = -1;
  if (keys['KeyQ']) S.yIn = 1;
  if (keys['KeyE']) S.yIn = -1;
  if (keys['ShiftLeft'] || keys['ShiftRight']) S.tIn = 1;
  if (keys['ControlLeft'] || keys['ControlRight']) S.tIn = -1;
  if (keys['Space']) S.fire = true;
}

/* ===== Controller input ===== */
const CONTROLLER_STALE_MS = 300;
const controller = {
  at: -Infinity, pitch: 0, roll: 0, yaw: 0, yawHold: 0, joyX: 0, joyY: 0,
  pendingFire: 0, fireSession: null, fireCount: 0,
  ldrValid: false, dark: false, ldrRaw: 0,
  fireHeld: false, throttle: null, throttleApplied: null, tiltValid: false,
};
// Press I during a game to show what the hardware controller is sending.
let showInputs = false;
let lastSentState = null;
// Press Y to compare yaw styles. 'rate': yaw while the controller is turning
// (original). 'hold': keep yawing while it stays twisted away from the front.
let yawMode = 'rate';
try { if (localStorage.getItem('yawMode') === 'hold') yawMode = 'hold'; } catch (e) {}

function toggleYawMode() {
  yawMode = yawMode === 'hold' ? 'rate' : 'hold';
  try { localStorage.setItem('yawMode', yawMode); } catch (e) {}
  addKillMsg('YAW MODE: ' + yawMode.toUpperCase());
}

function clearController() {
  controller.at = -Infinity;
  controller.pendingFire = 0;
  controller.fireHeld = false;
  controller.throttle = null;
  controller.throttleApplied = null;
  controller.tiltValid = false;
  controller.ldrValid = false;
  controller.dark = false;
  S.stealth = false;
  // Keep the last session/count so reconnects do not replay delivered presses.
}

function receiveSensor(d) {
  const axes = ['pitch', 'roll', 'yaw', 'joyX', 'joyY'];
  if (!axes.every(key => typeof d[key] === 'number' && Number.isFinite(d[key]))) return;
  controller.at = performance.now();
  for (const key of axes) controller[key] = clamp(d[key], -1, 1);
  // Optional: firmware without hold-mode yaw leaves it at 0.
  controller.yawHold = Number.isFinite(d.yawHold) ? clamp(d.yawHold, -1, 1) : 0;
  controller.tiltValid = d.tiltValid !== false;
  if (!controller.tiltValid) {
    controller.pitch = controller.roll = controller.yaw = controller.yawHold = 0;
  }
  controller.ldrValid = d.ldrValid === true && Number.isInteger(d.ldrRaw) &&
    d.ldrRaw >= 0 && d.ldrRaw <= 4095 && typeof d.dark === 'boolean';
  controller.dark = controller.ldrValid && d.dark;
  controller.ldrRaw = controller.ldrValid ? d.ldrRaw : 0;
  controller.fireHeld = d.fireHeld === true;
  controller.throttle = Number.isFinite(d.throttle) && d.throttle >= 0 && d.throttle <= 100
    ? d.throttle : null;

  const uint32 = n => Number.isInteger(n) && n >= 0 && n <= 0xffffffff;
  let presses = 0;
  if (uint32(d.fireSession) && uint32(d.fireCount)) {
    if (controller.fireSession !== d.fireSession) {
      controller.fireSession = d.fireSession;
      controller.pendingFire = 0;
      // A first packet only owns presses still pending at the ESP32, not its
      // entire boot history. Subsequent counters recover missed sensor packets.
      presses = uint32(d.firePending) ? d.firePending : (d.fire === true ? 1 : 0);
      controller.fireCount = d.fireCount;
    } else {
      const delta = (d.fireCount - controller.fireCount) >>> 0;
      if (delta < 0x80000000) {
        presses = delta;
        controller.fireCount = d.fireCount;
      }
    }
  } else if (d.fire === true) presses = 1;  // Legacy boolean sender.
  if (S.running) controller.pendingFire += presses;
}

function readInputs() {
  readKeyboard();  // Original keyboard directions and held-space firing.
  if (!ws || ws.readyState !== 1 || performance.now() - controller.at > CONTROLLER_STALE_MS) {
    clearController();
    return;
  }
  S.pIn = controller.pitch;
  S.rIn = controller.roll;
  S.yIn = yawMode === 'hold' ? controller.yawHold : controller.yaw;
  // Joystick X steers yaw too, overriding tilt yaw while pushed (the ESP32
  // applies the joystick deadzone, so a centred stick sends exactly 0).
  if (controller.joyX !== 0) S.yIn = controller.joyX;
  S.stealth = controller.ldrValid && controller.dark;
  // The encoder reports an absolute level (0..100). Apply it only when it changes,
  // so Shift/Ctrl keep working between knob movements.
  if (controller.throttle !== null && controller.throttle !== controller.throttleApplied) {
    S.targetSpeed = C.MIN_SPD + (controller.throttle / 100) * (C.MAX_SPD - C.MIN_SPD);
    controller.throttleApplied = controller.throttle;
  }
  S.fire = S.fire || controller.fireHeld || controller.pendingFire > 0;
}

function sendGameState() {
  if (ws && ws.readyState === 1) {
    const state = {
      type: 'game_state',
      // Hull in 10% steps (0..10) for the MCX servo gauge: one step per 10% lost.
      lives: Math.ceil(clamp(S.health, 0, 100) / 10),
      score: Math.floor(clamp(S.score, 0, 9999999)),
      mode: S.over ? 'OVER' : S.running ? 'PLAY' : 'IDLE',
    };
    lastSentState = state;
    ws.send(JSON.stringify(state));
  }
}

function sendEvent(event, value) {
  if (!['HIT', 'START', 'OVER', 'BONUS'].includes(event)) return;
  if (ws && ws.readyState === 1) {
    sendGameState();
    ws.send(JSON.stringify({ type: 'event', event, value }));
  }
}

// Runs on the start screen and after game over too, independently of rendering.
setInterval(sendGameState, 400);

/* ===== High-score display ===== */
let bestScore = null;
let bestRequestId = null;
const pendingScores = new Map();

function displayBestScore() {
  const text = bestScore === null ? '—' : String(bestScore);
  for (const id of ['start-best-score', 'hud-best-score', 'final-best-score']) {
    document.getElementById(id).textContent = text;
  }
}

function requestBestScore() {
  bestRequestId = createRunId();
  document.getElementById('best-score-status').textContent = 'Loading best score…';
  sendScoreRequests();
}

function sendScoreRequests() {
  if (!ws || ws.readyState !== 1) return;
  if (bestRequestId) {
    ws.send(JSON.stringify({ type: 'get_high_score', requestId: bestRequestId }));
  }
  for (const submission of pendingScores.values()) ws.send(JSON.stringify(submission));
}

function receiveHighScore(d) {
  if (d.type === 'high_score_ready') { requestBestScore(); return; }
  if (d.type !== 'high_score' && d.type !== 'high_score_result') return;
  if (!Number.isInteger(d.highScore) || d.highScore < 0 || d.highScore > 2147483647) return;
  if (d.type === 'high_score') {
    if (d.requestId !== bestRequestId) return;
    bestRequestId = null;
    document.getElementById('best-score-status').textContent = '';
  } else {
    const pending = pendingScores.get(d.runId);
    if (!pending || pending.score !== d.score || typeof d.newRecord !== 'boolean' ||
        d.highScore < d.score || (d.newRecord && d.highScore !== d.score)) return;
    pendingScores.delete(d.runId);
    if (d.runId === runId && S.over) {
      document.getElementById('score-save-status').textContent =
        d.newRecord ? 'New record!' : 'Score checked — best score unchanged.';
    }
  }
  // A delayed response for an older game must not lower the displayed record.
  bestScore = Math.max(bestScore === null ? 0 : bestScore, d.highScore);
  displayBestScore();
}

function queueFinalScore() {
  pendingScores.set(runId, { type: 'submit_score', runId, score: S.score });
  document.getElementById('score-save-status').textContent =
    ws && ws.readyState === 1 ? 'Checking high score…' : 'Connect to check and save your score.';
  if (ws && ws.readyState === 1) ws.send(JSON.stringify(pendingScores.get(runId)));
}

// Handles either startup order and retries when a response is lost.
setInterval(sendScoreRequests, 10000);

/* ===== WebSocket ===== */
function toggleWS() {
  if (ws && ws.readyState <= 1) { ws.close(); return; }
  const url = document.getElementById('ws-url').value.trim();
  if (!url) return;
  try {
    const socket = new WebSocket(url);
    ws = socket;
    clearController();
    socket.onopen = () => {
      if (ws !== socket) return;
      document.getElementById('ws-dot').classList.add('on');
      document.getElementById('ws-btn').textContent = 'Disconnect';
      socket.send(JSON.stringify({ type: 'identify', role: 'game' }));
      sendGameState();   // MCX actuators stay silent until the ESP32 has a game state.
      requestBestScore();
    };
    socket.onclose = () => {
      if (ws !== socket) return;
      clearController();
      document.getElementById('ws-dot').classList.remove('on');
      document.getElementById('ws-btn').textContent = 'Connect';
      document.getElementById('best-score-status').textContent = 'Offline — connect to load best score.';
      if (pendingScores.has(runId) && S.over) {
        document.getElementById('score-save-status').textContent = 'Connection lost — score pending; reconnect to retry.';
      }
    };
    socket.onerror = () => socket.close();
    socket.onmessage = (ev) => {
      if (ws !== socket) return;
      try {
        const d = JSON.parse(ev.data);
        receiveHighScore(d);
        if (d && d.type === 'sensor') receiveSensor(d);
      } catch (_) {}
    };
  } catch (_) {}
}

/* ===== Player physics ===== */
function updatePlayer() {
  S.cP = lerp(S.cP, S.pIn, C.CTRL_LERP);
  S.cR = lerp(S.cR, S.rIn, C.CTRL_LERP);
  S.cY = lerp(S.cY, S.yIn, C.CTRL_LERP);

  // Throttle: Shift/Ctrl adjust; the MCX encoder level is applied in readInputs().
  S.targetSpeed = clamp(S.targetSpeed + S.tIn * 0.015, C.MIN_SPD, C.MAX_SPD);
  S.speed = lerp(S.speed, S.targetSpeed, 0.03);

  _qP.setFromAxisAngle(_axisX, S.cP * C.PITCH_SPD);
  _qR.setFromAxisAngle(_axisZ, S.cR * C.ROLL_SPD);
  _qY.setFromAxisAngle(_axisY, S.cY * C.YAW_SPD);
  _qBY.setFromAxisAngle(_axisY, -S.cR * C.BANK_YAW);

  player.mesh.quaternion.multiply(_qP);
  player.mesh.quaternion.multiply(_qR);
  player.mesh.quaternion.multiply(_qY);
  player.mesh.quaternion.multiply(_qBY);
  player.mesh.quaternion.normalize();

  _fwd.set(0, 0, -1).applyQuaternion(player.mesh.quaternion);
  player.mesh.position.addScaledVector(_fwd, S.speed);

  // Circular wrap — snap camera so it doesn't lerp across the map
  const _preX = player.mesh.position.x, _preZ = player.mesh.position.z;
  wrapPos(player.mesh.position);
  const wdx = player.mesh.position.x - _preX;
  const wdz = player.mesh.position.z - _preZ;
  if (Math.abs(wdx) > 1 || Math.abs(wdz) > 1) {
    camera.position.x += wdx;
    camera.position.z += wdz;
  }

  // Ground collision
  if (player.mesh.position.y < C.GROUND_Y + 5) {
    player.mesh.position.y = C.GROUND_Y + 5;
    takeDamage(5);
  }

  // Ceiling — stall speed + tick damage + icing effect
  if (player.mesh.position.y > C.CEILING) {
    S.speed *= 0.1;
    S.targetSpeed = C.MIN_SPD;
    if (S.dmgCD <= 0) takeDamage(5);
    document.getElementById('ceiling-warn').classList.add('show');
    // Ice over the plane: tint all parts white-blue
    player.mesh.children.forEach(c => {
      if (c.material && !c.material._iced) {
        c.material._origHex = c.material.color.getHex();
        c.material._origOpacity = c.material.opacity;
        c.material._iced = true;
      }
      if (c.material && c.material._iced) {
        c.material.color.lerp(new THREE.Color(0xccddff), 0.08);
        c.material.opacity = Math.max(c.material.opacity, 0.85);
      }
    });
  } else {
    document.getElementById('ceiling-warn').classList.remove('show');
    // Thaw: restore original colors
    player.mesh.children.forEach(c => {
      if (c.material && c.material._iced) {
        c.material.color.lerp(new THREE.Color(c.material._origHex), 0.05);
        const cur = c.material.color.getHex();
        const orig = c.material._origHex;
        // Close enough — snap back
        if (Math.abs(((cur >> 16) & 0xff) - ((orig >> 16) & 0xff)) < 4) {
          c.material.color.setHex(orig);
          c.material.opacity = c.material._origOpacity;
          delete c.material._iced;
        }
      }
    });
  }

  // Propeller
  if (player.mesh.userData.prop) {
    player.mesh.userData.prop.rotation.z += 0.2 + S.speed * 0.15;
  }

  // Firing — three guns: center nose + left/right wing
  S.fireCD = Math.max(0, S.fireCD - 1);
  if (S.fire && S.fireCD === 0) {
    const right = new THREE.Vector3(1, 0, 0).applyQuaternion(player.mesh.quaternion);

    // Center gun (nose)
    const pC = player.mesh.position.clone().addScaledVector(_fwd, 3.5);
    projectiles.push(new Projectile(pC, _fwd.clone()));

    // Left wing gun
    const pL = player.mesh.position.clone()
      .addScaledVector(right, -2.8)
      .addScaledVector(_fwd, 1.0);
    projectiles.push(new Projectile(pL, _fwd.clone()));

    // Right wing gun
    const pR = player.mesh.position.clone()
      .addScaledVector(right, 2.8)
      .addScaledVector(_fwd, 1.0);
    projectiles.push(new Projectile(pR, _fwd.clone()));

    S.fireCD = C.FIRE_CD;
    // One controller button press = one volley.
    if (controller.pendingFire > 0) controller.pendingFire--;
  }
}

/* ===== Camera ===== */
const camTarget = new THREE.Vector3();
const camPos    = new THREE.Vector3();

function updateCamera() {
  // Fixed follow camera — always behind and above the player
  const offset = new THREE.Vector3(0, C.CAM_H, C.CAM_DIST);
  offset.applyQuaternion(player.mesh.quaternion);
  camPos.copy(player.mesh.position).add(offset);
  camera.position.lerp(camPos, C.CAM_LERP);

  // Look ahead of the player
  const look = new THREE.Vector3(0, 0, -25);
  look.applyQuaternion(player.mesh.quaternion);
  camTarget.copy(player.mesh.position).add(look);
  camera.lookAt(camTarget);
}

/* ===== Collision ===== */
function checkCollisions() {
  // Player projectiles vs enemies (use per-enemy hitR)
  for (let pi = projectiles.length - 1; pi >= 0; pi--) {
    const p = projectiles[pi];
    if (!p.alive) continue;
    for (const e of enemies) {
      if (!e.alive) continue;
      if (p.mesh.position.distanceTo(e.mesh.position) < e.hitR) {
        p.alive = false;
        e.hit();
        break;
      }
    }
  }

  // Enemy projectiles vs player
  for (let i = enemyProjectiles.length - 1; i >= 0; i--) {
    const ep = enemyProjectiles[i];
    if (!ep.alive) continue;
    if (ep.mesh.position.distanceTo(player.mesh.position) < C.HIT_R) {
      ep.alive = false;
      takeDamage(C.E_PROJ_DMG);
    }
  }

  // Clean dead projectiles
  for (let i = projectiles.length - 1; i >= 0; i--) {
    if (!projectiles[i].alive) { projectiles[i].remove(); projectiles.splice(i, 1); }
  }
  for (let i = enemyProjectiles.length - 1; i >= 0; i--) {
    if (!enemyProjectiles[i].alive) { enemyProjectiles[i].remove(); enemyProjectiles.splice(i, 1); }
  }
}

function takeDamage(amount) {
  S.health = Math.max(0, S.health - amount);
  S.dmgCD = C.DMG_COOLDOWN;
  document.getElementById('damage-flash').style.opacity = '1';
  setTimeout(() => { document.getElementById('damage-flash').style.opacity = '0'; }, 150);
  sendEvent('HIT', 1);
  if (S.health <= 0) {
    gameOver();
  }
}

/* ===== Wave system ===== */
function spawnWave(waveNum) {
  const count = Math.pow(2, waveNum - 1);   // 1, 2, 4, 8, 16…
  S.waveEnemiesAlive = count;
  const params = getWaveParams(waveNum);

  for (let i = 0; i < count; i++) {
    enemies.push(new Enemy(params));
  }

  const banner = document.getElementById('wave-banner');
  document.getElementById('wave-banner-text').textContent = 'WAVE ' + waveNum;
  const desc = waveNum <= 2 ? 'slow bombers' : waveNum <= 4 ? 'fighters' : 'elite aces';
  document.getElementById('wave-banner-sub').textContent =
    count + (count === 1 ? ' ' : ' ') + desc + ' incoming';
  banner.style.opacity = '1';
  setTimeout(() => { banner.style.opacity = '0'; }, 2000);

  if (waveNum > 1) sendEvent('BONUS', waveNum);
}

function checkWaveComplete() {
  if (S.waveTransition || S.over) return;
  if (S.waveEnemiesAlive <= 0) {
    S.waveTransition = true;
    S.waveDelay = C.WAVE_DELAY;
    S.health = Math.min(100, S.health + 25);
    addKillMsg('WAVE ' + S.wave + ' CLEARED!');
  }
}

function updateWaveTransition() {
  if (!S.waveTransition) return;
  S.waveDelay--;
  if (S.waveDelay <= 0) {
    S.wave++;
    S.waveTransition = false;
    spawnWave(S.wave);
  }
}

/* ===== HUD ===== */
function addKillMsg(text) {
  const el = document.createElement('div');
  el.className = 'kill-msg';
  el.textContent = text;
  document.getElementById('killfeed').appendChild(el);
  setTimeout(() => el.remove(), 2200);
}

function inputDebugText() {
  const f = v => (v >= 0 ? '+' : '') + v.toFixed(2);
  const age = performance.now() - controller.at;
  let link;
  if (!ws || ws.readyState !== 1) link = 'disconnected (click Connect)';
  else if (!Number.isFinite(age) || age > CONTROLLER_STALE_MS) link = 'connected, no sensor data from ESP32';
  else link = 'connected (' + age.toFixed(0) + ' ms ago)';
  const live = ws && ws.readyState === 1 && age <= CONTROLLER_STALE_MS;
  const lines = ['LINK  ' + link];
  if (live) {
    lines.push('TILT  ' + (controller.tiltValid
      ? 'valid  pitch ' + f(controller.pitch) + ' roll ' + f(controller.roll) +
        ' yaw ' + f(controller.yaw) + ' (rate)  hold ' + f(controller.yawHold)
      : 'INVALID (calibrating or MPU fault: hold still / check ESP32 serial)'));
    lines.push('YAW   mode ' + yawMode.toUpperCase() + ' (press Y)');
    lines.push('JOY   x ' + f(controller.joyX) + ' y ' + f(controller.joyY));
    lines.push('FIRE  taps ' + controller.fireCount + '  held ' + (controller.fireHeld ? 'YES' : 'no'));
    lines.push('THR   ' + (controller.throttle !== null
      ? controller.throttle + ' %'
      : 'none: no $ENC from MCX (keyboard Shift/Ctrl)'));
  }
  lines.push('MCX   sent: ' + (lastSentState
    ? lastSentState.mode + '  lives ' + lastSentState.lives + '  score ' + lastSentState.score
    : 'nothing yet'));
  return lines.join('\n');
}

function updateHUD() {
  document.getElementById('score').textContent = S.score;
  document.getElementById('kills').textContent = S.kills + ' KILL' + (S.kills !== 1 ? 'S' : '');
  document.getElementById('wave-label').textContent = 'WAVE ' + S.wave;
  document.getElementById('enemies-left').textContent =
    S.waveEnemiesAlive + (S.waveEnemiesAlive === 1 ? ' enemy' : ' enemies') + ' remaining';
  document.getElementById('health-fill').style.width = S.health + '%';
  const hf = document.getElementById('health-fill');
  hf.style.background = S.health > 50
    ? 'linear-gradient(90deg,#4ade80,#22c55e)'
    : S.health > 25
      ? 'linear-gradient(90deg,#fbbf24,#f59e0b)'
      : 'linear-gradient(90deg,#ef4444,#dc2626)';

  document.getElementById('speed-box').textContent = 'SPD: ' + (S.speed * 100).toFixed(0);
  document.getElementById('alt-box').textContent = 'ALT: ' + (player.mesh.position.y - C.GROUND_Y).toFixed(0);
  document.getElementById('pos-box').textContent = 'POS: ' + Math.round(player.mesh.position.x) + ', ' + Math.round(player.mesh.position.z);

  if (showInputs) document.getElementById('input-debug').textContent = inputDebugText();
}

/* ===== Minimap ===== */
const mmCanvas = document.getElementById('minimap');
const mm = mmCanvas.getContext('2d');
const MM_R = 75;  // canvas is 150×150, radius 75
const MM_SCALE = MM_R / C.WORLD;  // map units → minimap pixels

function updateMinimap() {
  mm.clearRect(0, 0, 150, 150);

  const px = player.mesh.position.x;
  const pz = player.mesh.position.z;

  // Background circle
  mm.save();
  mm.beginPath();
  mm.arc(MM_R, MM_R, MM_R - 1, 0, Math.PI * 2);
  mm.clip();

  // Player heading (yaw angle from quaternion)
  const pFwd = new THREE.Vector3(0, 0, -1).applyQuaternion(player.mesh.quaternion);
  const heading = Math.atan2(pFwd.x, pFwd.z);

  // Rotate map so player always faces up
  mm.translate(MM_R, MM_R);
  mm.rotate(-heading);

  // Map boundary ring (circular world edge)
  mm.strokeStyle = 'rgba(255,255,255,0.15)';
  mm.lineWidth = 1;
  const pDist = Math.sqrt(px * px + pz * pz);
  const borderR = (C.WORLD - pDist) * MM_SCALE;
  if (borderR > 0 && borderR < MM_R + 20) {
    mm.beginPath();
    mm.arc(0, 0, Math.min(borderR, MM_R + 10), 0, Math.PI * 2);
    mm.stroke();
  }

  // Use wrappedTarget to find closest position of each enemy
  const _mmTarget = new THREE.Vector3();
  const _mmObs = new THREE.Vector3(px, 0, pz);

  for (const e of enemies) {
    if (!e.alive) continue;
    wrappedTarget(_mmObs, e.mesh.position, _mmTarget);
    const dx = (_mmTarget.x - px) * MM_SCALE;
    const dz = (_mmTarget.z - pz) * MM_SCALE;
    if (dx * dx + dz * dz < (MM_R - 4) * (MM_R - 4)) {
      mm.fillStyle = '#ff4444';
      mm.beginPath();
      mm.arc(dx, dz, 3, 0, Math.PI * 2);
      mm.fill();
    }
  }

  // Enemy projectiles (small red dots)
  for (const ep of enemyProjectiles) {
    if (!ep.alive) continue;
    wrappedTarget(_mmObs, ep.mesh.position, _mmTarget);
    const dx = (_mmTarget.x - px) * MM_SCALE;
    const dz = (_mmTarget.z - pz) * MM_SCALE;
    if (dx * dx + dz * dz < (MM_R - 4) * (MM_R - 4)) {
      mm.fillStyle = 'rgba(255,68,68,0.5)';
      mm.beginPath();
      mm.arc(dx, dz, 1.5, 0, Math.PI * 2);
      mm.fill();
    }
  }

  mm.restore();

  // Player triangle (always centered, pointing up)
  mm.fillStyle = '#4ade80';
  mm.beginPath();
  mm.moveTo(MM_R, MM_R - 6);
  mm.lineTo(MM_R - 4, MM_R + 4);
  mm.lineTo(MM_R + 4, MM_R + 4);
  mm.closePath();
  mm.fill();

  // Outer ring
  mm.strokeStyle = 'rgba(255,255,255,0.3)';
  mm.lineWidth = 2;
  mm.beginPath();
  mm.arc(MM_R, MM_R, MM_R - 1, 0, Math.PI * 2);
  mm.stroke();
}

/* ===== Game over / restart ===== */
function createRunId() {
  if (globalThis.crypto && typeof globalThis.crypto.randomUUID === 'function') {
    return globalThis.crypto.randomUUID();
  }
  return Date.now().toString(36) + '-' + Math.random().toString(36).slice(2) +
    '-' + Math.random().toString(36).slice(2);
}

let runId = createRunId();

function gameOver() {
  // Several collisions can end the same frame. Submit only once per game.
  if (S.over) return;
  S.over = true;
  S.running = false;
  document.getElementById('final-score').textContent = S.score;
  document.getElementById('final-wave').textContent = S.wave;
  document.getElementById('game-over').style.display = 'flex';
  sendEvent('OVER', S.score);   // MCX game-over feedback
  // Separate from the MCX event: only this message is a completed, scored run.
  queueFinalScore();
}

function restartGame() {
  runId = createRunId();
  document.getElementById('score-save-status').textContent = '';
  S.score = 0; S.kills = 0; S.lives = 3; S.health = 100;
  S.speed = 0.6; S.targetSpeed = 0.6;
  S.pIn = 0; S.rIn = 0; S.yIn = 0; S.tIn = 0;
  S.cP = 0; S.cR = 0; S.cY = 0;
  S.fireCD = 0; S.dmgCD = 0; S.over = false; S.running = true;
  S.fire = false;
  S.wave = 1; S.waveEnemiesAlive = 0; S.waveTransition = false; S.waveDelay = 0;

  projectiles.forEach(p => p.remove());      projectiles.length = 0;
  enemyProjectiles.forEach(p => p.remove()); enemyProjectiles.length = 0;
  particles.forEach(p => scene.remove(p.mesh)); particles.length = 0;
  enemies.forEach(e => e.remove());          enemies.length = 0;

  initPlayer();
  spawnWave(1);
  sendEvent('START', 1);
  document.getElementById('game-over').style.display = 'none';
  document.getElementById('hud').style.display = 'block';
  loop();
}

/* ===== Main loop ===== */
function loop() {
  if (!S.running) return;
  requestAnimationFrame(loop);

  readInputs();
  updatePlayer();
  S.dmgCD = Math.max(0, S.dmgCD - 1);

  enemies.forEach(e => e.update());
  projectiles.forEach(p => p.update());
  enemyProjectiles.forEach(ep => ep.update());
  updateParticles();
  checkCollisions();
  checkWaveComplete();
  updateWaveTransition();
  updateCamera();
  updateHUD();
  updateMinimap();

  renderer.render(scene, camera);
}

/* ===== Initialise ===== */
buildWorld();
initPlayer();

camera.position.set(0, C.START_ALT + 20, 40);
camera.lookAt(0, C.START_ALT, 0);
renderer.render(scene, camera);

document.getElementById('start-screen').addEventListener('click', () => {
  document.getElementById('start-screen').style.display = 'none';
  document.getElementById('hud').style.display = 'block';
  renderer.domElement.focus();
  S.running = true;
  spawnWave(1);
  sendEvent('START', 1);
  loop();
});
