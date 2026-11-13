# Exact saves

An exact save keeps the whole running game — its memory, every guest thread's registers and the
runtime's state around them — so loading it puts the player back at the exact moment, mid-level
included, instead of at the last checkpoint. `simpsons/src/snapshot.cpp` makes and restores them.
The recompiled game runs as native code, so a guest thread can't be serialized and recreated
mid-function (the runtime's inherited save-state code stops there: "thread restore not
implemented"). Instead a snapshot is put back *under* a running game whose threads are parked at
the same points.

## Why it works

- The generated code keeps every guest register in the thread's `PPCContext` (no
  `REX_CONFIG_*_AS_LOCAL`) and its frames on the guest stack, in guest memory. A thread parked in a
  kernel call has host frames that only hold return addresses, so giving it the saved memory and
  registers makes it carry on as the saved thread would have — as long as its host call chain is
  the same.
- At a pause in gameplay every guest thread is parked at the same call chain and stack pointer:
  the same across pauses, after playing, in different levels and across launches (14 guest
  threads; thread ids, start contexts and stacks are deterministic). The main thread is at the
  frame start (`sub_82718788`, chain `826B83D4 82867DF0 82860894 …`). At the main menu the main
  thread's chain differs by one frame (`8285FDD0`) and one worker (thread 22) doesn't exist yet,
  so a snapshot can only be put back while a level runs.

## Freezing

Everything that runs guest code or writes guest memory stops, in this order:

1. Audio, before the frame the freeze happens in (`snapshot::Ready`). `AudioSystem::Pause` waits
   until its worker is between the game's callbacks, and a callback can be waiting on the main
   thread until later in its frame: paused from the main thread at a frame start, that wait never
   ends (it did, intermittently, in play and after a restore). So the pause is asked of another
   thread, the game runs a frame or two without audio, and the freeze follows at a frame start
   once audio has paused. Then the GPU command processor (`GraphicsSystem::Pause`).
2. The game's graphics interrupt callback, `sub_82456830`, is skipped while frozen. The runtime
   holds its global lock around the callback (`ExecuteInterrupt`), so waiting inside it deadlocks.
3. Guest threads park cooperatively: `XThread::RequestFreeze`, then every blocking host wait
   (`XObject::Wait`, `SignalAndWait`, `WaitMultiple`, `XThread::Delay`) runs in an
   `XThread::WaitScope`. A thread blocked in a wait can't run guest code; a wait that returns while
   frozen parks before returning. `XThread::AllFrozen` reports threads still running guest code.
   Signal-based suspension (`XThread::Suspend`) was unreliable: a suspended thread kept running
   and crashed on half-restored memory.
4. Deferred I/O (the kernel dispatch thread) drains and pauses (`KernelState::PauseDispatch`). File
   reads write guest memory, and a queued read would never complete after a restore.
5. The kernel's global lock must be free.

## The recompilation's own guest memory

Scratch memory for calls into the game, the canvas's vertices and its picture textures live in
guest memory, taken once at startup from one block of virtual memory (1 MB) and one of physical
memory (4 MB) (`simpsons/src/host_memory.cpp`), so they sit at the same addresses in every run of
a build. A restore leaves those pages as they are (their page-table entries and data), and an
image only goes back under a build whose blocks are allocated exactly as the image's were
(`snapshot::Fits`, checked as the save is read): otherwise the restore would hand the game's old
data over our memory, or ours over the game's. Saves keep the layout they were made under
(`layout` in save.toml); a save from another layout loads at its checkpoint instead.

## What a snapshot holds

- Every heap's page table (v00000000, v40000000, v80000000, v90000000, physical, and the
  0xA0000000/0xC0000000/0xE0000000 views) and the committed pages of the data heaps (the views
  share the physical heap's pages): 528 MB in Springfield and Big Super Happy Fun Fun Game, about
  286 MB of it empty. Between two pauses ~22 MB changes; after walking around ~39 MB.
- The runtime's function table for indirect calls (`0x82EC0000`, `(REX_CODE_SIZE + 0x10000) * 2`
  bytes) holds host pointers and is never restored.
- Each guest thread's `PPCContext`, matched to the live thread by id and start address and checked
  against its stack pointer and LR.
- The command processor's ring state (`GraphicsSystem::Save`); after a restore,
  `InvalidateGpuMemory` and `ClearCaches`.
- Kernel objects (events, semaphores, files, mutants, threads, notify listeners, timers). Handle
  numbers differ between launches (thread creation races), and guest memory refers to handles —
  including the handle stashed in guest dispatcher headers (`wait_list_blink` with the `REX\0`
  signature). The restore empties the object table and puts each live object that is the same
  thing (thread by id, object in guest memory by address, file by path, named object by name, the
  rest by type in handle order) under its saved handles with their open counts
  (`ObjectTable::RestoreHandle(handle, object, ref_count)`); saved objects with no live
  counterpart are rebuilt (`XObject::Restore`), e.g. a voice-line stream that was open at save time.
  `XFile::Save` wrote its path as a raw `std::string` object; fixed to write the text.
- Lazy native resolve memory (`native_resolve_buffer_lazy_memory`, on in the launcher's defaults):
  reading such a page from the frame-start hook triggers a GPU readback and hangs, so unreadable
  host pages are opened page by page and read as they are (their contents are render output that
  is redrawn every frame).

## Measured

- Freeze 30–50 ms; copying memory 0.4–0.6 s; restoring memory 0.1–0.3 s.
- In one session: 10 rollbacks in a row in Springfield, each identical to the saved frame (mean
  difference 0.2/255) and the game responsive after.
- Across launches: Springfield free roam, and mid-level in Big Super Happy Fun Fun Game five
  times from one snapshot with identical results, including a voice line that was playing at save
  time finishing after the load. Works with lazy resolve memory on.

## Development commands

`snapshot_test_file` takes `info LABEL` (threads, objects, memory hashes), `save NAME` / `load NAME`
(in this process), `savefile PATH` / `loadfile PATH` and `matchfile PATH` (the object matching only).
The harness steps are `snap CMD ARGS` and `stacks NAME` (all thread stacks with eu-stack; test runs
allow any debugger to attach). Progress of a freeze and a restore goes to stderr, flushed, because
the log can still be buffered when something hangs.

## The save library and its screens

`simpsons/src/saves.cpp` keeps saves under `<user data>/saves/<id>/` (save.toml, thumb.png,
state.snap, and a copy of the game's slot A), the snapshot memory in a shared store
(`saves/chunks`, 64 KB pieces by xxh3-128, snappy-compressed): a first save in a level is 70–85 MB,
later ones nearby 7–12 MB. The game only ever plays slot A: loading puts the save's copy back
as slot A first. A watcher thread compares the game's slots with what the library last knew
(`slots.toml`): on the first run every slot with progress becomes a playthrough of its own
("Slot C"), then new progress the game writes to slot A becomes an autosave of the playthrough
being played (three kept); a slot whose header text (play time, completion, where) hasn't
changed is no new save, which also skips the rewrite the game does as a level starts.

Snapshot files and chunks use the runtime's native file API, including wide paths on Windows,
so accented and non-Latin account names work when saving and loading. Metadata and thumbnails
pass UTF-8 paths to their readers and writers too.

`simpsons/src/saves_screen.cpp` uses the original APT menu, its shipped text references,
buttons, TV artwork and transitions. A custom RenderWare widget occupies the original menu's
text child in the APT display list; it does not draw after the finished frame or hide the TV
behind another panel. The widget clips in its parent's local space, then transforms vertices
and touch targets with the APT matrix. The layout follows the title's transform so one
button's pulse does not bounce the whole page. It provides framed save previews, a two-column list,
metadata, and larger touch keyboard cells. The front end's `PickSlot` (`0x8239F330`) provides
four text references; its button script can reuse a reference for the fifth native action.
Longer save lists use pages. Original arrays, text properties and selection are kept and restored
before the game's own menus run again. The slot labels are refreshed by the game during its
render pass, so library text also passes through the native APT text hook. Text fitting uses
the native font's glyph advances; these children's `textWidth` and `textHeight` are invalid.

Saved Games offers Continue, New Game, Load Game, the playthrough's Game Menu and Classic
Slots. Choosing a save restores slot A, answers the original load question and chooses Continue.
The game's TV turns off and its original loading screen shows. An exact save goes in once the
level plays (`LevelStepped`; the loading screen is in state 9 too): black from that frame,
START, the snapshot under the paused game, then B, and the black lifts as the pause menu closes.
A new game empties slot A through the original Delete confirmation first. Save data, snapshot
chunks and thumbnails keep their existing format; the native menu shows the saved previews.

The pause menu's script starts from every row (`MenuItemIds` with `MenuItemButtons` and
`TextRefs`), drops those in `RemoveItems` (always `Save_game`) in `initializeButtons`, and asks the
native `IsPlayingDemo` (`sub_823AC620`) just before. With Save anywhere, Save Game's id becomes
`Save_anywhere` there, so the row stays with the game's own button and text; Accept on it
(`sub_823A6C78`) opens the saves screen. Saves are taken with the pause menu up (the game's audio
is paused then) and the screen gone.

The library reads its list in the background, filters cached names as text is typed and
keeps selection by save id when the catalog refreshes. A save's actions are on its own page:
Load, Rename, Delete and Back. Deleting starts on Cancel. Keyboard controls are Enter, Esc,
F2 (rename), Delete (delete), F3 (find/filter) and Page Up / Page Down (pages). Backspace and
Delete erase during naming; Enter finishes. Controller shoulders change page. The native
keyboard supports sticks, the D-pad and touch; whole key cells are touch targets, transformed
with the widget rather than inferred from an unrelated screen layout. Footer prompts follow the active input.

For input checks, `game_ui_test_file` also accepts `key VKEY` and `char CODEPOINT`; both go through
the same window listener as keyboard input. This lets a private run check text entry without
taking keyboard focus from another window.
