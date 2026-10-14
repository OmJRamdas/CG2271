/* CG2271 WebSocket relay server.
 * Deploy to Glitch / Render / Railway (free tier).
 *
 * Two clients connect:
 *   - ESP32  (identifies as  { type: "identify", role: "esp32" })
 *   - Browser game           { type: "identify", role: "game"  }
 *
 * Everything the ESP32 sends is forwarded to the game and vice-versa.
 *
 * Run:  npm install && node server.js
 * Env:  PORT (optional, default 3000)
 */

const { WebSocketServer } = require('ws');
const http = require('http');

const PORT = process.env.PORT || 3000;

const server = http.createServer((req, res) => {
  res.writeHead(200, { 'Content-Type': 'text/plain' });
  res.end('CG2271 relay running');
});

const wss = new WebSocketServer({ server });

let esp32 = null;
let game = null;

wss.on('connection', (ws) => {
  console.log('Client connected');

  ws.on('message', (raw) => {
    let msg;
    try { msg = JSON.parse(raw); } catch { return; }

    // Identify the client role
    if (msg.type === 'identify') {
      if (msg.role === 'esp32') { esp32 = ws; console.log('ESP32 registered'); }
      if (msg.role === 'game')  { game = ws;  console.log('Game registered'); }
      return;
    }

    // Relay: ESP32 → game, game → ESP32
    if (ws === esp32 && game && game.readyState === 1) {
      game.send(raw.toString());
    }
    if (ws === game && esp32 && esp32.readyState === 1) {
      esp32.send(raw.toString());
    }
  });

  ws.on('close', () => {
    if (ws === esp32) { esp32 = null; console.log('ESP32 disconnected'); }
    if (ws === game)  { game = null;  console.log('Game disconnected'); }
  });
});

server.listen(PORT, () => {
  console.log(`Relay listening on ws://localhost:${PORT}`);
});
