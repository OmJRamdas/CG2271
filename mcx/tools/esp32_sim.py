"""
ESP32 simulator for standalone MCXC444 testing.

Plays the ESP32's side of the UART protocol (see mcx/source/protocol.h):
sends $GS / $EV frames and checks the MCXC444's $ACK / $NAK / $ENC replies.

Wiring: USB-UART adapter TX -> PTE23, RX -> PTE22, GND -> GND (3.3 V levels!).
(Or flash the ESP32 with a Serial<->Serial1 passthrough sketch.)

Usage (from the repo root):
    .venv\\Scripts\\python mcx\\tools\\esp32_sim.py --list
    .venv\\Scripts\\python mcx\\tools\\esp32_sim.py COM7                # scripted demo
    .venv\\Scripts\\python mcx\\tools\\esp32_sim.py COM7 --corrupt 3    # every 3rd frame bad
    .venv\\Scripts\\python mcx\\tools\\esp32_sim.py COM7 --interactive
    .venv\\Scripts\\python mcx\\tools\\esp32_sim.py --selftest          # no hardware needed
"""

import argparse
import sys
import threading
import time


def checksum(body: str) -> int:
    ck = 0
    for b in body.encode("ascii"):
        ck ^= b
    return ck


def build(body: str) -> str:
    return f"${body}*{checksum(body):02X}\n"


def parse(line: str):
    """Return (type, fields) for a valid frame, else raise ValueError."""
    line = line.rstrip("\r\n")
    if len(line) < 4 or line[0] != "$" or line[-3] != "*":
        raise ValueError("bad framing")
    body, ck = line[1:-3], line[-2:]
    if checksum(body) != int(ck, 16):
        raise ValueError(f"bad checksum (got {ck}, want {checksum(body):02X})")
    fields = body.split(",")
    return fields[0], fields[1:]


def selftest() -> int:
    # Must match mcx/tests/test_protocol.c
    assert build("ACK,12") == "$ACK,12*66\n"
    t, f = parse("$ACK,12*66")
    assert t == "ACK" and f == ["12"]
    try:
        parse("$ACK,12*67")
        raise AssertionError("bad checksum accepted")
    except ValueError:
        pass
    print("selftest OK")
    return 0


class Sim:
    def __init__(self, port: str, baud: int, corrupt_every: int):
        import serial  # imported here so --selftest works without pyserial

        self.ser = serial.serial_for_url(port, baudrate=baud, timeout=0.1)
        self.corrupt_every = corrupt_every
        self.last_gs = (3, 0, "IDLE")
        self.seq = 0
        self.sent = 0
        self.pending = {}            # seq -> (body, expect "ACK"/"NAK")
        self.stats = {"ACK": 0, "NAK": 0, "ENC": 0, "unexpected": 0, "bad": 0}
        self.last_enc = None
        self.lock = threading.Lock()
        self.running = True
        self.reader = threading.Thread(target=self._read_loop, daemon=True)
        self.reader.start()

    # ---- sending ----
    def send(self, body_after_seq: str, kind: str):
        with self.lock:
            seq = self.seq
            self.seq = (self.seq + 1) % 65536
            self.sent += 1
            corrupt = self.corrupt_every > 0 and self.sent % self.corrupt_every == 0
            body = f"{kind},{seq},{body_after_seq}"
            frame = build(body)
            if corrupt:  # flip the checksum's last hex digit
                frame = frame[:-2] + ("0" if frame[-2] != "0" else "1") + "\n"
            self.pending[seq] = (body, "NAK" if corrupt else "ACK")
        tag = " [CORRUPTED]" if corrupt else ""
        print(f">> {frame.strip()}{tag}")
        self.ser.write(frame.encode("ascii"))

    def gs(self, lives: int, score: int, mode: str):
        self.last_gs = (lives, score, mode)
        self.send(f"{lives},{score},{mode}", "GS")

    def ev(self, name: str):
        self.send(name, "EV")

    # ---- receiving ----
    def _read_loop(self):
        buf = b""
        while self.running:
            buf += self.ser.read(64)
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                self._handle(raw.decode("ascii", errors="replace"))

    def _handle(self, line: str):
        try:
            kind, fields = parse(line)
        except ValueError as e:
            self.stats["bad"] += 1
            print(f"<< {line!r}  !! {e}")
            return
        if kind == "ENC":
            self.stats["ENC"] += 1
            _, level = fields
            if level != self.last_enc:  # heartbeats repeat the level; print changes only
                print(f"<< ENC throttle={level:>3}%")
                self.last_enc = level
            return
        if kind in ("ACK", "NAK"):
            seq = int(fields[0])
            with self.lock:
                expected = self.pending.pop(seq, (None, None))[1]
            self.stats[kind] += 1
            ok = "ok" if kind == expected else f"!! expected {expected}"
            if kind != expected:
                self.stats["unexpected"] += 1
            print(f"<< {kind} {seq}  {ok}")
            return
        self.stats["unexpected"] += 1
        print(f"<< unknown frame {line!r}")

    def close(self):
        time.sleep(0.5)  # let final replies arrive
        self.running = False
        self.reader.join(timeout=1)
        self.ser.close()
        with self.lock:
            missing = len(self.pending)
        print("\n--- summary ---")
        print(f"sent {self.sent}, " + ", ".join(f"{k} {v}" for k, v in self.stats.items())
              + f", unanswered {missing}")
        return 0 if missing == 0 and self.stats["unexpected"] == 0 and self.stats["bad"] == 0 else 1


def demo(sim: Sim):
    """Scripted game: start, take hits, bonus, game over, then silence."""
    steps = [
        ("Idle",           lambda: sim.gs(3, 0, "IDLE"), 1.0),
        ("Start",          lambda: sim.ev("START"),      0.3),
        ("Playing, 3 lives", lambda: sim.gs(3, 0, "PLAY"), 1.5),
        ("Hit",            lambda: sim.ev("HIT"),        0.3),
        ("2 lives",        lambda: sim.gs(2, 150, "PLAY"), 1.5),
        ("Bonus",          lambda: sim.ev("BONUS"),      0.3),
        ("Hit",            lambda: sim.ev("HIT"),        0.3),
        ("1 life",         lambda: sim.gs(1, 420, "PLAY"), 1.5),
        ("Hit -> over",    lambda: sim.ev("OVER"),       0.3),
        ("Game over",      lambda: sim.gs(0, 420, "OVER"), 2.0),
    ]
    for label, action, wait in steps:
        print(f"\n# {label}")
        action()
        # keep $GS flowing at 2 Hz during waits, like the real ESP32, so the link stays up
        t_end = time.time() + wait
        while True:
            time.sleep(min(0.5, max(0.0, t_end - time.time())))
            if time.time() >= t_end:
                break
            sim.gs(*sim.last_gs)
    print("\n# Silence for 3 s -> MCX should report LINK lost (LED slow blink)")
    time.sleep(3)


def interactive(sim: Sim):
    print("commands: hit | start | over | bonus | gs <lives> <score> <IDLE|PLAY|OVER> | raw <body> | quit")
    while True:
        try:
            cmd = input("> ").strip().split()
        except EOFError:
            break
        if not cmd:
            continue
        c = cmd[0].lower()
        if c == "quit":
            break
        elif c in ("hit", "start", "over", "bonus"):
            sim.ev(c.upper())
        elif c == "gs" and len(cmd) == 4:
            sim.gs(int(cmd[1]), int(cmd[2]), cmd[3].upper())
        elif c == "raw" and len(cmd) >= 2:
            line = build(" ".join(cmd[1:]))
            print(f">> {line.strip()}  (raw, not tracked)")
            sim.ser.write(line.encode("ascii"))
        else:
            print("?")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port", nargs="?", help="serial port, e.g. COM7")
    ap.add_argument("--baud", type=int, default=9600)
    ap.add_argument("--corrupt", type=int, default=0, metavar="N", help="corrupt every Nth frame (expect NAK)")
    ap.add_argument("--interactive", action="store_true")
    ap.add_argument("--list", action="store_true", help="list serial ports")
    ap.add_argument("--selftest", action="store_true", help="check frame encoding, no hardware")
    args = ap.parse_args()

    if args.selftest:
        return selftest()
    if args.list:
        from serial.tools import list_ports
        for p in list_ports.comports():
            print(f"{p.device:8} {p.description}")
        return 0
    if not args.port:
        ap.error("port is required (use --list to find it)")

    sim = Sim(args.port, args.baud, args.corrupt)
    try:
        interactive(sim) if args.interactive else demo(sim)
    except KeyboardInterrupt:
        pass
    return sim.close()


if __name__ == "__main__":
    sys.exit(main())
