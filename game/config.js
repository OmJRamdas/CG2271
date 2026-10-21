/* ===== config.js — Sky Strike constants, state & helpers ===== */

const C = {
  WORLD: 800,
  GROUND_Y: -60,
  START_ALT: 80,

  // Player weapons
  FIRE_CD: 10,
  PROJ_SPEED: 5.5,
  PROJ_LIFE: 120,
  HIT_R: 5,

  // Player flight
  MIN_SPD: 0.3,
  MAX_SPD: 2.8,
  CAM_DIST: 14,
  CAM_H: 5,
  CAM_LERP: 0.055,
  CAM_ORBIT_SPD: 1.2,
  CAM_ORBIT_LERP: 0.06,
  CTRL_LERP: 0.07,
  PITCH_SPD: 0.028,
  ROLL_SPD: 0.032,
  YAW_SPD: 0.018,
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
  score: 0, kills: 0, health: 100, speed: 1.2, targetSpeed: 1.2,
  pIn: 0, rIn: 0, yIn: 0, tIn: 0, fire: false,
  stealth: false,          // Fresh MCX LDR reports darkness
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
