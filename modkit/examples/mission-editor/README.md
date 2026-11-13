# Mission editor

Modders: create this project through **File → New project → Mission editor** in Modkit or
the creator CLI's `--template mission-editor` option. Creation includes the
shared Scene editor as scene.lua; copying this example folder alone does not.
Your project starts disabled, with the creator credit you entered.

Choose **Test**, load a level and open **Options → Extras → Mods → Mission
editor**. Add objectives at your character, choose reach, continuous stay or
wait rules, and set prerequisites, optional goals and retry points. Stop the
preview before editing. The Scene editor page in the same project places props
and supports movement, rotation, scaling and undo/redo.

Choose **Save mission and scene drafts** and wait for confirmation. Close Test,
then choose **Apply saved drafts** in Modkit. Both documents are applied
together after checking the project fingerprint. **Mission settings** edits
names, descriptions and rules without writing Lua. Save those edits before
testing again. Validate, Test and Export ZIP when ready; importing preserves
your credit and leaves the package disabled.

Read the source comments before extending the rules. Only the living family
members controlled by logical P1/P2 count toward visits; idle AI characters do
not. A stay must be continuous for the same player. Clocks use simulation time
and stop while paused. Retry restores objective progress and editor clocks;
it does not rewind the world. Exact saves separately retain supported objects
and mission state.

This is a one-episode route and prop editor, with at most 32 objectives and
64 props. It does not replace campaign progression, provide a mission HUD,
create NPC encounters, or author maps, collision or navigation. See the
[mission editor guide](https://github.com/YesterMester/TheSimpsonsGameRecomp/blob/main/modkit/MISSION_EDITOR.md)
for the document format and full workflow.
