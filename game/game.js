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
  scene.add(player.mesh);
}

/* ===== Player projectile ===== */
class Projectile {
  constructor(pos, dir) {
    const g = new THREE.CylinderGeometry(0.06, 0.06, 0.8, 4);
    g.rotateX(Math.PI / 2);
    this.mesh = new THREE.Mesh(g, new THREE.MeshBasicMaterial({ color: 0xffdd44 }));
    this.mesh.position.copy(pos);
    this.dir = dir.clone().normalize();
    this.mesh.lookAt(pos.clone().add(this.dir));
    this.life = 0;
    this.alive = true;
    scene.add(this.mesh);
  }
  update() {
    this.mesh.position.addScaledVector(this.dir, C.PROJ_SPEED);
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

let mouseX = 0, mouseY = 0;
document.addEventListener('mousemove', e => {
  mouseX = (e.clientX / innerWidth) * 2 - 1;
  mouseY = -((e.clientY / innerHeight) * 2 - 1);
});

document.addEventListener('keydown', e => {
  if (e.target.tagName === 'INPUT') return;
  keys[e.code] = true;
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
  S.camX = 0; S.camY = 0;
  if (keys['KeyW']) S.pIn = -1;
  if (keys['KeyS']) S.pIn = 1;
  if (keys['KeyA']) S.rIn = 1;
  if (keys['KeyD']) S.rIn = -1;
  if (keys['KeyQ']) S.yIn = 1;
  if (keys['KeyE']) S.yIn = -1;
  if (keys['ShiftLeft'] || keys['ShiftRight']) S.tIn = 1;
  if (keys['ControlLeft'] || keys['ControlRight']) S.tIn = -1;
  if (keys['Space']) S.fire = true;
  // Camera: mouse/trackpad; arrow keys override
  if (keys['ArrowLeft']) S.camX = -1;
  else if (keys['ArrowRight']) S.camX = 1;
  else S.camX = mouseX;
  if (keys['ArrowUp']) S.camY = 1;
  else if (keys['ArrowDown']) S.camY = -1;
  else S.camY = mouseY;
}

/* ===== WebSocket ===== */
function toggleWS() {
  if (ws && ws.readyState <= 1) { ws.close(); return; }
  const url = document.getElementById('ws-url').value.trim();
  if (!url) return;
  try {
    ws = new WebSocket(url);
    ws.onopen = () => {
      document.getElementById('ws-dot').classList.add('on');
      document.getElementById('ws-btn').textContent = 'Disconnect';
      ws.send(JSON.stringify({ type: 'identify', role: 'game' }));
    };
    ws.onclose = () => {
      document.getElementById('ws-dot').classList.remove('on');
      document.getElementById('ws-btn').textContent = 'Connect';
    };
    ws.onerror = () => ws.close();
    ws.onmessage = (ev) => {
      try {
        const d = JSON.parse(ev.data);
        if (d.type === 'sensor') {
          if (d.pitch !== undefined) S.pIn = clamp(d.pitch, -1, 1);
          if (d.roll  !== undefined) S.rIn = clamp(d.roll, -1, 1);
          if (d.yaw   !== undefined) S.yIn = clamp(d.yaw, -1, 1);
          if (d.joyX  !== undefined) S.camX = clamp(d.joyX, -1, 1);
          if (d.joyY  !== undefined) S.camY = clamp(d.joyY, -1, 1);
          if (d.fire) S.fire = true;
          if (d.pot   !== undefined) S.potThrottle = clamp(d.pot, 0, 1);
        }
      } catch (_) {}
    };
  } catch (_) {}
}

function sendEvent(event, value) {
  if (ws && ws.readyState === 1) {
    ws.send(JSON.stringify({ type: 'event', event, value }));
  }
}

/* ===== Player physics ===== */
function updatePlayer() {
  S.cP = lerp(S.cP, S.pIn, C.CTRL_LERP);
  S.cR = lerp(S.cR, S.rIn, C.CTRL_LERP);
  S.cY = lerp(S.cY, S.yIn, C.CTRL_LERP);

  // Throttle: pot overrides keyboard
  if (S.potThrottle >= 0) {
    S.targetSpeed = C.MIN_SPD + S.potThrottle * (C.MAX_SPD - C.MIN_SPD);
  } else {
    S.targetSpeed = clamp(S.targetSpeed + S.tIn * 0.015, C.MIN_SPD, C.MAX_SPD);
  }
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

  // Ground collision
  if (player.mesh.position.y < C.GROUND_Y + 5) {
    player.mesh.position.y = C.GROUND_Y + 5;
    takeDamage(5);
  }

  // Propeller
  if (player.mesh.userData.prop) {
    player.mesh.userData.prop.rotation.z += 0.2 + S.speed * 0.15;
  }

  // Firing
  S.fireCD = Math.max(0, S.fireCD - 1);
  if (S.fire && S.fireCD === 0) {
    const p = player.mesh.position.clone().addScaledVector(_fwd, 3.5);
    projectiles.push(new Projectile(p, _fwd.clone()));
    S.fireCD = C.FIRE_CD;
  }
}

/* ===== Camera ===== */
const camTarget = new THREE.Vector3();
const camPos    = new THREE.Vector3();

function updateCamera() {
  S.sCamX = lerp(S.sCamX, S.camX, C.CAM_ORBIT_LERP);
  S.sCamY = lerp(S.sCamY, S.camY, C.CAM_ORBIT_LERP);

  const offset = new THREE.Vector3(0, C.CAM_H, C.CAM_DIST);
  offset.applyAxisAngle(_axisY, S.sCamX * C.CAM_ORBIT_SPD);
  offset.applyAxisAngle(_axisX, -S.sCamY * 0.5);
  offset.applyQuaternion(player.mesh.quaternion);
  camPos.copy(player.mesh.position).add(offset);
  camera.position.lerp(camPos, C.CAM_LERP);

  const look = new THREE.Vector3(-S.sCamX * 15, S.sCamY * 8, -25);
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
  if (S.health <= 0) gameOver();
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

  sendEvent('BOOST', 1);
  setTimeout(() => sendEvent('BOOST', 0), 500);
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

  if (ws && ws.readyState === 1) {
    const euler = new THREE.Euler().setFromQuaternion(player.mesh.quaternion, 'YXZ');
    sendEvent('ROLL', Math.round(THREE.MathUtils.radToDeg(euler.z)));
  }
}

/* ===== Game over / restart ===== */
function gameOver() {
  S.over = true;
  S.running = false;
  document.getElementById('final-score').textContent = S.score;
  document.getElementById('final-wave').textContent = S.wave;
  document.getElementById('game-over').style.display = 'flex';
  sendEvent('SCORE', S.score);
}

function restartGame() {
  S.score = 0; S.kills = 0; S.health = 100;
  S.speed = 1.2; S.targetSpeed = 1.2;
  S.pIn = 0; S.rIn = 0; S.yIn = 0; S.tIn = 0;
  S.cP = 0; S.cR = 0; S.cY = 0;
  S.fireCD = 0; S.dmgCD = 0; S.over = false; S.running = true;
  S.fire = false; S.potThrottle = -1;
  S.camX = 0; S.camY = 0; S.sCamX = 0; S.sCamY = 0;
  S.wave = 1; S.waveEnemiesAlive = 0; S.waveTransition = false; S.waveDelay = 0;

  projectiles.forEach(p => p.remove());      projectiles.length = 0;
  enemyProjectiles.forEach(p => p.remove()); enemyProjectiles.length = 0;
  particles.forEach(p => scene.remove(p.mesh)); particles.length = 0;
  enemies.forEach(e => e.remove());          enemies.length = 0;

  initPlayer();
  spawnWave(1);
  document.getElementById('game-over').style.display = 'none';
  document.getElementById('hud').style.display = 'block';
  loop();
}

/* ===== Main loop ===== */
function loop() {
  if (!S.running) return;
  requestAnimationFrame(loop);

  readKeyboard();
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
  loop();
});
