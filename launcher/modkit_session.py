"""Project-scoped live editing through the existing native creator bridge."""
from pathlib import Path
import math
import time
import uuid

if __package__:
    from . import mod_bridge, mod_packages, mod_projects
else:
    import mod_bridge
    import mod_packages
    import mod_projects


class EditorSession:
    def __init__(self, backend, identifier, started_ns):
        self.backend = backend
        self.identifier = identifier
        self.started_ns = started_ns
        test = getattr(backend, 'last_creator_session', None)
        root = (test['user_data'] if test and test['project']['id'] == identifier else
                Path(backend.USER_DATA) / "mod-creator-tests" / identifier)
        self.folder = Path(root) / "modkit-dev"
        self.descriptor = None
        self.preview_entity = None
        self.preview_tag = 'modkit-preview-' + uuid.uuid4().hex[:16]

    def ready(self):
        process = self.backend.game_proc
        return process is not None and process.poll() is None

    def failure_details(self, error):
        message = str(error)
        if not any(word in message.casefold() for word in ('disabled_mod', 'script stopped', 'script_error')):
            return message
        # Keep script errors actionable without reading an entire game log or
        # including other mods' diagnostics or the creator session descriptor.
        build = getattr(self.backend, 'BUILD_DIR', None)
        if build is None: return message
        folder = Path(build) / 'logs'
        try:
            logs = [path for path in folder.glob('simpsons_*.log') if path.is_file() and not path.is_symlink()
                    and path.stat().st_mtime_ns >= self.started_ns]
            if not logs: return message
            path = max(logs, key=lambda value: value.stat().st_mtime_ns)
            with path.open('rb') as stream:
                stream.seek(max(0, path.stat().st_size - 128 * 1024)); text = stream.read(128 * 1024).decode('utf-8', errors='replace')
            lines = [line for line in text.splitlines() if '[modkit:' + self.identifier + ']' in line
                     and any(word in line.casefold() for word in ('error', 'failed', 'exceeded', 'exception'))]
            return message + ('\n' + '\n'.join(lines[-3:]) if lines else '')
        except OSError:
            return message

    def request(self, value):
        if not self.ready():
            raise mod_packages.ModError("Start this project's Test session first")
        if self.descriptor is None or not self.descriptor.is_file():
            candidates = [p for p in self.folder.glob("*/session.json")
                          if not p.is_symlink() and p.stat().st_mtime_ns >= self.started_ns]
            if not candidates:
                raise mod_packages.ModError("The game is starting its creator connection")
            self.descriptor = max(candidates, key=lambda p: p.stat().st_mtime_ns)
        return mod_bridge.request(self.descriptor, value, 4)

    def call(self, method, **arguments):
        return self.request({"action": "call", "owner": self.identifier,
                             "method": method, "arguments": arguments})

    def resolve_library_asset(self, asset):
        if not asset.get('_offline'): return asset['id']
        locations=asset.get('locations',[])
        identity=next((p['resource_id'] for p in locations if p.get('resource_id')), None)
        if not identity: raise mod_packages.ModError('Choose a named native resource from the library')
        result=self.call('assets.search',resource_id=identity,limit=200)
        wanted={(p['stream'].casefold(),p.get('entry'),p.get('resource_id')) for p in locations}
        matches=[item for item in result['items'] if item.get('native_name')==asset.get('native_name') and
                 any((p['stream'].casefold(),p.get('entry'),p.get('resource_id')) in wanted for p in item.get('locations',[]))]
        if len(matches)!=1: raise mod_packages.ModError('This resource changed between the offline library and Test; refresh the library')
        return matches[0]['id']

    def menu(self, row, value=None, page="scene"):
        return self.request({"action": "activate", "page": self.identifier + ":" + page,
                             "row": row, "value": value, "player": 0})

    def state(self):
        return self.request({"action": "state", "owner": self.identifier})

    def inspect(self):
        value = self.request({"action": "inspect"})
        owner = next((m for m in value["mods"] if m["id"] == self.identifier), None)
        if not owner or owner["failed"]:
            raise mod_packages.ModError("The project's script stopped; inspect Output and the game log")
        return value

    def snapshot(self, assets=False, epoch=None):
        info = self.inspect()
        value = {"level_ready": info["level_ready"], "state": self.state(),
                 "pages": [p for p in info["pages"] if p["id"].startswith(self.identifier + ":")],
                 "mods": [m for m in info["mods"] if m["id"] == self.identifier]}
        if 'viewport' in info: value['viewport'] = info['viewport']
        if info["level_ready"]:
            value['camera'] = self.call('camera.status')
            value['source'] = self.call('assets.source_status')
            value["world"] = self.call("world.status")
            value["owned"] = self.call("world.owned")
            if assets or epoch is not None and value['world'].get('epoch') != epoch:
                value["assets"] = self.call("world.catalog")
        return value

    def camera(self, mode):
        if mode not in ('game', 'free', 'paused'):
            raise mod_packages.ModError('Choose Game camera, Free camera or Paused editor')
        if mode == 'game':
            self.call('camera.release')
        else:
            self.call('camera.free', enabled=True)
            self.call('camera.photo', enabled=mode == 'paused')
        return self.snapshot()

    def view(self, mode, tag=None):
        request = {'action': 'viewport', 'mode': mode}
        if mode == 'isolate':
            request['entity'] = self.call('world.tagged', tag=tag)
            if not request['entity']:
                raise mod_packages.ModError('Select a live scene object to isolate')
        self.call('camera.photo', enabled=True)
        self.request(request)
        return self.snapshot()

    def focus(self, tag, distance=12):
        entity = self.call('world.tagged', tag=tag)
        if not entity: raise mod_packages.ModError('Select a live scene object to frame')
        position = self.call('entities.transform', entity=entity)['position']
        self.focus_position(position, distance)
        return self.snapshot()

    def focus_position(self, position, distance=5):
        pose = self.call('camera.pose')
        if not pose: raise mod_packages.ModError('No native camera is available yet')
        pitch, yaw = math.radians(15), math.radians(pose['rotation']['yaw'])
        direction = [math.cos(pitch)*math.sin(yaw), -math.sin(pitch), math.cos(pitch)*math.cos(yaw)]
        pose['rotation'].update(pitch=15, roll=0)
        pose['position']={axis: position[axis] + (1 if axis=='y' else 0) - direction[i]*distance
                          for i,axis in enumerate(('x','y','z'))}
        self.call('camera.photo', enabled=True)
        self.call('camera.set_pose', pose=pose)

    def preview_prop(self, definition, distance=12):
        handle = definition.get('resident_template')
        if not handle: raise mod_packages.ModError('Prepare this original source archive first')
        catalog = self.call('world.catalog')
        if not any(prop['id']==handle and prop['category']=='Props' for prop in catalog):
            raise mod_packages.ModError('This factory is not a qualified prop preview; inspect its source instead')
        pose = self.call('camera.pose')
        if not pose: raise mod_packages.ModError('No native camera is available yet')
        pitch,yaw=math.radians(pose['rotation']['pitch']),math.radians(pose['rotation']['yaw'])
        direction=[math.cos(pitch)*math.sin(yaw),-math.sin(pitch),math.cos(pitch)*math.cos(yaw)]
        position={axis:pose['position'][axis]+direction[i]*distance for i,axis in enumerate(('x','y','z'))}
        self.clear_preview()
        self.call('camera.photo', enabled=True)
        entity=self.call('world.spawn', template=handle, position=position, radius=.25, tag=self.preview_tag)
        self.preview_entity=entity
        if 'viewport' in self.inspect():
            self.request({'action':'viewport','mode':'isolate','entity':entity})
        self.focus_position(position, distance)
        return {'entity':entity, 'snapshot':self.snapshot(assets=True)}

    def clear_preview(self):
        if not self.preview_entity: return
        try:
            self.call('world.destroy', entity=self.preview_entity)
        except mod_packages.ModError as error:
            if not str(error).startswith('stale_handle:'): raise
        self.preview_entity=None

    def place(self, identity):
        # Drive the same public Scene editor callbacks as the game's native
        # menu. Its source retention, stable tags, undo and save document stay
        # authoritative; the desktop editor never creates a parallel spawn.
        props = [a for a in self.call("world.catalog") if a["category"] == "Props"
                 and a.get("source_stream") and a.get("source_guid")
                 and a["source_guid"] != "0" * 32]
        index = next((i for i, a in enumerate(props, 1) if a["id"] == identity), None)
        if index is None:
            raise mod_packages.ModError("This resource is not a supported loaded prop; refresh Assets")
        def template_row():
            page = next((p for p in self.inspect()["pages"] if p["id"] == self.identifier + ":scene"), None)
            if page is None:
                raise mod_packages.ModError("Use a Scene editor or Mission editor project to place props")
            return next(row for row in page["rows"] if row["id"] == "template")
        def select(row, exact):
            current = row["value"]
            if exact:
                current = next((i for i, prop in enumerate(props, 1) if prop['id'] == current), 1)
            count = max(1, (len(props) + 39) // 40)
            for _ in range(((index - 1) // 40 - (max(1, current) - 1) // 40) % count):
                self.menu("next-page")
            try:
                self.menu("template", identity if exact else index)
            except mod_packages.ModError as error:
                # Activate rejects an absent choice before invoking Lua. Only
                # this non-mutating rejection is safe to refresh and retry;
                # a timeout may already have executed the user's callback.
                if exact and str(error) == 'invalid_argument: Unknown selector value':
                    return False
                raise
            return template_row()['value'] == (identity if exact else index)
        row = template_row()
        selected = isinstance(row['value'], str) and select(row, True)
        if not selected:
            # Older projects use ordinal choices. Refresh only that path or
            # a changed catalog, rather than imposing a full Lua/native
            # catalog rebuild on each placement in current projects.
            self.menu("refresh"); row = template_row()
            if not select(row, isinstance(row['value'], str)):
                raise mod_packages.ModError('The template list changed; refresh Assets before placing')
        before = {a["id"] for a in self.state()["scene"]["objects"]}
        self.menu("place")
        result = self.snapshot()
        added = [a["id"] for a in result["state"]["scene"]["objects"] if a["id"] not in before]
        if len(added) != 1:
            page = next(p for p in result["pages"] if p["id"] == self.identifier + ":scene")
            message = next((row["label"] for row in page["rows"] if row["id"] == "status"), "Placement did not finish")
            raise mod_packages.ModError(message)
        result["selected_tag"] = added[0]
        return result

    def edit(self, tag, row, value=None):
        allowed = {"position-x", "position-y", "position-z", "turn-left", "turn-right",
                   "grow", "shrink", "duplicate", "remove"}
        if row not in allowed:
            raise mod_packages.ModError("Choose a supported Scene editor property")
        self.menu("object", tag)
        self.menu(row, value)
        return self.snapshot()

    def action(self, row):
        if row not in {"undo", "redo", "save", "load", "unload", "cancel"}:
            raise mod_packages.ModError("Choose a supported Scene editor action")
        self.menu(row)
        if row == "save":
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline:
                status = self.call("storage.status")
                if status["errors"]:
                    raise mod_packages.ModError("The draft could not be written; inspect the game log")
                if status["pending"] == 0:
                    break
                time.sleep(.05)
            else:
                raise mod_packages.ModError("Saving is still in progress; do not apply drafts yet")
        return self.snapshot()
