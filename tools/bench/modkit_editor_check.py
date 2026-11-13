#!/usr/bin/env python3
"""Exercise the native standalone editor without a game or normal save writes."""
import argparse
import copy
import faulthandler
import importlib.util
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import threading
import time
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]


def main():
    faulthandler.dump_traceback_later(60, exit=True)
    parser = argparse.ArgumentParser(description=__doc__); parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--platform', default='offscreen', choices=['offscreen', 'xcb', 'wayland'])
    args = parser.parse_args(); output = args.output.resolve(); output.mkdir(parents=True, exist_ok=True)
    os.environ['QT_QPA_PLATFORM'] = args.platform
    sys.path.insert(0, str(ROOT / 'launcher'))
    import modkit_app, mod_packages
    from PySide6.QtWidgets import QApplication, QFileDialog, QInputDialog, QLineEdit, QMessageBox
    from PySide6.QtCore import QSettings, Qt
    from PySide6.QtGui import QTextCursor
    from PySide6.QtTest import QTest
    application = QApplication([]); modkit_app.apply_theme(application)
    with tempfile.TemporaryDirectory(prefix='modkit-editor-') as name:
        base = Path(name); launch_dir = base / 'launcher'; launch_dir.mkdir()
        shutil.copy2(ROOT / 'launcher/launcher.py', launch_dir / 'launcher.py')
        spec = importlib.util.spec_from_file_location('native_editor_backend_check', launch_dir / 'launcher.py')
        backend = importlib.util.module_from_spec(spec); spec.loader.exec_module(backend)
        backend.UI_DIR = ROOT / 'launcher/ui'; backend.USER_DATA = base / 'userdata'; backend.USER_DATA.mkdir()
        backend.MODS_DIR = launch_dir / 'mods'; backend.game_running_pids = lambda: []
        backend.BUILD_DIR = base; shutil.copy2(ROOT / 'modkit/api.json', base / 'simpsons-modkit.json')
        window = modkit_app.ModkitWindow(backend, settings=QSettings(str(base / 'settings.ini'), QSettings.IniFormat)); window.show()
        def wait(predicate):
            deadline = time.monotonic() + 12
            while time.monotonic() < deadline:
                application.processEvents(); QTest.qWait(20)
                if predicate(): return
            raise RuntimeError('Timed out: ' + window.output.toPlainText())
        wait(lambda: not window.jobs)
        window.create_project({'template':'mission-editor','name':'Springfield delivery','id':'creator.editor-route',
                               'author':'Créateur UI','description':'A route authored in the standalone editor.'})
        wait(lambda: window.project and window.source and not window.jobs)
        assert window.project['id'] == 'creator.editor-route'
        title = window.findChild(QLineEdit, 'missionTitle'); assert title
        title.selectAll(); QTest.keyClicks(title, 'Downtown deliveries'); assert window.dirty
        window.save_source(); wait(lambda: not window.jobs); assert not window.dirty
        window.open_source('main.lua'); wait(lambda: not window.jobs and window.source['path'] == 'main.lua')
        window.code.appendPlainText('\n-- Edited in standalone Modkit.'); window.save_source(); wait(lambda: not window.jobs)
        assert not window.dirty
        folder = Path(window.project['folder']); before = (folder / 'main.lua').read_text()
        (folder / 'main.lua').write_text(before + '\n-- External edit.\n')
        window.code.appendPlainText('-- Stale edit.'); window.save_source(); wait(lambda: not window.jobs)
        assert window.dirty and 'changed since you opened' in window.output.toPlainText()
        assert (folder / 'main.lua').read_text() == before + '\n-- External edit.\n'
        with mock.patch.object(QMessageBox, 'question', return_value=QMessageBox.Cancel):
            assert not window.close() and window.isVisible()
        # A second file keeps its own unsaved buffer. Explicitly discard only
        # this stale buffer; switching tabs must never erase it implicitly.
        with mock.patch.object(QMessageBox, 'question', return_value=QMessageBox.Discard):
            window.close_source(window.tabs.indexOf(window.documents['main.lua']))
        window.open_source('mod.json'); wait(lambda: not window.jobs)

        # Independent buffers survive tab changes and Save All keeps every
        # file's own revision. A partially failed save must advance revisions
        # only for files that were actually written.
        window.open_source('main.lua'); wait(lambda: not window.jobs)
        script = window.code; script.appendPlainText('-- Retained source buffer.')
        window.open_source('README.md'); wait(lambda: not window.jobs)
        readme = window.code; readme.appendPlainText('Retained documentation buffer.')
        window.open_source('main.lua'); assert window.code is script and window.dirty
        assert script.toPlainText().endswith('-- Retained source buffer.')
        window.save_all(); wait(lambda: not window.jobs)
        assert not window.dirty and (folder / 'README.md').read_text().endswith('Retained documentation buffer.')
        script.appendPlainText('-- First save succeeds.')
        readme.appendPlainText('Keep this unsaved documentation.')
        (folder / 'README.md').write_text((folder / 'README.md').read_text() + '\nExternal documentation edit.\n')
        window.save_all(); wait(lambda: not window.jobs)
        assert script not in window.changed_sources() and readme in window.changed_sources()
        assert (folder / 'README.md').read_text().endswith('External documentation edit.\n')
        with mock.patch.object(QMessageBox, 'question', return_value=QMessageBox.Discard):
            window.close_source(window.tabs.indexOf(readme))
        assert not window.dirty

        # Offline scene properties edit the same portable document, keep full
        # native basis vectors, and participate in the source undo history.
        window.open_source('mod.json'); wait(lambda: not window.jobs)
        source = window.code; original_manifest = json.loads(source.toPlainText())
        scene_manifest = copy.deepcopy(original_manifest)
        scene_manifest['scene'].update(level_root='gamehub/gamehub.str', objects=[{
            'id':'scene-1', 'name':'Coffee table', 'radius':1,
            'source':{'guid':'0123456789ABCDEF0123456789ABCDEF', 'class':'ScriptableProp', 'stream':'brt/brt/zone01.str'},
            'transform':{'position':{'x':1.25,'y':2.5,'z':-3}, 'right':{'x':1.5,'y':0,'z':0},
                'up':{'x':0,'y':.75,'z':0},'forward':{'x':0,'y':0,'z':2}}}])
        window.replace_document(source, json.dumps(scene_manifest, indent=2) + '\n')
        assert window.scene_tree.topLevelItemCount() == 1
        window.selected_tag='scene-1'; window.edit_object('position-x', 12.5)
        assert window.scene_document()['objects'][0]['transform']['position']['x'] == 12.5
        window.scene_action('undo')
        assert window.scene_document()['objects'][0]['transform']['position']['x'] == 1.25
        window.scene_action('redo')
        assert window.scene_document()['objects'][0]['transform']['position']['x'] == 12.5
        window.edit_object('rename', 'Living room table')
        assert window.scene_document()['objects'][0]['id'] == 'scene-1'
        assert window.scene_tree.topLevelItem(0).text(0) == 'Living room table'
        window.edit_object('duplicate')
        assert len(window.scene_document()['objects']) == 2 and window.selected_tag == 'scene-2'
        assert window.scene_document()['objects'][1]['transform']['right'] == {'x':1.5,'y':0,'z':0}
        window.scene_filter.setText('missing-object'); assert window.scene_tree.topLevelItem(0).isHidden()
        window.scene_filter.clear()
        window.edit_object('remove'); assert len(window.scene_document()['objects']) == 1
        window.scene_action('undo'); assert len(window.scene_document()['objects']) == 2

        # Asset filters run against one model and bookmarks survive new native
        # handles. Unsupported resource types never become placeable props.
        assets = [{'id':'template:1','name':'Coffee table','category':'Props','class':'ScriptableProp',
                   'source_stream':'brt/brt/zone01.str','source_guid':'1'*32,'model_resource':'2'*32},
                  {'id':'template:2','name':'Frink','category':'NPCs','class':'NPCFrink',
                   'source_stream':'gamehub/zone02.str','source_guid':'3'*32,'model_resource':'4'*32},
                  {'id':'template:3','name':'Coffee cup','category':'Props','class':'ScriptableProp',
                   'source_stream':'brt/brt/zone02.str','source_guid':'5'*32,'model_resource':'6'*32}]
        library = window.library; library.set_records(assets); library.props.setChecked(True)
        assert library.proxy.rowCount() == 2
        library.search.setText('coffee zone01'); library.filter(); assert library.proxy.rowCount() == 1
        library.view.setCurrentIndex(library.proxy.index(0,0)); library.toggle_favorite()
        library.search.clear(); library.props.setChecked(False); library.starred.setChecked(True); library.filter()
        assert library.proxy.rowCount() == 1
        assets[0] = {**assets[0], 'id':'template:new-session'}; library.set_records(assets)
        assert library.proxy.rowCount() == 1 and library.proxy.index(0,0).data(Qt.UserRole)['id'] == 'template:new-session'
        assert not modkit_app.placeable(assets[1])
        window.tabs.setCurrentWidget(window.viewport); window.scene_dock.raise_(); window.assets_dock.raise_()
        window.resize(1280,800); application.processEvents(); window.grab().save(str(output / 'scene-workspace.png'))
        window.replace_document(source, json.dumps(original_manifest, indent=2, ensure_ascii=False) + '\n')
        window.save_all(); wait(lambda: not window.jobs)

        # A foreground edit requested during a quiet poll executes once after
        # the read, instead of being lost by the single-worker busy guard.
        release = threading.Event(); order = []
        window.work('Test poll', lambda: release.wait(3), lambda value: order.append('read'), quiet=True)
        window.work('Test edit', lambda: order.append('write'), lambda value: order.append('done'))
        assert window.pending_work; release.set(); wait(lambda: not window.jobs and not window.pending_work)
        assert order == ['read','write','done']

        window.validate_project(); wait(lambda: not window.jobs)
        with mock.patch.object(QInputDialog,'getInt',return_value=(3,True)):
            window.goto_line()
        assert window.code.textCursor().blockNumber()==2
        assert 'dependencies are valid' in window.output.toPlainText()
        archive = base / 'editor-route.zip'
        with mock.patch.object(QFileDialog, 'getSaveFileName', return_value=(str(archive), 'Mod package (*.zip)')):
            window.export_project(); wait(lambda: not window.jobs)
        installed = mod_packages.install(archive, base / 'imported')
        assert installed['author'] == 'Créateur UI' and mod_packages.profile(base / 'imported')['enabled'] == []
        window.show_help('api:world.spawn'); assert 'world.spawn' in window.help_text.toPlainText()
        window.show_help('EDITOR_WORKFLOW.md'); assert 'Getting started' in window.help_text.toPlainText()
        window.grab().save(str(output / 'workspace-desktop.png'))
        window.resize(1280, 800); application.processEvents(); window.grab().save(str(output / 'workspace-steam-deck.png'))
        window.resize(800,700); application.processEvents(); window.adapt_workspace()
        assert window.compact_mode and window.dockWidgetArea(window.property_dock) == Qt.LeftDockWidgetArea
        QTest.qWait(50); window.grab().save(str(output / 'workspace-compact.png'))
        window.resize(1280,800); application.processEvents(); window.adapt_workspace(); assert not window.compact_mode
        # Indenting a selection preserves both lines; readonly Test views reject edits.
        editor = modkit_app.SourceEditor(); editor.setPlainText('one\ntwo'); editor.selectAll(); QTest.keyClick(editor, Qt.Key_Tab)
        assert editor.toPlainText() == '  one\n  two'
        editor.setReadOnly(True); before = editor.toPlainText(); QTest.keyClick(editor, Qt.Key_Tab); assert editor.toPlainText() == before
        editor.setReadOnly(False); original = '🍩 Homer homer Homeric\nHomer'
        editor.setPlainText(original); find = modkit_app.FindBar(lambda: editor)
        find.query.setText('Homer'); find.words.setChecked(True); find.replacement.setText('Bart')
        find.replace_all(); assert editor.toPlainText() == '🍩 Bart Bart Homeric\nBart'
        editor.undo(); assert editor.toPlainText() == original
        editor.setReadOnly(True); find.replace_all(); assert editor.toPlainText() == original
        editor.setReadOnly(False); editor.setPlainText('  game.log.info("hello")'); editor.moveCursor(QTextCursor.End)
        QTest.keyClick(editor, Qt.Key_Return); assert editor.toPlainText().endswith('\n  ')
        palette = modkit_app.CommandPalette([{'label':'Unavailable action','enabled':False}, {'label':'Open source','enabled':True}])
        palette.search.setText('unavailable'); palette.activate(); assert palette.selected is None
        palette.search.setText('open source'); palette.activate(); assert palette.selected['label'] == 'Open source'
        find.close(); palette.close()
        window.set_compact_workspace(True, manual=True)
        window.timer.stop(); window.close(); application.processEvents()
        restored = modkit_app.ModkitWindow(backend, settings=window.settings)
        restored.show(); wait(lambda: not restored.jobs)
        assert restored.compact_mode and not restored.workspace_auto
        restored.reset_workspace(); assert restored.workspace_auto
        release_close = threading.Event()
        restored.work('Closing poll', lambda: release_close.wait(3), quiet=True)
        assert not restored.close() and restored.close_when_idle
        release_close.set(); wait(lambda: not restored.isVisible() and not restored.jobs)
        report = {'ok':True,'native_project_create':True,'guided_mission_edit':True,'source_save':True,
                  'stale_write_rejected':True,'unsaved_close_canceled':True,'credited_disabled_import':True,
                  'built_in_help_api':True,'readonly_source':True,'selection_indent_preserves_code':True,
                  'steam_deck_layout':True,'output':window.output.toPlainText()}
        report.update(multiple_source_buffers=True, save_all=True, partial_save_conflict_preserves_buffers=True,
            offline_scene_edit=True, scene_outliner=True, scene_source_undo_redo=True, native_basis_preserved=True,
            asset_filters=True, portable_asset_favorites=True, foreground_edit_survives_poll=True,
            unicode_find_replace_undo=True, readonly_replace_blocked=True, source_auto_indent=True, command_palette=True,
            compact_workspace_restores_wide_layout=True, compact_workspace_preference_persists=True,
            close_during_background_poll_finishes=True)
        report['go_to_source_line']=True
        (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
        shutil.copy2(archive, output / archive.name)
        print(json.dumps({k:v for k,v in report.items() if k != 'output'}))
    faulthandler.cancel_dump_traceback_later()


if __name__ == '__main__':
    main()
