"""Read and select original map entries without executing Lua or editing game files."""
import re
from pathlib import Path

GAMEFLOW = 'simpsons_gameflow.lua'


def installed_script(game, project=None):
    """Use the effective project override, then the original installed script."""
    for root in ([Path(project) / 'gamedata'] if project else []) + [Path(game)]:
        if not root.is_dir():
            continue
        files = {p.name.casefold(): p for p in root.iterdir() if p.is_file() and not p.is_symlink()}
        path = files.get(GAMEFLOW + '.original') or files.get(GAMEFLOW)
        if path:
            if path.stat().st_size > 2 * 1024 * 1024:
                raise ValueError('The gameflow script exceeds the editor size limit')
            return read_script(path)
    raise ValueError('Install the extracted game files to browse levels')


def level_options(game, names=None, project=None):
    names = names or {}
    return [{**public_entry(entry), 'label': names.get(entry['episode'], entry['episode'].replace('_', ' ').title())
             + ' / ' + entry['name'] + ' / entry ' + str(entry['entry'])}
            for entry in catalog(installed_script(game, project)) if entry['mode'] == 'MODE_STANDARD']

EPISODE = re.compile(r'^[ \t]*episode\s*=\s*NewEpisode\(\s*game\s*,\s*"([A-Z0-9_]+)"', re.M)
MODE = re.compile(r'^[ \t]*mode\s*=\s*NewMode\(', re.M)
DEFAULT = re.compile(r'^[ \t]*episode:SetDefault\(\)[^\r\n]*', re.M)
GUID = re.compile(r'\{[0-9A-Fa-f]{8}(?:-[0-9A-Fa-f]{4}){3}-[0-9A-Fa-f]{12}\}')


def read_script(path):
    return path.read_bytes().decode("utf-8", errors="surrogateescape")


def call_arguments(text, start):
    """Read a builder's simple string/nil arguments without executing Lua."""
    opening = text.index("(", start)
    arguments = []
    begin = opening + 1
    quote = None
    escaped = False
    for i in range(begin, len(text)):
        char = text[i]
        if quote:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == quote:
                quote = None
        elif char in "\"'":
            quote = char
        elif char in ",)":
            value = text[begin:i].strip()
            if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
                arguments.append(value[1:-1])
            elif value == "nil":
                arguments.append(None)
            else:
                arguments.append({"expression": value})
            if char == ")":
                return arguments, i + 1
            begin = i + 1
        elif char == "(":
            raise ValueError("Nested builder arguments require a separate parser")
    raise ValueError("Unterminated Lua builder")


def catalog(text):
    entries = []
    episodes = list(EPISODE.finditer(text))
    install = re.search(r'^[ \t]*gameFlowManager:SetGame\(\s*game\s*\)', text, re.M)
    if not install:
        raise ValueError("Gameflow installation statement was not found")
    for e, episode in enumerate(episodes):
        episode_end = episodes[e + 1].start() if e + 1 < len(episodes) else install.start()
        modes = list(MODE.finditer(text, episode.start(), episode_end))
        for m, mode in enumerate(modes):
            mode_end = modes[m + 1].start() if m + 1 < len(modes) else episode_end
            mode_args, header_end = call_arguments(text, mode.start())
            mode_type = mode_args[1]
            if not isinstance(mode_type, str):
                raise ValueError("Mode type is not a literal string")
            for n, map_call in enumerate(re.finditer(r'\bNewMap\s*\(', text[header_end:mode_end])):
                map_start = header_end + map_call.start()
                arguments, map_end = call_arguments(text, map_start)
                if len(arguments) < 3 or not all(isinstance(a, str) for a in arguments[:3]):
                    raise ValueError("Map name, folder and stream must be literal strings")
                checkpoint = arguments[3] if len(arguments) > 3 else None
                if checkpoint is not None and (not isinstance(checkpoint, str) or not GUID.fullmatch(checkpoint)):
                    raise ValueError("Checkpoint is not a literal GUID")
                line_start = text.rfind("\n", 0, map_start) + 1
                prefix = text[line_start:map_start].strip()
                if prefix == "map =":
                    add = re.search(r'^[ \t]*mode:AddEntry\(\s*map\s*\)[^\r\n]*',
                                    text[map_end:mode_end], re.M)
                    if not add or "NewMap(" in text[map_end:map_end + add.start()]:
                        raise ValueError("Map assignment has no matching AddEntry")
                    entry_end = map_end + add.end()
                elif prefix == "mode:AddEntry(":
                    entry_end = text.find("\n", map_end)
                    if entry_end < 0:
                        entry_end = len(text)
                    if text[map_end:entry_end].strip() != ")":
                        raise ValueError("Unexpected statements after inline map entry")
                else:
                    raise ValueError("Unrecognized NewMap statement: " + prefix)
                setter = re.search(r'^[ \t]*map:SetCheckpoint\(', text[map_end:entry_end], re.M)
                if setter:
                    values, _ = call_arguments(text, map_end + setter.start())
                    if len(values) != 1 or not isinstance(values[0], str) or not GUID.fullmatch(values[0]):
                        raise ValueError("Checkpoint setter must contain one literal GUID")
                    checkpoint = values[0]
                entries.append(dict(
                    id=f"{episode.group(1)}:{mode_type}:{n + 1}",
                    episode=episode.group(1), mode=mode_type, entry=n + 1,
                    name=arguments[0], folder=arguments[1], stream=arguments[2],
                    checkpoint=checkpoint,
                    _episode_start=episode.start(), _episode_end=episode_end,
                    _mode_start=mode.start(), _first_mode=modes[0].start(),
                    _header_end=header_end, _entry_start=line_start, _entry_end=entry_end))
    if not entries:
        raise ValueError("No gameflow map entries found")
    return entries


def select_episode_start(text, entry):
    """Select a fresh episode without removing its movies or mission entries."""
    if entry["mode"] != "MODE_STANDARD" or entry["entry"] != 1:
        raise ValueError("Episode start requires the first standard-mode map")
    defaults = list(DEFAULT.finditer(text))
    if (len(defaults) == 1 and
            entry["_episode_start"] <= defaults[0].start() < entry["_first_mode"]):
        return text
    newline = "\r\n" if "\r\n" in text else "\n"
    result = DEFAULT.sub(lambda match: match.group().replace(
        "episode:SetDefault()", "-- episode:SetDefault()", 1), text)
    target = next(e for e in EPISODE.finditer(result) if e.group(1) == entry["episode"])
    _, header_end = call_arguments(result, target.start())
    line_end = result.find("\n", header_end)
    if line_end < 0:
        raise ValueError("Episode header has no newline")
    result = result[:line_end + 1] + "\tepisode:SetDefault()" + newline + result[line_end + 1:]
    if (len(DEFAULT.findall(result)) != 1 or
            [public_entry(e) for e in catalog(result)] !=
            [public_entry(e) for e in catalog(text)]):
        raise ValueError("Episode selection changed the map inventory")
    return result


def select_entry(text, entry, checkpoint=None):
    if entry["mode"] != "MODE_STANDARD":
        raise ValueError("Timed-mode boot selection has not been qualified; use a standard entry")
    newline = "\r\n" if "\r\n" in text else "\n"
    initial = text[entry["_episode_start"]:entry["_first_mode"]]
    header = text[entry["_mode_start"]:entry["_header_end"]]
    selected = text[entry["_entry_start"]:entry["_entry_end"]]
    if checkpoint is not None:
        if not GUID.fullmatch(checkpoint):
            raise ValueError("Checkpoint override must be a GUID")
        begin = selected.index("NewMap")
        _, end = call_arguments(selected, begin)
        setter = '\t\t\tmap:SetCheckpoint("' + checkpoint + '")'
        if selected[:begin].strip() == "map =":
            selected = selected[:end] + newline + setter + selected[end:]
        else:
            selected = ("\t\t\tmap = " + selected[begin:end] + newline + setter +
                        newline + "\t\t\tmode:AddEntry( map )")
    replacement = initial + header + newline + selected + newline + newline
    result = text[:entry["_episode_start"]] + replacement + text[entry["_episode_end"]:]
    # Keep exactly one default episode. The map entry itself stays verbatim.
    result = DEFAULT.sub(lambda match: match.group().replace(
        "episode:SetDefault()", "-- episode:SetDefault()", 1), result)
    target = next(e for e in EPISODE.finditer(result) if e.group(1) == entry["episode"])
    _, header_end = call_arguments(result, target.start())
    line_end = result.find("\n", header_end)
    if line_end < 0:
        raise ValueError("Episode header has no newline")
    result = result[:line_end + 1] + "\tepisode:SetDefault()" + newline + result[line_end + 1:]
    chosen = [e for e in catalog(result) if e["episode"] == entry["episode"]]
    expected = checkpoint if checkpoint is not None else entry["checkpoint"]
    if len(chosen) != 1 or chosen[0]["checkpoint"] != expected or len(DEFAULT.findall(result)) != 1:
        raise ValueError("Prepared script failed its entry/default validation")
    return result


def public_entry(entry):
    return {key: value for key, value in entry.items() if not key.startswith("_")}

