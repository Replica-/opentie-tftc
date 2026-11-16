"""Waits for a message sent from Cockpit Notes, then prints it and exits.

Claude runs this in the background; its exit is what wakes Claude to read
the message. Messages Claude has handled carry "status": "read".

    python tools/cockpit_notes/wait_for_message.py
"""

import json
import time
from pathlib import Path

INBOX = Path(__file__).resolve().parents[2] / "docs" / "tftc_reference" / "inbox"

while True:
    waiting = []
    for file in sorted(INBOX.glob("*.json")):
        try:
            message = json.loads(file.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            continue  # still being written
        if message.get("status") == "sent":
            waiting.append((file, message))
    if waiting:
        for file, message in waiting:
            print(f"MESSAGE {file}")
            print(json.dumps(message, indent=2))
        break
    time.sleep(1)
