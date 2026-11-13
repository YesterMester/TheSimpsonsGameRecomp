"""Cross-app ownership, package isolation and Android consumer staging."""
import importlib.util
import copy
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'launcher'))
import mod_packages
import mod_projects
import modkit_process
from modkit_session import EditorSession


class DesktopEditor(unittest.TestCase):
    def test_failed_scripts_report_only_their_own_bounded_diagnostics(self):
        with tempfile.TemporaryDirectory() as name:
            root=Path(name);(root/'logs').mkdir()
            (root/'logs/simpsons_001.log').write_text('unrelated prefix\n'*20000 +
                '[modkit:other.mod] main.lua:10: private error\n'+
                '[modkit:author.scene] ui.action: main.lua:352: Script exceeded its callback budget\n')
            session=EditorSession(SimpleNamespace(USER_DATA=root,BUILD_DIR=root),'author.scene',0)
            message=session.failure_details(RuntimeError('disabled_mod: disabled'))
            self.assertIn('main.lua:352',message);self.assertNotIn('other.mod',message)
            self.assertEqual(session.failure_details(RuntimeError('not_ready')),'not_ready')

    def placement_session(self, legacy=False, reversed_cache=False):
        props = [{'id':f'template:{i}', 'name':f'Prop {i}', 'category':'Props',
                  'source_stream':'gamehub/gamehub.str', 'source_guid':f'{i:032X}'} for i in range(1,46)]
        cached = list(reversed(props)) if reversed_cache else list(props)
        selected = [0]; scene = {'objects':[]}; calls=[]
        session = EditorSession(SimpleNamespace(USER_DATA='/unused'), 'author.scene', 0)
        def inspect():
            return {'pages':[{'id':'author.scene:scene','rows':[{'id':'template',
                'value':selected[0]+1 if legacy else cached[selected[0]]['id']}]}]}
        def menu(row, value=None):
            calls.append((row,value))
            if row=='refresh':cached[:]=props;selected[0]=min(selected[0],len(cached)-1)
            elif row=='next-page':selected[0]=((selected[0]//40+1)%2)*40
            elif row=='template':
                first = selected[0] // 40 * 40
                choices = [index+1 if legacy else item['id']
                           for index,item in enumerate(cached) if first<=index<first+40]
                if value not in choices:
                    raise mod_packages.ModError('invalid_argument: Unknown selector value')
                selected[0]=value-1 if legacy else next(i for i,item in enumerate(cached) if item['id']==value)
            elif row=='place':scene['objects'].append({'id':'scene-1','template':cached[selected[0]]['id']})
        session.call=lambda method: props if method=='world.catalog' else None
        session.inspect=inspect;session.menu=menu
        session.state=lambda: {'scene':copy.deepcopy(scene)}
        session.snapshot=lambda: {'state':{'scene':copy.deepcopy(scene)}}
        return session,calls,scene

    def test_exact_placement_skips_redundant_catalog_refresh(self):
        session,calls,scene=self.placement_session()
        result=session.place('template:43')
        self.assertEqual(result['selected_tag'],'scene-1')
        self.assertEqual(scene['objects'][0]['template'],'template:43')
        self.assertNotIn('refresh',[row for row,value in calls])

    def test_changed_catalog_refreshes_once_without_selecting_the_wrong_prop(self):
        session,calls,scene=self.placement_session(reversed_cache=True)
        session.place('template:15')
        self.assertEqual(scene['objects'][0]['template'],'template:15')
        self.assertEqual([row for row,value in calls].count('refresh'),1)
        self.assertEqual([row for row,value in calls].count('place'),1)

    def test_legacy_ordinal_projects_keep_the_original_selection_protocol(self):
        session,calls,scene=self.placement_session(legacy=True)
        session.place('template:43')
        self.assertEqual(scene['objects'][0]['template'],'template:43')
        self.assertIn(('template',43),calls)
        self.assertEqual([row for row,value in calls].count('refresh'),1)

    def test_placement_never_retries_an_ambiguous_timeout(self):
        session,calls,scene=self.placement_session()
        original=session.menu
        def timeout(row,value=None):
            if row=='template':
                raise mod_packages.ModError('Creator request timed out; an action already running may still finish')
            return original(row,value)
        session.menu=timeout
        with self.assertRaisesRegex(mod_packages.ModError,'timed out'):
            session.place('template:43')
        self.assertEqual(scene['objects'],[])
        self.assertNotIn('refresh',[row for row,value in calls])

    def test_snapshot_refreshes_native_handles_after_a_world_change(self):
        session = EditorSession(SimpleNamespace(USER_DATA='/unused'), 'author.scene', 0)
        info = {'level_ready': True, 'pages': [], 'mods': []}
        def call(method):
            return {'world.status': {'epoch': 2}, 'world.owned': [],
                    'camera.status': {'free':False, 'photo':False}, 'assets.source_status': {'phase':'idle'},
                    'world.catalog': [{'id': 'template:current-world'}]}[method]
        with mock.patch.object(session, 'inspect', return_value=info), \
             mock.patch.object(session, 'state', return_value={}), \
             mock.patch.object(session, 'call', side_effect=call) as request:
            snapshot = session.snapshot(epoch=1)
            self.assertEqual(snapshot['assets'], [{'id': 'template:current-world'}])
            request.reset_mock()
            self.assertNotIn('assets', session.snapshot(epoch=2))
            self.assertNotIn(mock.call('world.catalog'), request.call_args_list)

    def test_running_test_allows_source_reads_but_rejects_writes(self):
        with tempfile.TemporaryDirectory() as name:
            base = Path(name); folder = base / 'launcher'; folder.mkdir()
            shutil.copy2(ROOT / 'launcher/launcher.py', folder / 'launcher.py')
            spec = importlib.util.spec_from_file_location('editor_readonly_backend', folder / 'launcher.py')
            backend = importlib.util.module_from_spec(spec); spec.loader.exec_module(backend)
            backend.UI_DIR = ROOT / 'launcher/ui'; backend.MODS_DIR = base / 'mods'
            backend.USER_DATA = base / 'userdata'; backend.game_running_pids = lambda: []
            result = backend.change_project({'action':'create','template':'scene-editor',
                'id':'author.scene','name':'Read-only test'})
            self.assertTrue(result['ok'])
            backend.game_running_pids = lambda: [12345]
            opened = backend.change_project({'action':'read','id':'author.scene','path':'main.lua'})
            self.assertTrue(opened['ok'])
            denied = backend.change_project({'action':'write','id':'author.scene','path':'main.lua',
                'revision':opened['revision'],'text':opened['text']+'\n-- Must stay unwritten.\n'})
            self.assertFalse(denied['ok'])
            self.assertEqual((Path(result['project']['folder'])/'main.lua').read_text(), opened['text'])

    @unittest.skipUnless(sys.platform == 'linux', 'Linux executable identity check')
    def test_renamed_native_process_is_still_detected(self):
        script = 'import ctypes,time; ctypes.CDLL(None).prctl(15,b"RenamedGame",0,0,0); print("ready",flush=True); time.sleep(15)'
        process = subprocess.Popen([sys.executable, '-c', script], stdout=subprocess.PIPE, text=True)
        try:
            self.assertEqual(process.stdout.readline().strip(), 'ready')
            self.assertIn(process.pid, modkit_process.linux_game_pids(sys.executable))
        finally:
            process.terminate(); process.wait(); process.stdout.close()

    def test_project_browser_ignores_the_normal_profile(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            mod_projects.create(root, ROOT / 'launcher/ui/modkit-templates.json',
                {'template': 'scene-editor', 'name': 'Scene', 'id': 'author.scene'})
            mod_packages.write_json(mod_packages.profile_path(root), {'version': 1, 'enabled': ['missing.normal-mod']})
            self.assertEqual([p['id'] for p in mod_projects.list_projects(root)], ['author.scene'])
            self.assertEqual(mod_packages.profile(root)['enabled'], ['missing.normal-mod'])

    @unittest.skipIf(sys.platform == 'win32', 'POSIX lock conflict check; Windows uses msvcrt')
    def test_two_applications_cannot_prepare_the_game_together(self):
        with tempfile.TemporaryDirectory() as name:
            code = 'import sys; sys.path.insert(0,sys.argv[1]); from modkit_process import game_launch_lock\nwith game_launch_lock(sys.argv[2]): pass'
            with modkit_process.game_launch_lock(name):
                result = subprocess.run([sys.executable, '-c', code, str(ROOT / 'launcher'), name], capture_output=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(b'BlockingIOError', result.stderr)
            result = subprocess.run([sys.executable, '-c', code, str(ROOT / 'launcher'), name], capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_android_staging_removes_old_authoring_assets(self):
        spec = importlib.util.spec_from_file_location('android_assets_editor_check', ROOT / 'tools/android/prepare_assets.py')
        module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            for filename in ['modkit-templates.json', 'mission-editor.js']:
                (root / filename).write_text('stale authoring data')
            module.stage_launcher(root)
            self.assertTrue((root / 'index.html').is_file())
            self.assertFalse((root / 'modkit-templates.json').exists())
            self.assertFalse((root / 'mission-editor.js').exists())
            html = (root / 'index.html').read_text()
            self.assertNotIn('id="projectcreate"', html)
            self.assertIn('id="modinstallbtn"', html)

    def test_help_and_reference_ship_with_the_creator_templates(self):
        value = mod_projects.catalog(ROOT / 'launcher/ui/modkit-templates.json')
        self.assertIn('EDITOR_WORKFLOW.md', value['guides'])
        self.assertIn('SCENES.md', value['guides'])
        self.assertIn('world.spawn', {op['name'] for op in value['reference']['operations']})
        self.assertLess((ROOT / 'launcher/ui/modkit-templates.json').stat().st_size, 4 * 1024**2)


if __name__ == '__main__':
    unittest.main()
