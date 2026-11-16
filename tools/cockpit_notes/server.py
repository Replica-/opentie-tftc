"""Cockpit Notes: a local page for marking up OpenTIE cockpit captures.

Serves http://localhost:8765/. Each craft shows OpenTIE's latest capture
beside TFTC's screenshot from docs/tftc_reference/. What you paint on a
capture is sent with your message to docs/tftc_reference/inbox/ as a
marked-up image and a .json file; Claude's watcher
(tools/cockpit_notes/wait_for_message.py) wakes on it and Claude answers
in the chat. "Capture again" flies the craft's mission headless and
replaces OpenTIE's capture.

Run from the repository root:  python tools/cockpit_notes/server.py
"""

import base64
import json
import os
import subprocess
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
REFERENCE = ROOT / "docs" / "tftc_reference"
NOTES = REFERENCE / "notes.json"
INBOX = REFERENCE / "inbox"
PAGE = Path(__file__).with_name("index.html")
GAME = ROOT / "build" / "Debug" / "OpenTIE.exe"
PORT = int(os.environ.get("COCKPIT_NOTES_PORT", "8765"))
# The game's DLLs: MSYS2's UCRT64 and the project's dependency prefix.
DLL_PATHS = ["C:/msys64/ucrt64/bin", "D:/development/OpenTIE-deps/prefix/bin"]

# Each craft: its file stem in docs/tftc_reference and the mission that flies it.
CRAFT = [
    ("tie_fighter", "TIE Fighter", 1, 1),
    ("tie_interceptor", "TIE Interceptor", 1, 4),
    ("tie_bomber", "TIE Bomber", 3, 1),
    ("tie_avenger", "TIE Advanced", 6, 1),
    ("tie_defender", "TIE Defender", 8, 1),
    ("assault_gunboat", "Assault Gunboat", 1, 5),
    ("missile_boat", "Missile Boat", 11, 1),
]

lock = threading.Lock()
capturing = {}  # craft id -> "running" | error text


def load_notes():
    try:
        return json.loads(NOTES.read_text(encoding="utf-8"))
    except (FileNotFoundError, json.JSONDecodeError):
        return []


def save_notes(notes):
    tmp = NOTES.with_suffix(".tmp")
    tmp.write_text(json.dumps(notes, indent=2), encoding="utf-8")
    tmp.replace(NOTES)


def shots():
    out = []
    for key, title, battle, mission in CRAFT:
        ours = REFERENCE / f"ours_{key}.png"
        tftc = REFERENCE / f"tftc_{key}_cockpit.png"
        out.append({
            "id": key,
            "title": title,
            "mission": f"Battle {battle}, mission {mission}",
            "ours": f"/img/{ours.name}?v={int(ours.stat().st_mtime)}" if ours.exists() else None,
            "taken": time.strftime("%Y-%m-%d %H:%M", time.localtime(ours.stat().st_mtime)) if ours.exists() else None,
            "tftc": f"/img/{tftc.name}" if tftc.exists() else None,
            "capture": capturing.get(key),
        })
    return out


def capture(key, battle, mission):
    """Flies the craft's mission headless, selects a target and saves the view."""
    out = REFERENCE / f"ours_{key}.png"
    env = dict(os.environ)
    env["PATH"] = os.pathsep.join(DLL_PATHS + [env.get("PATH", "")])
    try:
        result = subprocess.run(
            [str(GAME), "--headless", "--skip-to-mission", "REPLICA", str(battle), str(mission),
             "--press", "t", "--capture", str(out), "4"],
            cwd=str(GAME.parent), env=env, capture_output=True, timeout=180)
        capturing[key] = None if result.returncode == 0 and out.exists() else f"Capture failed (exit {result.returncode})"
    except Exception as error:  # noqa: BLE001 - shown to the user as is
        capturing[key] = f"Capture failed: {error}"


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def send(self, code, body=b"", kind="application/json"):
        self.send_response(code)
        self.send_header("Content-Type", kind)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def json(self, value, code=200):
        self.send(code, json.dumps(value).encode("utf-8"))

    def body(self):
        length = int(self.headers.get("Content-Length") or 0)
        return json.loads(self.rfile.read(length) or b"{}")

    def do_GET(self):
        path = self.path.split("?")[0]
        if path in ("/", "/index.html"):
            self.send(200, PAGE.read_bytes(), "text/html; charset=utf-8")
        elif path == "/api/shots":
            self.json(shots())
        elif path == "/api/notes":
            with lock:
                self.json(load_notes())
        elif path == "/api/inbox":
            items = []
            for file in sorted(INBOX.glob("*.json"), reverse=True)[:30]:
                try:
                    items.append(json.loads(file.read_text(encoding="utf-8")))
                except (OSError, json.JSONDecodeError):
                    pass
            self.json(items)
        elif path.startswith("/inbox/"):
            file = INBOX / Path(path[7:]).name
            if file.suffix.lower() == ".jpg" and file.exists():
                self.send(200, file.read_bytes(), "image/jpeg")
            else:
                self.send(404)
        elif path.startswith("/img/"):
            file = REFERENCE / Path(path[5:]).name
            if file.suffix.lower() in (".png", ".jpg") and file.exists():
                self.send(200, file.read_bytes(), "image/png" if file.suffix.lower() == ".png" else "image/jpeg")
            else:
                self.send(404)
        else:
            self.send(404)

    def do_POST(self):
        path = self.path.split("?")[0]
        if path == "/api/send":
            data = self.body()
            stamp = time.strftime("%Y%m%d-%H%M%S")
            message_id = f"{stamp}-{uuid.uuid4().hex[:4]}"
            INBOX.mkdir(parents=True, exist_ok=True)
            image = data.get("image", "")
            if image.startswith("data:image/jpeg;base64,"):
                (INBOX / f"{message_id}.jpg").write_bytes(base64.b64decode(image.split(",", 1)[1]))
            craft = next((c for c in CRAFT if c[0] == data.get("shot")), None)
            message = {
                "id": message_id,
                "shot": data.get("shot"),
                "craft": craft[1] if craft else data.get("shot"),
                "view": data.get("view", "OpenTIE"),
                "text": data.get("text", ""),
                "marks": data.get("marks", 0),
                "image": f"{message_id}.jpg" if image else None,
                "imagePath": str(INBOX / f"{message_id}.jpg") if image else None,
                "status": "sent",
                "sentAt": time.strftime("%Y-%m-%dT%H:%M:%S"),
            }
            (INBOX / f"{message_id}.json").write_text(json.dumps(message, indent=2), encoding="utf-8")
            self.json(message, 201)
        elif path == "/api/notes":
            data = self.body()
            note = {
                "id": uuid.uuid4().hex[:8],
                "shot": data["shot"],
                "x": data["x"], "y": data["y"], "w": data["w"], "h": data["h"],
                "text": data["text"],
                "status": "open",
                "createdAt": time.strftime("%Y-%m-%dT%H:%M:%S"),
            }
            with lock:
                notes = load_notes()
                notes.append(note)
                save_notes(notes)
            self.json(note, 201)
        elif path.startswith("/api/notes/"):
            note_id, data = path.rsplit("/", 1)[1], self.body()
            with lock:
                notes = load_notes()
                for note in notes:
                    if note["id"] == note_id:
                        note.update({k: v for k, v in data.items() if k in ("status", "text")})
                save_notes(notes)
            self.json({"ok": True})
        elif path.startswith("/api/capture/"):
            key = path.rsplit("/", 1)[1]
            craft = next((c for c in CRAFT if c[0] == key), None)
            if not craft:
                self.send(404)
            elif capturing.get(key) == "running":
                self.json({"ok": False, "error": "Already capturing"}, 409)
            else:
                capturing[key] = "running"
                threading.Thread(target=capture, args=(key, craft[2], craft[3]), daemon=True).start()
                self.json({"ok": True}, 202)
        else:
            self.send(404)

    def do_DELETE(self):
        path = self.path.split("?")[0]
        if path.startswith("/api/notes/"):
            note_id = path.rsplit("/", 1)[1]
            with lock:
                save_notes([n for n in load_notes() if n["id"] != note_id])
            self.json({"ok": True})
        else:
            self.send(404)


if __name__ == "__main__":
    REFERENCE.mkdir(parents=True, exist_ok=True)
    print(f"Cockpit Notes on http://localhost:{PORT}/  (notes in {NOTES})", flush=True)
    ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
