/* ===== config.js — Sky Strike constants, state & helpers ===== */

const C = {
  WORLD: 400,
  GROUND_Y: -60,
  START_ALT: 80,

  // Player weapons
  FIRE_CD: 10,
  PROJ_SPEED: 5.5,
  PROJ_LIFE: 120,
  HIT_R: 5,

  // Player flight
  MIN_SPD: 0.3,
  MAX_SPD: 1.0,
  CEILING: 1200,          // altitude ceiling (just below 1500 render distance)
  CAM_DIST: 14,
  CAM_H: 5,
  CAM_LERP: 0.055,
  CAM_ORBIT_SPD: 1.2,
  CAM_ORBIT_LERP: 0.06,
  MOUSE_SENS: 0.003,      // pointer-lock mouse sensitivity
  MOUSE_V_CLAMP: 1.2,     // max vertical orbit (radians)
  CTRL_LERP: 0.07,
  PITCH_SPD: 0.05,
  ROLL_SPD: 0.1,
  YAW_SPD: 0.05,
  BANK_YAW: 0.012,
  DMG_COOLDOWN: 40,

  // Enemy defaults (wave params override per-enemy)
  E_SPD: 1.0,
  E_TURN: 0.025,
  E_FIRE_CD: 70,
  E_FIRE_ANGLE: 0.35,
  E_PROJ_SPEED: 3.5,
  E_PROJ_LIFE: 100,
  E_PROJ_DMG: 10,
  E_FLYBY_R: 40,
  E_DISENGAGE: 120,
  E_LOOP_DIST: 200,

  // Wave timing
  WAVE_DELAY: 150,
};

/* ===== Mutable game state ===== */
const S = {
  score: 0, kills: 0, health: 100, speed: 0.6, targetSpeed: 0.6,
  pIn: 0, rIn: 0, yIn: 0, tIn: 0, fire: false,
  potThrottle: -1,          // -1 = pot not connected; 0..1 from MCX
  camX: 0, camY: 0,        // raw joystick / mouse camera input
  sCamX: 0, sCamY: 0,      // smoothed camera orbit
  cP: 0, cR: 0, cY: 0,    // smoothed control inputs
  fireCD: 0, dmgCD: 0,
  running: false, over: false,
  wave: 1, waveEnemiesAlive: 0, waveDelay: 0, waveTransition: false,
};

/* ===== Shared arrays ===== */
const keys = {};
let ws = null;
const enemies = [];
const projectiles = [];
const enemyProjectiles = [];
const particles = [];

/* ===== Helpers ===== */
function clamp(v, lo, hi) { return Math.max(lo, Math.min(hi, v)); }
function lerp(a, b, t) { return a + (b - a) * t; }

/* Circular pac-man wrap — teleport through the origin when past radius */
function wrapPos(pos) {
  const r = Math.sqrt(pos.x * pos.x + pos.z * pos.z);
  if (r > C.WORLD) {
    const scale = -(C.WORLD - 2) / r;
    pos.x *= scale;
    pos.z *= scale;
  }
}

/* wrappedTarget: for a circular map, just use direct position.
 * Both objects are wrapped each frame so they're always inside the circle.
 */
function wrappedTarget(observer, target, out) {
  out.copy(target);
}
