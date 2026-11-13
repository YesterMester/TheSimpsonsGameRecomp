"""Inspect a running private creator session through named game operations."""
import json
from pathlib import Path
import sys
import time
import uuid

import packages


from mod_bridge import request


def source(args):
    if not 0 < args.timeout <= 30:
        raise packages.ModError("Creator timeout must be above 0 and at most 30 seconds")
    deadline = time.monotonic() + args.timeout

    def call(method, arguments=None):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise packages.ModError("Source wait timed out; inspect with source status or resume with "
                                    "source wait. Accepted work has not been cancelled")
        return request(args.session, {"action": "call", "method": method,
                       "arguments": arguments or {}, "owner": args.owner}, min(remaining, 30))

    action = args.source_command
    released = None
    # Never repeat a mutation: an expired reply can still have started a load.
    if action == "load":
        call("assets.load_source", {"stream": args.stream})
    elif action == "release":
        released = call("assets.release_source")
    elif action == "cancel":
        canceled = call("assets.cancel_source")
        return {**call("assets.source_status"), "canceled": canceled}
    waiting = action == "wait" or (action == "load" and args.wait)
    previous = None
    while True:
        state = call("assets.source_status")
        if released is not None:
            return {**state, "released": released}
        if not waiting or state["ready"]:
            return state
        if state["phase"] == "failed":
            raise packages.ModError("source_failed: " + state["message"])
        if not state["owned"]:
            raise packages.ModError("This mod has no object source claim; use source load first")
        progress = (state["phase"], state["loaded"], state["files"], state["message"])
        if progress != previous:
            print(f"{state['phase']}: {state['loaded']}/{state['files']} native files — "
                  f"{state['message']}", file=sys.stderr, flush=True)
            previous = progress
        time.sleep(min(0.1, max(0, deadline - time.monotonic())))


def execute(args, gamedata, mods):
    action = args.developer_command
    if action == "source":
        print(json.dumps(source(args), indent=2, ensure_ascii=False))
        return
    if action == "status":
        payload = {"action": "inspect", "after": args.after}
    elif action == "call":
        arguments = json.loads(args.arguments)
        if not isinstance(arguments, dict):
            raise packages.ModError("--arguments must be a JSON object")
        payload = {"action": "call", "method": args.method,
                   "arguments": arguments, "owner": args.owner or ""}
    elif action == "state":
        payload = {"action": "state", "owner": args.owner}
    elif action == "menu":
        payload = {"action": "activate", "page": args.page, "row": args.row,
                   "value": json.loads(args.value), "player": args.player}
    elif action == "reload":
        if args.cancel:
            result = request(args.session, {"action": "cancel_reload"}, args.timeout)
        else:
            result = request(args.session, {"action": "reload"}, args.timeout)
            generation = result["generation"]
            deadline = time.monotonic() + args.timeout
            while result["state"] in ("preparing", "ready") and time.monotonic() < deadline:
                time.sleep(0.05)
                status = request(args.session, {"action": "inspect"},
                                 min(2, max(0.01, deadline - time.monotonic())))
                result = status["reload"]
                if result["generation"] != generation:
                    raise packages.ModError("Another reload replaced this request; inspect status")
            if result["state"] == "failed":
                raise packages.ModError(result.get("code", "reload_failed") + ": " + result["error"])
        print(json.dumps(result, indent=2, ensure_ascii=False))
        return
    else:
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            result = request(args.session, {"action": "inspect"}, min(2, max(0.01, deadline - time.monotonic())))
            if result["level_ready"]:
                print(json.dumps(result, indent=2, ensure_ascii=False))
                return
            time.sleep(min(0.2, max(0, deadline - time.monotonic())))
        raise packages.ModError("The level did not become ready before the timeout")
    result = request(args.session, payload, args.timeout)
    print(json.dumps(result, indent=2, ensure_ascii=False))


def register(sub):
    parser = sub.add_parser("dev", help="inspect and drive a running private creator session")
    parser.add_argument("--session", required=True, help="session.json path printed in the game's log")
    parser.add_argument("--timeout", type=float, default=10)
    actions = parser.add_subparsers(dest="developer_command", required=True)
    status = actions.add_parser("status", help="mods, menu rows, timer/storage state and recent events")
    status.add_argument("--after", type=int, default=0, help="only include trace events after this sequence")
    call = actions.add_parser("call", help="invoke a named operation from the API catalog")
    call.add_argument("method")
    call.add_argument("--arguments", default="{}", help="JSON object of named arguments")
    call.add_argument("--owner", help="active mod id; required for mutations and namespaced services")
    state = actions.add_parser("state", help="read one mod's current serialized state")
    state.add_argument("owner")
    menu = actions.add_parser("menu", help="activate a registered native menu row")
    menu.add_argument("page", help="namespaced page id, as shown by status")
    menu.add_argument("row")
    menu.add_argument("--value", default="null", help="JSON toggle/slider/selector value")
    menu.add_argument("--player", type=int, choices=(0, 1), default=0)
    reload = actions.add_parser("reload", help="prepare Lua/native code on a worker and replace it after successful preflight")
    reload.add_argument("--cancel", action="store_true", help="cancel preparation and keep the running code")
    sources = actions.add_parser("source", help="load, inspect and release a mod's native object source")
    source_actions = sources.add_subparsers(dest="source_command", required=True)
    for name, help_text in (("status", "show this mod's claim and native loading progress"),
                            ("load", "request a relative .str path from assets.templates"),
                            ("wait", "wait for this mod's accepted source load"),
                            ("cancel", "cancel this mod's addition while keeping earlier sources"),
                            ("release", "release this mod's claim; other owners keep their source")):
        source_parser = source_actions.add_parser(name, help=help_text)
        source_parser.add_argument("--owner", required=True, help="active mod id")
        if name == "load":
            source_parser.add_argument("stream", help="original relative source path")
            source_parser.add_argument("--wait", action="store_true", help="wait until ready within --timeout")
    actions.add_parser("wait", help="wait until a level is ready")
    parser.set_defaults(func=execute)
