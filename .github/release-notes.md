You need your own copy of The Simpsons Game for Xbox 360. No game content is included; the
launcher installs the game from your own ISO.

### Security

GitHub's code scanning and Dependabot are now turned on for the project. This release fixes what they found in the launcher, the game's runtime and the tools that come with it.

- The launcher's built-in server, which the launcher window talks to, now only answers requests addressed to 127.0.0.1 or localhost. Before, a web page that pointed its own domain name at your computer (DNS rebinding) could read the launcher's answers, including its folder listings. Every request to the launcher's API now also needs the launcher's token; the ISO browser's folder listings didn't before.
- The ISO browser only opens folders inside its starting locations (your home folder, drives and media folders), checked after resolving `..` and links, and a folder like `/home/deck2` no longer counts as being inside `/home/deck`. Launcher artwork, fonts and save backups are only served or restored by their exact name in their own folder.
- Game runtime: sizes and offsets for memory protection, index buffers and GPU buffer bindings are computed in 64 bits so they can't overflow first, and closing a window no longer goes through code that could call into a half-destroyed window. The Xbox 360 kernel's DES encryption functions are stubs now and the DES code is removed; the game never calls them.
- extract-xiso, which installs the game from your ISO: its rewrite mode (`-r`, which the launcher doesn't use) can no longer replace a file that appears next to the image while it runs.
- The Build check workflow's GitHub token can only read the repository.
- Bundled third-party code: SPIRV-Tools' sva, a JavaScript tool that nothing here builds or runs, is removed along with the vulnerable npm packages it pinned (brace-expansion, braces, minimatch, js-yaml, nanoid, serialize-javascript and others), and Google Benchmark's Python requirements now ask for scipy 1.10.0 (CVE-2023-25399, CVE-2023-29824). Neither was part of the game or the launcher.

### Renderer

- Render target copies that full-screen passes overwrite anyway are skipped.
- The game's clears are done as real GPU clears, which the GPU can fast-clear: about 0.45 ms less GPU time per frame at 2x.
- The post-processing passes fetch their texture samples in batches so the waits overlap: about another 0.45 ms per frame at 2x.
- Same images as 0.0.6.2 in captured scenes at 1x and 2x.

### Launcher

- The launcher no longer uses about a third of a CPU core while it is open. Its spinning donut and picture crossfade repainted the whole page every frame, which took frames from the game when the launcher stayed open behind it. The donut now only spins when clicked, and the pictures change every 15 seconds and not at all while the game runs or the launcher is hidden.

### Known issues

- In-game prompts still show controller buttons. Press F1 to see which key each one is on.
- Keyboard and mouse has been tested much less on Windows than on Linux and the Steam Deck.
- At 2x, occasional frame drops remain in the busiest areas (a few per minute in Springfield). They come from the game's frame timing on the CPU, not from the GPU.
- Frame rate settings above 60 do not add frames, because the game's frame scheduler tops out at 60 FPS, and they make frame pacing less even. 60 is recommended.
- At 60 FPS some scripted sequences can misbehave. If random deaths happen at the dam in "Lisa the Tree Hugger", switch to 30 for that section.

### Installing

- Windows: extract the zip and run `simpsons-launcher.exe`.
- Linux and Steam Deck: extract the archive and run `Play.sh`.
- CPU without AVX2 (most CPUs from before 2013): use the package ending in `-NoAVX2` instead.
- In the launcher's Install tab, select your Xbox 360 ISO, then press Play.
- Existing installs can update from the launcher's About tab.
