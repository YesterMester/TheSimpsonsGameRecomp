#!/usr/bin/env python3
"""Exercise the consumer launcher with QtWebEngine and a temporary, empty install.

Requires the launcher's PySide6 dependency. No game process or real save is used.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import threading

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--android", action="store_true", help="check the actual Android bridge UI mode")
    args = parser.parse_args()
    output = args.output.resolve(); output.mkdir(parents=True, exist_ok=True)
    os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
    os.environ.setdefault("QTWEBENGINE_CHROMIUM_FLAGS", "--disable-gpu --no-sandbox")
    os.environ["XDG_CACHE_HOME"] = str(output / "browser-cache")
    from PySide6.QtCore import QTimer, QUrl
    from PySide6.QtWebEngineCore import QWebEnginePage, QWebEngineScript
    from PySide6.QtWebEngineWidgets import QWebEngineView
    from PySide6.QtWidgets import QApplication

    sys.path.insert(0, str(ROOT / "launcher"))
    import mod_packages
    report = {"ok": False, "environment": "QtWebEngine, isolated desktop backend", "console_errors": []}
    with tempfile.TemporaryDirectory(prefix="creator-launcher-") as temporary:
        base = Path(temporary); launcher = base / "launcher"; launcher.mkdir()
        shutil.copy2(ROOT / "launcher/launcher.py", launcher / "launcher.py")
        spec = importlib.util.spec_from_file_location("creator_ui_launcher", launcher / "launcher.py")
        backend = importlib.util.module_from_spec(spec); spec.loader.exec_module(backend)
        backend.UI_DIR = ROOT / "launcher/ui"; backend.USER_DATA = base / "userdata"
        backend.GAMEDATA = base / "gamedata"; backend.GAMEDATA.mkdir()
        backend.GAME_BIN = base / "game"; backend.GAME_BIN.touch()
        backend.BUILD_DIR = base
        shutil.copy2(ROOT / "modkit/api.json", base / "simpsons-modkit.json")
        backend.game_running_pids = lambda: []
        opened=[]
        backend.open_modkit=lambda identifier=None: (opened.append(identifier) is None, "Opened Modkit.")
        backend.change_mod=lambda body: {"ok":True,"msg":"Choose a mod ZIP package."}
        server = backend.LauncherServer(("127.0.0.1", 0), backend.Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()

        class Page(QWebEnginePage):
            def javaScriptConsoleMessage(self, level, message, line, source):
                if level == QWebEnginePage.JavaScriptConsoleMessageLevel.ErrorMessageLevel:
                    report["console_errors"].append({"message": message, "line": line})

        application = QApplication(sys.argv[:1])
        view = QWebEngineView(); page = Page(view); view.setPage(page); view.resize(1100, 900); view.show()
        if args.android:
            bridge = QWebEngineScript(); bridge.setInjectionPoint(QWebEngineScript.DocumentCreation)
            bridge.setWorldId(QWebEngineScript.MainWorld)
            bridge.setSourceCode('''window.AndroidLauncher={theme:()=>"dark",request:(method,path,body)=>{
                if(method==="POST" && path==="/api/mods" && JSON.parse(body).action==="install")window.__modImportRequests=(window.__modImportRequests||0)+1;
                const xhr=new XMLHttpRequest();xhr.open(method,path,false);
                xhr.setRequestHeader("X-Token",TOKEN);xhr.setRequestHeader("Content-Type","application/json");
                xhr.send(method==="POST"?body:null);return xhr.responseText;
            }};''')
            page.scripts().insert(bridge)
        test = r"""
        (async () => {
          const until = async predicate => {
            const deadline=Date.now()+12000;
            while(!predicate()) {if(Date.now()>deadline) throw new Error("Launcher did not become ready");await new Promise(r=>setTimeout(r,25));}
          };
          await until(() => typeof renderMods === "function");
          document.querySelector('nav [data-tab="mods"]').click();
          if(document.getElementById("projectcreate") || document.getElementById("projecteditor")) throw new Error("Authoring remained in the game launcher");
          if(!document.getElementById("modinstallbtn")) throw new Error("Mod importing was removed");
          if(IS_ANDROID){
            if(!document.getElementById("openmodkitcard").classList.contains("hidden"))throw new Error("Desktop tools appeared on Android");
            document.getElementById("modinstallbtn").click();
            await until(()=>window.__modImportRequests===1);
          }else{
            document.getElementById("openmodkit").click();
            await until(() => document.getElementById("modstatus").textContent.includes("Opened Modkit"));
          }
          window.__creatorResult={ok:true,consumer_only:true,separate_editor_shortcut:true};
        })().catch(error => window.__creatorResult={ok:false,error:error.stack||String(error)});
        """
        page.loadFinished.connect(lambda ok: page.runJavaScript(test) if ok else application.exit(2))
        stage = [0]

        def poll_result(result):
            if not result or stage[0]:
                return
            stage[0] = 1
            report["ui"] = json.loads(result)
            if not report["ui"].get("ok"):
                application.exit(1); return
            QTimer.singleShot(150, capture_desktop)

        def capture_desktop():
            page.runJavaScript("document.getElementById('openmodkitcard').scrollIntoView({block:'start'});")
            QTimer.singleShot(150, resize_mobile)

        def resize_mobile():
            view.grab().save(str(output / "launcher-desktop.png"))
            view.resize(412, 915)
            QTimer.singleShot(200, scroll_mobile)

        def scroll_mobile():
            page.runJavaScript("document.documentElement.dataset.platform='android'; document.getElementById('modinstallbtn').scrollIntoView({block:'start'});document.getElementById('openmodkitcard').classList.add('hidden');")
            QTimer.singleShot(200, finish_mobile)

        def finish_mobile():
            view.grab().save(str(output / "launcher-narrow.png"))
            page.runJavaScript("JSON.stringify({width:innerWidth,height:innerHeight,scroll:document.documentElement.scrollWidth,source:document.getElementById('modinstallbtn').getBoundingClientRect().toJSON(),controls:document.getElementById('modinstallbtn').getBoundingClientRect().toJSON()})", finish)

        def finish(value):
            report["narrow_layout"] = json.loads(value)
            layout = report["narrow_layout"]
            report["ok"] = layout["scroll"] <= layout["width"] and layout["controls"]["bottom"] <= layout["height"] and not report["console_errors"]
            application.exit(0 if report["ok"] else 1)

        timer = QTimer()
        timer.timeout.connect(lambda: page.runJavaScript("window.__creatorResult ? JSON.stringify(window.__creatorResult) : null", poll_result))
        timer.start(100)
        QTimer.singleShot(25000, lambda: application.exit(3))
        page.load(QUrl("http://127.0.0.1:" + str(server.server_address[1]) + "/"))
        code = application.exec()
        if code == 0:
            assert opened == ([] if args.android else [None])
            report["consumer_only"] = True
            report["mod_import_available"] = True
        server.shutdown(); server.server_close(); thread.join()
        (output / "report.json").write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n")
        print(json.dumps(report, ensure_ascii=False))
        page.deleteLater(); view.close()
        return code


if __name__ == "__main__":
    raise SystemExit(main())
