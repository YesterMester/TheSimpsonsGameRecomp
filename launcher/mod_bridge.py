"""Authenticated, bounded local creator requests shared by CLI and editor."""
import json
from pathlib import Path
import time
import uuid

if __package__:
    from . import mod_packages as packages
else:
    import mod_packages as packages


def request(session_file, payload, timeout=10):
    if not 0 < timeout <= 30:
        raise packages.ModError("Creator timeout must be above 0 and at most 30 seconds")
    session_file = Path(session_file).expanduser().resolve()
    if session_file.is_dir():
        session_file /= "session.json"
    session = packages.read_json(session_file)
    if session.get("api") != 1:
        raise packages.ModError("This creator session needs a compatible modkit")
    identity = uuid.uuid4().hex
    folder = session_file.parent
    incoming = folder / "requests" / (identity + ".json")
    outgoing = folder / "responses" / (identity + ".json")
    deadline = time.monotonic() + timeout
    packages.write_json(incoming, {"api": 1, "id": identity,
        "session": session["session"], "token": session["token"],
        "timeout_ms": max(1, int(timeout * 1000)),
        "expires_unix_ms": int((time.time() + timeout) * 1000), "request": payload})
    while time.monotonic() < deadline + 0.25:
        if outgoing.is_file():
            try:
                reply = packages.read_json(outgoing)
                if reply.get("id") != identity or reply.get("session") != session["session"]:
                    raise packages.ModError("The reply belongs to a different creator session")
                if not reply.get("ok"):
                    raise packages.ModError(reply.get("code", "error") + ": " + reply.get("error", "Creator request failed"))
                return reply.get("value")
            finally:
                outgoing.unlink(missing_ok=True)
        if not session_file.is_file():
            raise packages.ModError("The creator session has closed")
        time.sleep(0.02)
    # Removing an unclaimed request avoids executing it on a much later frame.
    # A request already running may still finish; never retry a mutation here.
    incoming.unlink(missing_ok=True)
    raise packages.ModError("Creator request timed out; an action already running may still finish")

