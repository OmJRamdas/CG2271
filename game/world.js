/* ===== world.js — Three.js scene, lighting, terrain & airplane model ===== */

/* ===== Renderer ===== */
const scene = new THREE.Scene();
scene.background = new THREE.Color(0x7ec8e3);
scene.fog = new THREE.FogExp2(0x7ec8e3, 0.002);

const camera = new THREE.PerspectiveCamera(65, innerWidth / innerHeight, 0.5, 1500);
const renderer = new THREE.WebGLRenderer({ antialias: true });
renderer.setPixelRatio(Math.min(devicePixelRatio, 2));
renderer.setSize(innerWidth, innerHeight);
renderer.shadowMap.enabled = false;
document.body.insertBefore(renderer.domElement, document.body.firstChild);

/* ===== Lights ===== */
scene.add(new THREE.AmbientLight(0x8899bb, 0.6));
const sun = new THREE.DirectionalLight(0xfff5e0, 1.0);
sun.position.set(200, 300, 100);
scene.add(sun);
scene.add(new THREE.HemisphereLight(0x87ceeb, 0x3a7d3a, 0.3));

/* ===== Airplane model builder ===== */
function makeplane(body, accent) {
  const g = new THREE.Group();
  const bm = new THREE.MeshPhongMaterial({ color: body, flatShading: true });
  const am = new THREE.MeshPhongMaterial({ color: accent, flatShading: true });
  const gl = new THREE.MeshPhongMaterial({ color: 0x88ddff, transparent: true, opacity: 0.5 });

  // Fuselage
  const fg = new THREE.CylinderGeometry(0.45, 0.3, 3.6, 8);
  fg.rotateX(Math.PI / 2);
  g.add(new THREE.Mesh(fg, bm));

  // Nose cone
  const ng = new THREE.ConeGeometry(0.45, 1.1, 8);
  ng.rotateX(-Math.PI / 2);
  const nose = new THREE.Mesh(ng, am);
  nose.position.z = -2.35;
  g.add(nose);

  // Canopy
  const cg = new THREE.SphereGeometry(0.32, 8, 5, 0, Math.PI * 2, 0, Math.PI * 0.55);
  const ck = new THREE.Mesh(cg, gl);
  ck.position.set(0, 0.32, -0.4);
  g.add(ck);

  // Wings
  const wg = new THREE.BoxGeometry(7, 0.1, 1.2);
  const w = new THREE.Mesh(wg, bm);
  w.position.set(0, -0.08, 0.15);
  g.add(w);

  // Wingtips
  const wtg = new THREE.BoxGeometry(0.6, 0.5, 0.3);
  [-3.5, 3.5].forEach(x => {
    const wt = new THREE.Mesh(wtg, am);
    wt.position.set(x, 0.1, 0.15);
    g.add(wt);
  });

  // Tail vertical
  const tvg = new THREE.BoxGeometry(0.1, 1.1, 0.7);
  const tv = new THREE.Mesh(tvg, am);
  tv.position.set(0, 0.55, 1.6);
  g.add(tv);

  // Tail horizontal
  const thg = new THREE.BoxGeometry(2.2, 0.08, 0.5);
  const th = new THREE.Mesh(thg, bm);
  th.position.set(0, 0.05, 1.6);
  g.add(th);

  // Propeller hub
  const phg = new THREE.CylinderGeometry(0.12, 0.12, 0.2, 6);
  phg.rotateX(Math.PI / 2);
  const ph = new THREE.Mesh(phg, am);
  ph.position.z = -2.95;
  g.add(ph);

  // Propeller blade
  const pbg = new THREE.BoxGeometry(0.12, 1.8, 0.05);
  const prop = new THREE.Mesh(pbg, new THREE.MeshPhongMaterial({ color: 0x333333, flatShading: true }));
  prop.position.z = -3.05;
  g.add(prop);
  g.userData.prop = prop;

  return g;
}

/* ===== Environment ===== */
function buildWorld() {
  // Circular terrain
  const tg = new THREE.CircleGeometry(C.WORLD, 80, 0, Math.PI * 2);
  const pos = tg.attributes.position;
  for (let i = 0; i < pos.count; i++) {
    const x = pos.getX(i), y = pos.getY(i);
    pos.setZ(i,
      Math.sin(x * 0.008) * 15 + Math.cos(y * 0.006) * 20 +
      Math.sin(x * 0.02 + y * 0.015) * 5 + (Math.random() - 0.5) * 3
    );
  }
  tg.computeVertexNormals();
  const terrain = new THREE.Mesh(tg, new THREE.MeshPhongMaterial({ color: 0x4a8c3f, flatShading: true }));
  terrain.rotation.x = -Math.PI / 2;
  terrain.position.y = C.GROUND_Y;
  scene.add(terrain);

  // Fog wall cylinder surrounding the map border
  const fogHeight = C.CEILING - C.GROUND_Y;
  const fogGeo = new THREE.CylinderGeometry(C.WORLD * 0.95, C.WORLD * 0.95, fogHeight, 64, 1, true);
  const fogMat = new THREE.MeshBasicMaterial({
    color: 0xc8dde8,
    transparent: true,
    opacity: 0.45,
    side: THREE.DoubleSide,
    depthWrite: false,
    fog: false,
  });
  const fogWall = new THREE.Mesh(fogGeo, fogMat);
  fogWall.position.y = C.GROUND_Y + fogHeight / 2;
  scene.add(fogWall);

  // Second, slightly larger fog layer for depth
  const fogGeo2 = new THREE.CylinderGeometry(C.WORLD * 1.05, C.WORLD * 1.05, fogHeight, 64, 1, true);
  const fogMat2 = new THREE.MeshBasicMaterial({
    color: 0xb0c8d8,
    transparent: true,
    opacity: 0.3,
    side: THREE.DoubleSide,
    depthWrite: false,
    fog: false,
  });
  const fogWall2 = new THREE.Mesh(fogGeo2, fogMat2);
  fogWall2.position.y = C.GROUND_Y + fogHeight / 2;
  scene.add(fogWall2);

  // Clouds
  const cmat = new THREE.MeshPhongMaterial({ color: 0xffffff, transparent: true, opacity: 0.75, flatShading: true });
  for (let i = 0; i < 40; i++) {
    const cg = new THREE.Group();
    const n = 3 + Math.floor(Math.random() * 4);
    for (let j = 0; j < n; j++) {
      const s = 4 + Math.random() * 12;
      const sg = new THREE.SphereGeometry(1, 6, 5);
      const puff = new THREE.Mesh(sg, cmat);
      puff.scale.set(s, s * 0.4, s * 0.8);
      puff.position.set((Math.random() - 0.5) * s * 1.5, (Math.random() - 0.5) * 2, (Math.random() - 0.5) * s);
      cg.add(puff);
    }
    const cAngle = Math.random() * Math.PI * 2;
    const cDist = Math.random() * C.WORLD * 0.75;
    cg.position.set(
      Math.cos(cAngle) * cDist,
      120 + Math.random() * 250,
      Math.sin(cAngle) * cDist
    );
    scene.add(cg);
  }

  // Mountains (ring around the world border)
  const mmg = new THREE.ConeGeometry(1, 1, 4);
  const mmat = new THREE.MeshPhongMaterial({ color: 0x556655, flatShading: true });
  for (let i = 0; i < 20; i++) {
    const m = new THREE.Mesh(mmg, mmat);
    const s = 40 + Math.random() * 80;
    m.scale.set(s, s * 1.2, s);
    const angle = (i / 20) * Math.PI * 2;
    const dist = C.WORLD * 0.8 + Math.random() * (C.WORLD * 0.25);
    m.position.set(Math.cos(angle) * dist, C.GROUND_Y + s * 0.5, Math.sin(angle) * dist);
    scene.add(m);
  }
}
