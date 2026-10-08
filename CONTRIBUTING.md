# Contributing

Contributions are welcome, from bug reports and testing to documentation, fixes and new features.
You do not need to know C++ to help. Please follow the [Code of Conduct](CODE_OF_CONDUCT.md).

## Before you start

- Read the [known issues](README.md#known-issues) and [roadmap](README.md#roadmap).
- Check the open issues and pull requests so work is not duplicated.
- For anything larger than a small fix, open an issue first to discuss the approach.

The goal is a fully native port with accurate rendering, reliable audio and correct game timing
on Linux, Steam Deck and Windows. The renderer is still being completed, and higher frame rates
need to keep physics, scripts, menus and videos running at the right speed.

## Reporting bugs and suggesting features

Use the [issue templates](https://github.com/YesterMester/TheSimpsonsGameRecomp/issues/new/choose).
For a bug, include the release version from the launcher's **About** tab, your operating system,
CPU, GPU and driver, and steps to reproduce it. Say which renderer and render scale you use.
For a regression, include the last version that worked if you know it.

The launcher's **Play** tab shows the game log and can create a diagnostics support bundle.
Include the startup error if the game cannot launch. For graphics problems, a screenshot or short
video helps; for audio or performance problems, include the scene and settings used. Remove
personal information from logs before posting them, and do not upload game data or ISOs.

For a feature, explain what you want to do and how the change would help. Accessibility reports
are welcome; see [ACCESSIBILITY.md](ACCESSIBILITY.md). Report security issues privately using
[SECURITY.md](SECURITY.md).

## Building and making changes

Fork the repository, branch from `main`, and follow the Linux or Windows instructions in
[Building from source](README.md#building-from-source). Build in your own checkout and keep your
game installation and saves backed up when testing changes.

- Follow the style of the code around your change. The SDK includes a `.clang-format`; format
  changed code without reformatting unrelated files.
- To change game behaviour, prefer overriding a recompiled function in `simpsons/src` over
  editing `simpsons/generated`, which the recompiler produces. `simpsons/src/frame_pacing.cpp`
  and `simpsons/src/subtitles.cpp` show how.
- Launcher settings are stored in `simpsons.toml`. If you change a default, add an entry to
  `SETTINGS_DEFAULT_MIGRATIONS` in `launcher/launcher.py` so existing configurations pick it up.
- Keep fixes focused and preserve existing settings, saves and working renderer paths.
- Never include game files, ISOs, extracted assets or anything else derived from the game's
  data. Keep build output, runtime binaries, shader dumps and local test captures out of commits.
- Preserve other contributors' authorship and the notices and licences of code you reuse.

## Checking your change

Run the checks relevant to what you changed, and explain what you could not test.

For launcher changes, run this from the repository root:

```sh
python3 tools/bench/launcher_patch_check.py
```

On Windows, use `python` if that is your Python command. Check the affected launcher flow as
well, including whether saved settings survive a restart.

For game or runtime changes, build and test the affected path with your own game data. Record
the platform, GPU, renderer, settings and scenes tested. For performance claims, compare the same
scene and settings before and after, and report frame times as well as FPS. A single scene does
not establish performance across the campaign.

Rendering changes should preserve the expected image and resource lifetimes. The
[Direct3D 11 checks](tools/bench/d3d11/README.md) and
[native renderer notes](simpsons/re/native_renderer_plan.md) describe the available checks and
coverage. Timing and audio changes also need checks through menus, loading, gameplay, pause and
cutscenes; videos should stay at their original rate.

Documentation-only changes need a check of the text and links.

## Opening a pull request

Open the pull request against `main`. Keep each pull request to one change, link any related issue,
and describe what it fixes or adds, how you tested it and any remaining limits. Use the pull
request template to make the result easy to review.

The **Build check** workflow must pass on both Linux and Windows. Changes covered by the
**Direct3D 11 renderer checks** also need those checks to pass. If you cannot test a platform
yourself, say so; a passing build does not establish that the game works on that platform.

By submitting a pull request, you agree that your contribution is licensed under the licence of
the part of the project it changes (see [License](README.md#license)).
