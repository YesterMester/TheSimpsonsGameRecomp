"""A scene created in game becomes an editable, credited, portable package."""
from pathlib import Path
import copy
import json
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'launcher'))
import mod_packages as packages
import mod_projects as projects
import mod_scenes as scenes


def document():
    result = scenes.empty()
    result.update(level_root='gamehub/gamehub.str', autoload=True)
    result['objects'] = [{'id': 'scene-1', 'name': 'Original prop', 'radius': 1,
        'source': {'guid': '0123456789abcdef0123456789abcdef', 'class': 'ScriptableProp', 'stream': 'brt/brt/zone01.str'},
        'transform': {'position': {'x': 1.25, 'y': 2.5, 'z': -3}, 'right': {'x': 1.5, 'y': 0, 'z': 0},
            'up': {'x': 0, 'y': .75, 'z': 0}, 'forward': {'x': 0, 'y': 0, 'z': 2}}}]
    return result


class SceneProjects(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(); self.addCleanup(temp.cleanup)
        self.base = Path(temp.name); self.mods = self.base / 'mods'; self.user = self.base / 'userdata'
        self.project = projects.create(self.mods, projects.DEFAULT_BUNDLE, {'template': 'scene-editor',
            'id': 'creator.scene', 'name': 'My scene', 'author': 'Scene Author'})
        self.folder = Path(self.project['folder'])
        self.draft_path = self.user / 'mod-creator-tests/creator.scene/mod-storage/creator.scene/scene-draft.json'
        self.saved = scenes.validate(document())
        packages.write_json(self.draft_path, {'schema': 1,
            'fingerprint': packages.manifest(self.folder)['fingerprint'], 'scene': self.saved})

    def apply(self):
        return projects.dispatch(self.mods, projects.DEFAULT_BUNDLE,
            {'action': 'apply-scene', 'id': 'creator.scene'}, self.user)

    def test_saved_draft_applies_to_project_and_exports_with_credit_and_full_pose(self):
        self.assertEqual(self.apply()['objects'], 1)
        manifest = packages.manifest(self.folder)
        self.assertEqual(manifest['scene'], self.saved)
        self.assertEqual(manifest['author'], 'Scene Author')
        self.assertEqual(packages.profile(self.mods)['enabled'], [])
        result = projects.dispatch(self.mods, projects.DEFAULT_BUNDLE, {'action': 'export', 'id': 'creator.scene'})
        imported = packages.install(Path(result['path']), self.base / 'imported')
        self.assertEqual(imported['scene'], self.saved)
        self.assertEqual(imported['author'], 'Scene Author')
        self.assertEqual(packages.profile(self.base / 'imported')['enabled'], [])
        self.assertFalse((Path(imported['folder']) / '.modkit').exists())
        self.assertEqual(self.apply()['msg'], 'This scene draft is already applied')

    def test_stale_source_draft_preserves_all_external_edits(self):
        source = self.folder / 'main.lua'; source.write_text(source.read_text() + '\n-- New source.\n')
        before = (self.folder / 'mod.json').read_bytes()
        with self.assertRaisesRegex(packages.ModError, 'sources changed'):
            self.apply()
        self.assertEqual((self.folder / 'mod.json').read_bytes(), before)
        self.assertTrue(source.read_text().endswith('-- New source.\n'))

    def test_draft_application_follows_the_latest_private_level_preview(self):
        base = self.user/'mod-creator-tests/creator.scene'
        relative = 'viewport/' + 'a'*32
        preview = base/relative/'mod-storage/creator.scene/scene-draft.json'
        draft = packages.read_json(self.draft_path)
        draft['scene']['objects'][0]['name'] = 'Latest preview object'
        packages.write_json(preview, draft)
        packages.write_json(base/'last-test.json', {'schema':1,'relative':relative})
        self.apply()
        self.assertEqual(packages.manifest(self.folder)['scene']['objects'][0]['name'],'Latest preview object')

    def test_invalid_last_preview_cannot_redirect_draft_reads(self):
        marker = self.user/'mod-creator-tests/creator.scene/last-test.json'
        packages.write_json(marker, {'schema':1,'relative':'../../unrelated'})
        before = (self.folder/'mod.json').read_bytes()
        with self.assertRaisesRegex(packages.ModError,'session path is invalid'): self.apply()
        self.assertEqual((self.folder/'mod.json').read_bytes(),before)

    def test_invalid_draft_never_modifies_project(self):
        old = (self.folder / 'mod.json').read_bytes()
        draft = packages.read_json(self.draft_path)
        for change in ('singular', 'address', 'handle', 'duplicate', 'overflow'):
            invalid = copy.deepcopy(draft)
            item = invalid['scene']['objects'][0]
            if change == 'singular': item['transform']['right'] = item['transform']['up']
            elif change == 'address': item['source']['address'] = 0x82000000
            elif change == 'handle': item['source']['guid'] = 'template:1'
            elif change == 'duplicate': invalid['scene']['objects'].append(copy.deepcopy(item))
            else: invalid['scene']['schema'] = 4294967297
            packages.write_json(self.draft_path, invalid)
            with self.assertRaises(packages.ModError): self.apply()
            self.assertEqual((self.folder / 'mod.json').read_bytes(), old)

    def test_draft_links_cannot_escape_the_test_storage(self):
        original = self.base / 'external.json'; original.write_bytes(self.draft_path.read_bytes())
        self.draft_path.unlink(); self.draft_path.symlink_to(original)
        before = (self.folder / 'mod.json').read_bytes()
        with self.assertRaisesRegex(packages.ModError, 'symbolic'):
            self.apply()
        self.assertEqual((self.folder / 'mod.json').read_bytes(), before)

    def test_scene_validation_is_canonical_and_rejects_native_float_underflow(self):
        self.assertEqual(scenes.validate(self.saved), self.saved)
        invalid = copy.deepcopy(self.saved); invalid['objects'][0]['radius'] = 1e-100
        with self.assertRaises(ValueError): scenes.validate(invalid)
        for version in (True, 1.0, 4294967297, -4294967295):
            invalid = copy.deepcopy(self.saved); invalid['schema'] = version
            with self.assertRaises(ValueError): scenes.validate(invalid)
        bad = json.loads((self.folder / 'mod.json').read_text()); bad['scene'] = invalid
        packages.write_json(self.folder / 'mod.json', bad)
        with self.assertRaises(packages.ModError): packages.manifest(self.folder)


if __name__ == '__main__': unittest.main()
