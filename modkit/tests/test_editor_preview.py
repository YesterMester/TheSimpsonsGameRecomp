"""Private level selection and native preview ownership contracts."""
import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'launcher'))
import modkit_levels as levels
import mod_packages
import mod_projects
from modkit_session import EditorSession

FLOW = '''game = NewGame()
episode = NewEpisode( game, "FIRST" )
    episode:SetDefault()
    mode = NewMode( episode, "MODE_STANDARD" )
        mode:AddEntry( NewMovie( "intro" ) )
        map = NewMap( "First map", "first", "first.str", nil )
            map:SetPlayers( "Homer", "Bart" )
            mode:AddEntry( map )
        mode:AddEntry( NewMap( "Second map", "second", "second.str", "{00000000-0000-0000-0000-000000000001}" ) )
    mode = NewMode( episode, "MODE_TIME_CHALLENGE" )
        mode:AddEntry( NewMap( "Timed", "first", "first.str", nil ) )
episode = NewEpisode( game, "SECOND" )
    mode = NewMode( episode, "MODE_STANDARD" )
        mode:AddEntry( NewMap( "Third map", "third", "third.str", nil ) )
gameFlowManager:SetGame( game )
'''


class EditorPreview(unittest.TestCase):
    def test_map_selection_preserves_original_entry_and_other_episodes(self):
        entries = levels.catalog(FLOW)
        self.assertEqual(len(entries), 4)
        text = levels.select_entry(FLOW, entries[0])
        chosen = [e for e in levels.catalog(text) if e['episode'] == 'FIRST']
        self.assertEqual([e['name'] for e in chosen], ['First map'])
        self.assertIn('map:SetPlayers( "Homer", "Bart" )', text)
        self.assertIn(FLOW[FLOW.index('episode = NewEpisode( game, "SECOND"'):], text)
        self.assertNotIn('NewMovie', text)
        self.assertEqual(len(levels.DEFAULT.findall(text)), 1)
        with self.assertRaisesRegex(ValueError, 'Timed-mode'):
            levels.select_entry(FLOW, entries[2])

    def test_direct_previews_are_fresh_and_leave_projects_and_game_files_unchanged(self):
        with tempfile.TemporaryDirectory() as name:
            root=Path(name);mods=root/'mods';game=root/'gamedata';game.mkdir()
            source=game/levels.GAMEFLOW;source.write_bytes(FLOW.replace('\n','\r\n').encode())
            project=mod_projects.create(mods, ROOT/'launcher/ui/modkit-templates.json',
                {'template':'scene-editor','id':'creator.preview','name':'Private preview'})
            descriptor=json.loads((ROOT/'modkit/api.json').read_text());descriptor['features']=['editor-viewport-1']
            before={str(p):p.read_bytes() for p in Path(project['folder']).rglob('*') if p.is_file()}
            sessions=[mod_projects.prepare_test(mods,'creator.preview',root/'userdata',descriptor,
                'FIRST:MODE_STANDARD:2',game) for _ in range(2)]
            self.assertNotEqual(sessions[0]['user_data'],sessions[1]['user_data'])
            for session in sessions:
                flow=levels.read_script(session['user_data']/'editor-content'/levels.GAMEFLOW)
                chosen=next(e for e in levels.catalog(flow) if e['episode']=='FIRST')
                self.assertEqual(chosen['name'],'Second map')
                self.assertEqual(chosen['checkpoint'],'{00000000-0000-0000-0000-000000000001}')
                self.assertEqual(json.loads(session['profile'].read_text())['enabled'],['creator.preview'])
            mod_projects.record_test(sessions[-1])
            marker=json.loads((sessions[-1]['session_root']/'last-test.json').read_text())
            self.assertEqual(marker['relative'],sessions[-1]['user_data'].relative_to(sessions[-1]['session_root']).as_posix())
            self.assertEqual(source.read_bytes(),FLOW.replace('\n','\r\n').encode())
            self.assertEqual(before,{str(p):p.read_bytes() for p in Path(project['folder']).rglob('*') if p.is_file()})
            with self.assertRaisesRegex(mod_packages.ModError,'Update the game'):
                mod_projects.prepare_test(mods,'creator.preview',root/'userdata',{},'FIRST:MODE_STANDARD:2',game)
            with self.assertRaisesRegex(mod_packages.ModError,'standard map'):
                mod_projects.prepare_test(mods,'creator.preview',root/'userdata',descriptor,'FIRST:MODE_TIME_CHALLENGE:1',game)

    def test_frame_selection_uses_native_pose_axes_and_preserves_the_lens(self):
        session=EditorSession(SimpleNamespace(USER_DATA='/unused'),'creator.preview',0)
        calls=[];pose={'position':{'x':20,'y':3,'z':10},'rotation':{'pitch':0,'yaw':90,'roll':8},'fov_degrees':53}
        def call(method,**args):
            calls.append((method,args))
            if method=='camera.pose':return copy.deepcopy(pose)
        session.call=call
        session.focus_position({'x':10,'y':2,'z':30})
        changed=calls[-1][1]['pose']
        self.assertEqual(set(changed),{'position','rotation','fov_degrees'})
        self.assertEqual(changed['fov_degrees'],53)
        self.assertAlmostEqual(changed['position']['z'],30)
        self.assertLess(changed['position']['x'],10)
        self.assertGreater(changed['position']['y'],3)
        self.assertEqual(changed['rotation'],{'pitch':15,'yaw':90,'roll':0})

    def test_clearing_a_preview_cannot_remove_another_owned_object(self):
        session=EditorSession(SimpleNamespace(USER_DATA='/unused'),'creator.preview',0)
        session.call=mock.Mock()
        session.clear_preview();session.call.assert_not_called()
        session.preview_entity='entity:exact-preview'
        session.clear_preview();session.call.assert_called_once_with('world.destroy',entity='entity:exact-preview')
        self.assertIsNone(session.preview_entity)


if __name__ == '__main__': unittest.main()
