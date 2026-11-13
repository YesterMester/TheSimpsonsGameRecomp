# In-game settings and player input

The development build adds PC settings and player assignment to the original Options movie,
shared by the front end and pause menu. It uses the game's APT interpreter, reference text clips,
pulsing buttons, font renderer and input listener. There is no separate settings overlay.

## Menu lifecycle

`game_menu.cpp` hooks the Options screen's show (`0x823A4890`), state transition (`0x823A4C90`)
and input (`0x823A5768`). The singleton is at `0x82D08DDC`; the root state at `0x82D08DE0` is 1.
That state remains set after closing, so it cannot identify a visible screen by itself.
The base screen's active flag at `screen + 28` and close function (`0x827F3780`) determine when
to release keyboard capture. The suspend hook (`0x823A4850`) also releases capture.

`game_menu_apt.cpp` wraps the title's variable and method interfaces on its own game thread:

| Interface | Entry |
|---|---|
| Get / set a property | `0x827BC0C8` / `0x827BC128` |
| Construct / release a string reference | `0x827C8180` / `0x827C7BB8` |
| Create string / integer / float values | `0x827C6A60` / `0x827C5CB8` / `0x827C5EE0` |
| Invoke a method | `0x827BF8F8` |

The method entry uses its movie argument as the receiver even when the method name contains a
path. Nested calls therefore pass the owner path separately from the method name. Array length
is readable but direct writes are ignored; `splice` clears arrays through the interpreter's own
ownership rules. This prevents old buttons from surviving on a shorter page.

The five original reference clips retain their font metadata and measured scale (about 67%).
Pages with more settings use four rows and a More entry. The input listener stops before a
rebuild and restarts once after button activation. New titles temporarily replace the original
Options title string during its native text draw and restore it afterward.

## Settings and assignments

Live changes use the appropriate game, input, GPU or window thread. Render scale, renderer,
V-sync, audio queue size and shader options are marked for restart. Restart choices are saved
without changing values that are already baked into initialized resources.

Only newly changed settings are written. `SaveConfigPatch` preserves comments, launcher markers,
unknown settings and unrelated TOML tables, then replaces the file atomically. Registry validation
finishes before disk access so the audio callback can continue reading live mute state.
The launcher saves a delta too and refreshes when it regains focus or the game exits.

The SDK routes physical inputs to logical player slots rather than combining their button states.
Assigning an input owned by the other player swaps their assignments. Captured and held inputs
are suppressed until released. Rumble and keystroke events follow the assigned input; any-user
keystroke queries retain the event's logical player. The [XInput event contract](https://learn.microsoft.com/en-us/windows/win32/api/xinput/nf-xinput-xinputgetkeystroke)
requires any-user queries to poll all users rather than pinning them to player one.

## Prompts and qualification

Button tags are identified through the game's resolver (`0x826BEEE0`). Replacement shapes and
labels use the existing native font batch, retaining the original frame, transforms and render
backend. Atlas coordinates have four fractional bits and need the original renderer's 1/16
conversion. Standalone footer icons have separate text fields from Back and Select; their keycaps
grow away from those labels. Inline tags can use cloned glyph metrics for longer key names.
Geometry is built in pixels after converting the font manager's coordinates; HUD text also uses
normalized coordinates, unlike the menus. Some icon-only HUD fields select a metric-only font
with transparent 1x1 glyphs. Replacement labels select a usable font from the currently loaded
font manager, avoiding both invisible keycaps and references to unloaded front-end fonts.
The resolver's fixed 20-glyph table also covers button glyphs drawn without an observed tag.
Direct HUD text establishes its owner before both measurement and drawing. The title's literal
START instruction uses the native Start tag.
Unrecognized tags and unsupported font callbacks retain the original rendering.

Linux live checks cover the original TV screen, root and nested pages, shorter pages, keyboard
navigation, controller assignment, automatic PlayStation face icons, native keyboard keycaps,
original Audio and pause-menu settings. The input fixture covers separate keyboard/controller
states, swaps, per-player prompt overrides, hotplug, held-input suppression, any-user events and
config preservation. Windows and Android source checks are separate from device playtests.

Names embedded in artwork and literal button names are not all converted. Two-player viewport
ownership uses the active camera slots and their integer bounds at
`manager + 36 + 16 * slot`. Slot 0 is the full-screen view, slots 1 and 2 are the two players' views.
The count includes inactive cameras, and director ids identify characters rather than players.
A live keyboard/controller check in Big Super Happy Fun Fun Game confirmed the split layout and
separate character control. HUD text drawn into separate half-size targets uses the original
`0x82734200` draw context at `0x82D097AC`: the split flag is at +112 and the player pass is at
+116 (1 or 2). That explicit owner applies only when split passes are enabled; full-image HUD
draws select their owner from the final font quad's viewport. Campaign-wide prompt coverage
remains to be checked.

## Mouse and touch

`game_menu_pointer.cpp` follows the common screen show (`0x827F2DC8`) and close lifecycle.
The deepest active screen receives input through its original vtable method at +100. Pointer
selection calls the original menu movement functions, preserving the screen's selection logic.
Startup screens that poll controller state instead of that callback receive a short native input
pulse. Screen changes cancel pending pulses; assignments and controller prompt families are kept.
Rendered APT text and button-font quads supply hit regions; native selectors and sliders receive
left/right actions instead of confirmation. Save-slot letters provide stable targets while their
displayed times change. Footer buttons retain their original actions. Multiple instances of a
button have separate targets, including confirmation popups; inactive save rows do not receive
input behind their confirmation screen. Targets expire when they stop being drawn.

The presenter exposes its full guest output rectangle before clipping. Pointer coordinates are
mapped through it, including letterboxing, cropping and overscan. A press and release must hit
the same target on the same page; cancellation and releases outside the target do not activate
it. A second touch cannot complete the first touch's gesture. Menu mode releases mouse capture
and clears accumulated camera movement without taking controller navigation away.

Android reads the native menu state to hide the touch gamepad and its layout editor while a menu
is open. SDL then sends touches directly to the same native targets. Device playtests remain
separate from cross-compilation and package checks.
