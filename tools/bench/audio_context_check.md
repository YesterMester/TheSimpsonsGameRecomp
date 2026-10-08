# Audio buffer check

Build the Linux runtime first, then run:

```sh
python3 tools/bench/build_audio_context_check.py
tools/bench/out/audio_context_check
```

The builder uses the SDK's compile definitions and headers. `--runtime` selects
another matching runtime directory; `--output` selects the executable path.

The check calls the runtime's decoder commit and native audio setters. It covers
495 consumer read-position cases, ring wraparound, 3,072 setter checks, 3,584 getter
checks, 100,000 concurrent producer/consumer updates and 100,000 PCM publications
through the native getters. Two cancellation checks also verify that work waiting for a
context lock cannot decode after the voice is disabled or released. Decoder writes
must preserve the consumer's newer read position and other independently owned
fields. The previous runtime fails because a decoder update rewinds the read position.

This checks buffer ownership. Live sound recordings and queue diagnostics under
CPU load are also needed to check delivery, mixer timing and device output.
