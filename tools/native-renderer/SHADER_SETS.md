# Building a shader set

Use a runtime rebuilt from the current SDK sources. Exported modules record the
real translator source hash and device configuration. Changing those labels on
an older set does not make its shaders compatible.

The source storage is a private cache from your own game. The builder copies it
before use. Keep the existing archive as the previous set so all its requested
variants are rebuilt, including variants absent from recorded pipelines.

To bake without launching the game or replaying a frame:

```sh
sh tools/native-renderer/build_translated_set.sh \
  /path/to/cache - /path/to/new-set.tar.xz \
  tools/native-renderer/translated_shaders.tar.xz "" /path/to/current/runtime
```

The runtime directory defaults to `tools/rexglue-sdk/out/linux-amd64`. The
storage-only exporter creates a headless Vulkan device, translates the stored
programs and requested modifications at 1x and 2x, then exports them during
shutdown. It does not execute game logic or render a test scene. Passing a trace
instead of `-` retains the existing export-and-replay workflow and its error
checks.

The builder rejects errors, an outdated translator hash or missing previously
shipped variants before replacing its output. Validate the SPIR-V modules too,
using the Vulkan target corresponding to each module's declared SPIR-V version.
Keep the candidate separate until those checks pass.

`native_shaders/build_native_shaders.py` adds the native GLSL replacements and
the shared warmup metadata. Android uses those replacements and variant
identities to prepare its own device-specific cache from the installed game;
it does not use the desktop translated binaries. A successful storage bake is
shader preparation evidence, not proof of complete campaign rendering coverage.
