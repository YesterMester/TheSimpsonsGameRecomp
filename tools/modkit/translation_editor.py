"""Local translation editor. No external service, account or Python dependencies."""
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import secrets
from urllib.parse import parse_qs, urlsplit
import webbrowser

import packages
import translations


def serve(folder, open_browser=True):
    info, catalogs = translations.load_project(folder)
    allowed = {record["file"] for record, _ in catalogs}
    cache = {}
    for record, data in catalogs:
        path = packages.inside(folder / translations.WORKSPACE, record["file"])
        cache[record["file"]] = (path.stat().st_mtime_ns, path.stat().st_size, data)
    token = secrets.token_urlsafe(32)
    assets = Path(__file__).with_name("translation-editor")

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def send(self, code, content, content_type="application/json; charset=utf-8"):
            body = content if isinstance(content, bytes) else json.dumps(content, ensure_ascii=False).encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.send_header("Content-Security-Policy", "default-src 'self'; style-src 'self'; frame-ancestors 'none'; base-uri 'none'")
            self.end_headers()
            self.wfile.write(body)

        def authorized(self):
            expected = f"127.0.0.1:{self.server.server_port}"
            origin = self.headers.get("Origin")
            return (self.headers.get("Host") == expected and
                    (origin is None or origin == "http://" + expected) and
                    secrets.compare_digest(self.headers.get("X-Modkit-Token", ""), token))

        def catalog_path(self):
            name = parse_qs(urlsplit(self.path).query).get("file", [""])[0]
            if name not in allowed:
                raise packages.ModError("Choose a catalog from this project")
            return packages.inside(folder / translations.WORKSPACE, name)

        def do_GET(self):
            try:
                path = urlsplit(self.path).path
                static = {"/": ("index.html", "text/html; charset=utf-8"),
                          "/editor.js": ("editor.js", "text/javascript; charset=utf-8"),
                          "/editor.css": ("editor.css", "text/css; charset=utf-8")}
                if path in static:
                    name, mime = static[path]
                    self.send(200, (assets / name).read_bytes(), mime)
                    return
                if not self.authorized():
                    self.send(403, {"error": "Open the editor URL printed by modkit"})
                    return
                if path == "/api/project":
                    project = dict(info)
                    project["catalogs"] = []
                    for record, data in catalogs:
                        entries = data["entries"]
                        first = next(iter(entries.values()), {})
                        label = data.get("resource", record["file"])
                        if label.startswith("{"):
                            label = first.get("context", "Dialogue").split("_")[0] + " · " + first.get("source", "")[:55]
                        project["catalogs"].append({**record, "label": label})
                    self.send(200, {"name": packages.read_json(folder / "mod.json")["name"],
                                    "project": project, "progress": translations.check_project(folder)})
                elif path == "/api/search":
                    query = parse_qs(urlsplit(self.path).query)
                    needle = query.get("q", [""])[0].casefold()
                    if not 1 <= len(needle) <= 160:
                        raise packages.ModError("Search needs 1 to 160 characters")
                    matches, total = [], 0
                    for record, _ in catalogs:
                        name = record["file"]
                        file = packages.inside(folder / translations.WORKSPACE, name)
                        stat = file.stat()
                        previous = cache[name]
                        if (stat.st_mtime_ns, stat.st_size) != previous[:2]:
                            cache[name] = (stat.st_mtime_ns, stat.st_size, packages.read_json(file))
                        for key, entry in cache[name][2]["entries"].items():
                            haystack = "\n".join([key, entry["source"], entry.get("translation") or "", entry.get("context", "")])
                            if needle not in haystack.casefold():
                                continue
                            total += 1
                            if len(matches) < 100:
                                matches.append({"file": name, "key": key, "source": entry["source"], "context": entry.get("context", "")})
                    self.send(200, {"total": total, "matches": matches})
                elif path == "/api/catalog":
                    source = self.catalog_path().read_bytes()
                    self.send(200, {"catalog": json.loads(source), "revision": hashlib.sha256(source).hexdigest()})
                else:
                    self.send(404, {"error": "Unknown editor request"})
            except (OSError, ValueError, KeyError) as error:
                self.send(400, {"error": str(error)})

        def do_POST(self):
            try:
                if not self.authorized():
                    self.send(403, {"error": "Editor authorization failed"})
                    return
                if urlsplit(self.path).path != "/api/catalog":
                    self.send(404, {"error": "Unknown editor request"})
                    return
                size = int(self.headers.get("Content-Length", "0"))
                if not 0 < size <= 4 * 1024 * 1024:
                    raise packages.ModError("The catalog update is too large")
                request = json.loads(self.rfile.read(size))
                path = self.catalog_path()
                try:
                    result = translations.save_catalog(folder, path.relative_to(folder / translations.WORKSPACE).as_posix(),
                                                       request["translations"], request.get("revision"))
                except translations.CatalogConflict as error:
                    self.send(409, {"error": str(error)})
                    return
                self.send(200, result)
            except (OSError, ValueError, KeyError, TypeError) as error:
                self.send(400, {"error": str(error)})

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    # Serialize catalog writes and revision checks; no two editor tabs may
    # overwrite each other after checking the same revision.
    server.block_on_close = True
    import threading
    lock = threading.Lock()
    original_post = Handler.do_POST

    def serialized_post(self):
        with lock:
            original_post(self)

    Handler.do_POST = serialized_post
    url = f"http://127.0.0.1:{server.server_port}/#{token}"
    print("Translation editor:", url, flush=True)
    print("Keep this terminal open. Press Ctrl+C to stop the editor.", flush=True)
    if open_browser:
        webbrowser.open(url)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
