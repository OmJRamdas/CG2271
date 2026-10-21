/* ===== enemies.js — Enemy AI (strafing runs) & wave scaling ===== */

/* ===== Temp vectors (enemy-only) ===== */
const _toPlayer = new THREE.Vector3();
const _eFwd    = new THREE.Vector3();
const _eUp     = new THREE.Vector3(0, 1, 0);
const _lookMat = new THREE.Matrix4();
const _targetQ = new THREE.Quaternion();
const _awayDir = new THREE.Vector3();

/* AI states: APPROACH → FLYBY → DISENGAGE → LOOP → APPROACH … */
const ST = { APPROACH: 0, FLYBY: 1, DISENGAGE: 2, LOOP: 3 };

/* ===== Wave-based enemy scaling =====
 * Early waves:  big, slow bombers  (easy targets)
 * Mid waves:    standard fighters
 * Late waves:   small, fast aces   (deadly)
 */
function getWaveParams(waveNum) {
  if (waveNum <= 2) {
    return {
      scale: 1.5,           // 1.5× bigger model
      speed: 0.5,           // half speed
      hp: 2,
      fireCD: 120,          // slow fire rate
      turnRate: 0.012,      // sluggish turning
      projSpeed: 2.5,       // slow bullets
      hitR: C.HIT_R * 1.5,  // bigger hitbox (easier to hit)
    };
  }
  if (waveNum <= 4) {
    const hpBonus = Math.min(waveNum - 1, 3);
    return {
      scale: 1.0,
      speed: 1.0,
      hp: 2 + hpBonus,
      fireCD: 70,
      turnRate: 0.025,
      projSpeed: 3.5,
      hitR: C.HIT_R,
    };
  }
  // Wave 5+
  const hpBonus = Math.min(waveNum - 1, 5);
  return {
    scale: 0.7,                                    // 0.7× smaller
    speed: Math.min(1.3 + (waveNum - 5) * 0.1, 2.0),
    hp: 3 + hpBonus,
    fireCD: 45,
    turnRate: 0.035,
    projSpeed: 4.5,
    hitR: C.HIT_R * 0.7,
  };
}

/* ===== Enemy class ===== */
class Enemy {
  constructor(params) {
    this.mesh = makeplane(0xcc2222, 0x661111);
    const s = params.scale;
    this.mesh.scale.set(s, s, s);
    this.alive = true;
    this.hp       = params.hp;
    this.speed    = params.speed;
    this.turnRate = params.turnRate;
    this.projSpd  = params.projSpeed;
    this.hitR     = params.hitR;
    this.maxFireCD = params.fireCD;
    this.fireCD   = Math.floor(Math.random() * this.maxFireCD);
    this.state    = ST.APPROACH;
    this.timer    = 0;
    this.spawn();
    scene.add(this.mesh);
  }

  spawn() {
    const angle = Math.random() * Math.PI * 2;
    const elev  = (Math.random() - 0.5) * 1.2;
    const dist  = 180 + Math.random() * 120;
    const px = player.mesh ? player.mesh.position.x : 0;
    const py = player.mesh ? player.mesh.position.y : C.START_ALT;
    const pz = player.mesh ? player.mesh.position.z : 0;
    this.mesh.position.set(
      px + Math.cos(angle) * dist,
      Math.max(C.GROUND_Y + 15, py + elev * 80),
      pz + Math.sin(angle) * dist
    );
    // Face toward player
    _toPlayer.copy(player.mesh ? player.mesh.position : new THREE.Vector3(0, C.START_ALT, 0))
      .sub(this.mesh.position).normalize();
    this.mesh.lookAt(this.mesh.position.clone().add(_toPlayer));
    this.state = ST.APPROACH;
    this.timer = 0;
  }

  update() {
    if (!this.alive) return;

    _toPlayer.copy(player.mesh.position).sub(this.mesh.position);
    const distToPlayer = _toPlayer.length();
    _toPlayer.normalize();

    _eFwd.set(0, 0, -1).applyQuaternion(this.mesh.quaternion);

    switch (this.state) {

      case ST.APPROACH:
        // Slerp toward player
        _lookMat.lookAt(this.mesh.position, player.mesh.position, _eUp);
        _targetQ.setFromRotationMatrix(_lookMat);
        this.mesh.quaternion.slerp(_targetQ, this.turnRate);

        // Fire when roughly facing and in range
        this.fireCD--;
        if (this.fireCD <= 0 && distToPlayer < 200) {
          const dot = _eFwd.dot(_toPlayer);
          if (dot > Math.cos(C.E_FIRE_ANGLE)) {
            const bulletPos = this.mesh.position.clone().addScaledVector(_eFwd, 3.5);
            enemyProjectiles.push(new EnemyProjectile(bulletPos, _eFwd.clone(), this.projSpd));
            this.fireCD = this.maxFireCD + Math.floor(Math.random() * 20);
          }
        }

        // Close enough → fly past
        if (distToPlayer < C.E_FLYBY_R) {
          this.state = ST.FLYBY;
          this.timer = 0;
        }
        break;

      case ST.FLYBY:
        // Straight-line zoom past; no turning
        this.timer++;
        if (this.timer > 30) {
          this.state = ST.DISENGAGE;
          this.timer = 0;
        }
        break;

      case ST.DISENGAGE:
        // Climb and veer away
        _awayDir.copy(this.mesh.position).sub(player.mesh.position).normalize();
        _awayDir.y += 0.3;
        _awayDir.normalize();
        const awayTarget = this.mesh.position.clone().add(_awayDir.multiplyScalar(50));
        _lookMat.lookAt(this.mesh.position, awayTarget, _eUp);
        _targetQ.setFromRotationMatrix(_lookMat);
        this.mesh.quaternion.slerp(_targetQ, 0.02);

        this.timer++;
        if (distToPlayer > C.E_LOOP_DIST || this.timer > C.E_DISENGAGE) {
          this.state = ST.LOOP;
          this.timer = 0;
        }
        break;

      case ST.LOOP:
        // Turn back for next run
        _lookMat.lookAt(this.mesh.position, player.mesh.position, _eUp);
        _targetQ.setFromRotationMatrix(_lookMat);
        this.mesh.quaternion.slerp(_targetQ, 0.015);

        this.timer++;
        const facingDot = _eFwd.dot(_toPlayer);
        if (facingDot > 0.7 || this.timer > 150) {
          this.state = ST.APPROACH;
          this.fireCD = Math.floor(Math.random() * 30);
        }
        break;
    }

    this.mesh.quaternion.normalize();

    // Move forward at this enemy's speed
    _eFwd.set(0, 0, -1).applyQuaternion(this.mesh.quaternion);
    this.mesh.position.addScaledVector(_eFwd, this.speed);

    // Altitude floor
    if (this.mesh.position.y < C.GROUND_Y + 10) {
      this.mesh.position.y = C.GROUND_Y + 10;
    }

    // Propeller spin
    if (this.mesh.userData.prop) this.mesh.userData.prop.rotation.z += 0.3;

    // Visual bank
    const right = new THREE.Vector3(1, 0, 0).applyQuaternion(this.mesh.quaternion);
    this.mesh.rotateZ(-right.dot(_toPlayer) * 0.015);
  }

  hit() {
    this.hp--;
    // Flash white
    this.mesh.children.forEach(c => {
      if (c.material && !c.material._origColor) {
        c.material._origColor = c.material.color.getHex();
      }
      if (c.material) c.material.color.setHex(0xffffff);
    });
    setTimeout(() => {
      this.mesh.children.forEach(c => {
        if (c.material && c.material._origColor !== undefined) {
          c.material.color.setHex(c.material._origColor);
        }
      });
    }, 80);
    if (this.hp <= 0) this.destroy();
  }

  destroy() {
    this.alive = false;
    this.mesh.visible = false;
    spawnExplosion(this.mesh.position);
    S.score += 10;
    S.kills++;
    S.waveEnemiesAlive--;
    addKillMsg('+10 ENEMY DOWN');
    sendGameState();
  }

  remove() { scene.remove(this.mesh); }
}

/* ===== Enemy projectile ===== */
class EnemyProjectile {
  constructor(pos, dir, speed) {
    const g = new THREE.CylinderGeometry(0.08, 0.08, 0.9, 4);
    g.rotateX(Math.PI / 2);
    this.mesh = new THREE.Mesh(g, new THREE.MeshBasicMaterial({ color: 0xff4444 }));
    this.mesh.position.copy(pos);
    this.dir = dir.clone().normalize();
    this.speed = speed || C.E_PROJ_SPEED;
    this.mesh.lookAt(pos.clone().add(this.dir));
    this.life = 0;
    this.alive = true;
    scene.add(this.mesh);
  }
  update() {
    this.mesh.position.addScaledVector(this.dir, this.speed);
    this.life++;
    if (this.life > C.E_PROJ_LIFE) this.alive = false;
  }
  remove() { scene.remove(this.mesh); }
}
