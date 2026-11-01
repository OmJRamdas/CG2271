const assert = require('node:assert/strict');
const { test } = require('node:test');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { webcrypto } = require('node:crypto');

// Exercise the actual game-over handler without needing WebGL or a browser.
function fixture(connected = true) {
  const packets = [];
  const elements = new Map();
  const context = vm.createContext({
    crypto: webcrypto,
    setInterval() {},
    S: { over: false, running: true, score: 30, wave: 2 },
    ws: { readyState: connected ? 1 : 3, send: s => packets.push(JSON.parse(s)) },
    sendEvent: (event, value) => packets.push({ type: 'event', event, value }),
    document: {
      pointerLockElement: null,
      getElementById(id) {
        if (!elements.has(id)) elements.set(id, { style: {} });
        return elements.get(id);
      },
    },
  });
  const source = fs.readFileSync(path.join(__dirname, '../game.js'), 'utf8');
  const section = source.split('/* ===== Game over / restart ===== */')[1]
    .split('function restartGame()')[0];
  vm.runInContext(source.split('/* ===== High-score display ===== */')[1]
    .split('/* ===== WebSocket ===== */')[0], context);
  vm.runInContext(section, context);
  return { context, packets, elements, run: code => vm.runInContext(code, context) };
}

test('game over submits one final score with an ID despite repeated calls', () => {
  const g = fixture();
  g.run('gameOver(); S.score = 40; gameOver();');
  const scores = g.packets.filter(p => p.type === 'submit_score');
  assert.equal(scores.length, 1);
  assert.equal(scores[0].score, 30);
  assert.match(scores[0].runId, /^[a-zA-Z0-9_-]{1,64}$/);
  assert.equal(g.elements.get('final-score').textContent, 30);
});

test('a new run has a different ID and can submit a zero score', () => {
  const g = fixture();
  g.run('gameOver(); runId = createRunId(); S.over = false; S.score = 0; gameOver();');
  const scores = g.packets.filter(p => p.type === 'submit_score');
  assert.equal(scores.length, 2);
  assert.notEqual(scores[0].runId, scores[1].runId);
  assert.equal(scores[1].score, 0);
});

test('disconnected game does not claim to have submitted a score', () => {
  const g = fixture(false);
  g.run('gameOver()');
  assert.equal(g.packets.filter(p => p.type === 'submit_score').length, 0);
});

test('best score loads only for the matching request and appears in all displays', () => {
  const g = fixture();
  g.run('requestBestScore()');
  const request = g.packets.find(p => p.type === 'get_high_score');
  g.run('receiveHighScore({type:"high_score", requestId:"old", highScore:999})');
  assert.equal(g.run('bestScore'), null);
  g.context.reply = { type: 'high_score', requestId: request.requestId, highScore: 30 };
  g.run('receiveHighScore(reply)');
  for (const id of ['start-best-score', 'hud-best-score', 'final-best-score']) {
    assert.equal(g.elements.get(id).textContent, '30');
  }
});

test('confirmed new record is shown once and stops submission retries', () => {
  const g = fixture();
  g.run('gameOver()');
  const submission = g.packets.find(p => p.type === 'submit_score');
  g.context.reply = { ...submission, type: 'high_score_result', highScore: 30, newRecord: true };
  g.run('receiveHighScore(reply); receiveHighScore(reply); sendScoreRequests()');
  assert.equal(g.elements.get('score-save-status').textContent, 'New record!');
  assert.equal(g.run('pendingScores.size'), 0);
  assert.equal(g.packets.filter(p => p.type === 'submit_score').length, 1);
});

test('non-record response displays best and old run replies cannot celebrate a new run', () => {
  const g = fixture();
  g.run('gameOver()');
  const submission = g.packets.find(p => p.type === 'submit_score');
  g.context.reply = { ...submission, type: 'high_score_result', highScore: 50, newRecord: false };
  g.run('receiveHighScore(reply)');
  assert.equal(g.elements.get('final-best-score').textContent, '50');
  assert.match(g.elements.get('score-save-status').textContent, /unchanged/);
  g.run('runId = createRunId(); S.over = false; S.score = 60; gameOver()');
  g.run('receiveHighScore(reply)');
  assert.equal(g.elements.get('score-save-status').textContent, 'Checking high score…');
});

test('offline submissions are retained and sent after connecting', () => {
  const g = fixture(false);
  g.run('gameOver(); ws.readyState = 1; sendScoreRequests()');
  assert.equal(g.packets.filter(p => p.type === 'submit_score').length, 1);
  assert.equal(g.run('pendingScores.size'), 1);
});

test('malformed and mismatched results do not confirm a save', () => {
  const g = fixture();
  g.run('gameOver()');
  const submission = g.packets.find(p => p.type === 'submit_score');
  g.context.reply = { ...submission, type: 'high_score_result', highScore: 20, newRecord: true };
  g.run('receiveHighScore(reply)');
  assert.equal(g.run('pendingScores.size'), 1);
  assert.equal(g.run('bestScore'), null);
});
