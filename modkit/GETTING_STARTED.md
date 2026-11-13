# Your first mod

Open the separate **Modkit** desktop application and choose **File → New project**.
Enter your name for the creator credit. Projects start disabled; **Test** uses
separate progress and leaves your normal saves alone.

| Start with | What you can change |
| --- | --- |
| Script starter | A small native Options page, an action and a saved counter. |
| Player actions | An ability for the character controlled by P1 or P2, with independent cooldowns. |
| Timed events | A delayed gameplay action that pauses with the game and survives supported mod-state restores. |
| Scene editor | Supported native props, placement, transforms and saved scene drafts. |
| There and back | Mission objectives, failure, retry points and saved progress. |
| File replacements | Your edited files at their original relative paths beneath `gamedata/`. |

For a first project, choose **Script starter**. Open `locales/en.json` and change
the page title and action label. Open `main.lua`: its comments explain where to
put your action and why saved values belong in `game.state`.

Use **Validate**, then **Test**. In the game, open **Options → Extras → Mods**
and select your page. Use the code reload action for Lua-only edits. Resource,
metadata and translation changes need a restart of Test.

Use **Export package** to create a ZIP with your name, version and checksums.
Other players import it from the launcher's **Mods** tab. Android imports and
manages these packages; authoring stays in the desktop Modkit.

Keep display words in `locales/en.json` and stable keys in Lua. This lets others
[translate your mod](TRANSLATIONS.md) without changing its behavior. Read the
built-in API reference before adding a named operation; unsupported asset
types or behaviors still need a reviewed native adapter.

Do not distribute `default.xex`, unmodified game archives or someone else's
game files with your project. Small language packs contain translated strings,
and data packages should contain only your own supported replacements.
