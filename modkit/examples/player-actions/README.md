# Player actions

A controlled-character action with independent P1/P2 cooldowns and an optional input shortcut.

1. Open main.lua and read the comments addressed to modders.
2. Change the words in locales/en.json. Keep the stable keys and placeholders.
3. Use Validate, then Test. The project starts disabled and Test has separate progress.
4. Open Options > Extras > Mods in the game to use the page.
5. Use Export to make an installable ZIP with your creator credit.

To let others translate this mod, keep default_locale and locales in mod.json.
In Modkit choose File > New language pack, select this mod and enter the new
language and translator credit. Exported language packs contain only translated
strings; missing entries fall back to the original.

This is a starting point using public named APIs. It does not require editing
recompiled functions, changing game addresses or replacing original archives.
