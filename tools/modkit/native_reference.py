"""Extract unchanged game decoders and their verified register-save helpers."""
import argparse
import hashlib
from pathlib import Path
import re


def extract(source, name, replacement, allowed=(), *, allow_indirect=False):
    text = source.read_text(encoding="utf-8")
    match = re.search(r"^DEFINE_REX_FUNC\(" + name + r"\) \{", text, re.MULTILINE)
    if not match:
        raise ValueError("The verified routine is missing from the generated game: " + name)
    tail = re.search(r"^DEFINE_REX_FUNC\(", text[match.end():], re.MULTILINE)
    if not tail:
        raise ValueError("Cannot locate the end of the game's routine: " + name)
    body = text[match.start():match.end() + tail.start()].strip()
    calls = set(re.findall(r"\b((?:sub_|__imp__|__save|__rest)[A-Za-z_0-9]*)\(ctx, base\)", body))
    if calls - set(allowed):
        raise ValueError("The reference gained external calls; review its dependencies before building")
    if ("REX_CALL_" in body and not allow_indirect) or "REX_MM_" in body:
        raise ValueError("The reference is no longer a standalone memory routine")
    digest = hashlib.sha256(body.encode("utf-8")).hexdigest()
    body = body.replace(match.group(), f"static void {replacement}(PPCContext& ctx, uint8_t* base) {{")
    # Keep the generated operations; spell the rotates with the standard C++
    # intrinsic so the creator tool also builds with non-Clang host compilers.
    body = body.replace("__builtin_rotateleft64(", "std::rotl<uint64_t>(")
    body = body.replace("__builtin_rotateleft32(", "std::rotl<uint32_t>(")
    return digest, body


def generate(source, output):
    routines = [
        (source, "sub_8281E5B0", "GameRefPackDecode", "kGameRefPackSource", ()),
        (source, "sub_8281E8C0", "GameRefPackReadSize", "kGameRefPackSizeSource", ()),
        (source.with_name("simpsons_recomp.65.cpp"), "__savegprlr_29", "__savegprlr_29",
         "kGameRefPackSaveSource", ()),
        (source.with_name("simpsons_recomp.65.cpp"), "__restgprlr_29", "__restgprlr_29",
         "kGameRefPackRestoreSource", ()),
        (source, "sub_8281E9B0", "GameRefPackStep", "kGameRefPackStepSource",
         ("__savegprlr_29", "__restgprlr_29")),
    ]
    text = "// Extracted from the repository's generated game routines.\n"
    for path, name, replacement, identity, allowed in routines:
        digest, body = extract(path, name, replacement, allowed)
        text += f'inline constexpr char {identity}[] = "{digest}";\n{body}\n'
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(text, encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    generate(args.source, args.output)
