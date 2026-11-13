# Android shader preparation

The game prepares known shader variants before resuming its main thread. Java
polls the runtime's completed/total counters and stage without blocking the UI.
The touch controls stay hidden until preparation completes. Disabling shader
storage still completes the loading screen.

The source programs come from the installed, owned executable. Vertex-format
patches are reconstructed with `warmup-vertex-patches.txt`: an original hash,
an expected result hash, a word count and word-index/XOR-mask pairs. Both program
identities are checked before the result enters the cache. There is no original
shader bytecode in this file. Unsupported or missing programs retain normal
runtime compilation and can be prepared on later launches.

`warmup-pipelines.txt` contains checksummed, packed pipeline state descriptions.
Its header records the schema version and description size. Import rejects
incompatible versions, missing shader identities and unsupported device state.
It contains no driver binaries. `warmup-modifications.txt` is generated from the
packaged shader filenames when Android assets are staged.

The October 16 set contains 6,660 translated modules. It preserves all 6,640
previously validated modules byte for byte and adds 20 variants for three more
observed vertex programs. The source/cache inventory now contains 383 programs.
Bundled translations avoid retranslating those known variants; device pipeline
creation still depends on the available program identities and supported state.

Use `tools/native-renderer/collect_warmup_recipes.py --help` to refresh the two
tracked recipe files from an `xex_inventory` JSON report, a local `.xsh` shader
storage and matching `.fbo.vk.xpso` storages. The developer collector requires
libxxhash. Do not redistribute those input storages or relabel old pipeline
records as a new schema. Asset staging includes the recipes in its content index.

Device translations use the translator source/configuration key and a separate
directory for each render scale. Opaque Vulkan driver caches remain checked by
the Vulkan driver. Translated modules, binding sidecars, pipeline descriptions
and driver-cache updates use temporary files so interrupted writes do not replace
previous complete files. Shader seeding validates and repairs partial trailing
records. Driver caches are saved after preparation and periodically when new
pipelines have been created, rather than only when Android exits cleanly.

The rectangle vertex loop uses an undecorated local copy of its per-vertex data.
The actual output keeps its BuiltIn decorations; decorating the local copy was
invalid Vulkan SPIR-V even on drivers that happened to accept it.

This prepares known work, not every possible campaign draw state. Native Windows
and Pixel timing need hardware qualification. A desktop check can disable vertex
storage writes and limit storage-buffer range, but does not reproduce every
Mali driver feature or the phone's thermal limits.
