#!/usr/bin/env python3
"""Compiles the native replacement shaders into an ahead-of-time shader set.

Each <hash>_<vs|ps>.frag/.vert here replaces the translation of one Xenos
shader. It is compiled with glslangValidator, given the float controls every
translated module declares (DenormFlushToZero, SignedZeroInfNanPreserve and
RoundingModeRTE for 32-bit floats, which GLSL cannot express), and written as
<out>/<hash>_<stage>_<modification>.spv next to the translation's .bind
sidecar, so aot_shader_path serves it instead of the runtime translation.
Pixel shaders that support them (XE_TEXTURES_PLAIN, XE_TEXTURES_LEVEL0) are
also built for the translator's plain and level 0 texture variants, and both,
under the modifications with those bits set.

Translations depend on the draw resolution scale, so the set for each scale
is built separately (XE_RESOLUTION_SCALE), scale 1 in <out_dir> and the others
in <out_dir>/scale<X>x<Y>, where the runtime looks for them. Every set records
the translator version and the render target path it was made for (the native
shaders write host render targets, so they're for the "host" path only).

usage: build_native_shaders.py <out_dir> <modification> [bind_source_dir]

The binding sidecars (<hash>_<stage>.bind, from the translation of each shader)
are next to the sources unless bind_source_dir says otherwise.

<modification> is for shaders that don't name theirs (// xe_modification <hex>).
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tarfile

OP_CAPABILITY = 17
OP_EXECUTION_MODE = 16
OP_ENTRY_POINT = 15
CAP_DENORM_FLUSH_TO_ZERO = 4465
CAP_SIGNED_ZERO_INF_NAN_PRESERVE = 4466
CAP_ROUNDING_MODE_RTE = 4467
MODE_DENORM_FLUSH_TO_ZERO = 4460
MODE_SIGNED_ZERO_INF_NAN_PRESERVE = 4461
MODE_ROUNDING_MODE_RTE = 4462
FLOAT_CONTROL_CAPABILITIES = (CAP_DENORM_FLUSH_TO_ZERO, CAP_SIGNED_ZERO_INF_NAN_PRESERVE,
                              CAP_ROUNDING_MODE_RTE)
FLOAT_CONTROL_MODES = (MODE_DENORM_FLUSH_TO_ZERO, MODE_SIGNED_ZERO_INF_NAN_PRESERVE,
                       MODE_ROUNDING_MODE_RTE)
# SpirvShaderTranslator::Modification::pixel.textures_plain (word 1, bit 17).
MODIFICATION_TEXTURES_PLAIN = 1 << 49
# SpirvShaderTranslator::Modification::pixel.textures_level0 (word 1, bit 18).
MODIFICATION_TEXTURES_LEVEL0 = 1 << 50


def add_float_controls(words):
    header, body = words[:5], words[5:]
    out, i = [], 0
    entry_id = None
    capabilities_done = modes_done = False
    instructions = []
    while i < len(body):
        n = body[i] >> 16
        instructions.append(body[i:i + n])
        i += n
    for inst in instructions:
        op = inst[0] & 0xFFFF
        if op == OP_ENTRY_POINT:
            entry_id = inst[2]
        # Capabilities go with the other capabilities, execution modes right
        # before the first existing one (both sections come in this order).
        if op != OP_CAPABILITY and not capabilities_done:
            for cap in FLOAT_CONTROL_CAPABILITIES:
                out.append([(2 << 16) | OP_CAPABILITY, cap])
            capabilities_done = True
        if op == OP_EXECUTION_MODE and not modes_done:
            for mode in FLOAT_CONTROL_MODES:
                out.append([(4 << 16) | OP_EXECUTION_MODE, entry_id, mode, 32])
            modes_done = True
        out.append(inst)
    assert capabilities_done and modes_done, "no execution mode section"
    return header + [w for inst in out for w in inst]


# Draw resolution scales to build sets for (every scale the launcher offers).
# The modules must compute 1/scale exactly like the translator's float
# constant (1.0f / float(scale)): glslang folds 1.0 / float(scale) to the same
# nearest float, and the coordinate nudge 0.00146484375 / scale is exact for
# 1, 2 and 3 (3/2048 * float(1/3) rounds to 1/2048), so 3x matches too.
RESOLUTION_SCALES = (1, 2, 3)


def build_set(set_dir, modification, bind_dir, scale):
    os.makedirs(set_dir, exist_ok=True)
    translator = (Path(__file__).resolve().parents[2] / "rexglue-sdk/include/rex/graphics/pipeline/shader/spirv_translator.h").read_text()
    version = re.search(r"kTranslatedModuleVersion\s*=\s*(\d+)", translator)
    if not version:
        raise ValueError("Could not read the translated module version")
    with open(os.path.join(set_dir, "translator_version.txt"), "w") as f:
        f.write(version.group(1) + "\n")
    with open(os.path.join(set_dir, "render_target_path.txt"), "w") as f:
        f.write("host\n")
    with open(os.path.join(set_dir, "spirv_version.txt"), "w") as f:
        f.write("00010500\n")
    here = os.path.dirname(os.path.abspath(__file__))
    for name in sorted(os.listdir(here)):
        base, ext = os.path.splitext(name)
        if ext not in (".frag", ".vert"):
            continue
        shader_hash, stage = base.split("_")
        source = os.path.join(here, name)
        text = open(source).read()
        if scale != 1 and "XE_RESOLUTION_SCALE" not in text:
            # Written for scale 1 only.
            continue
        scale_defines = [f"-DXE_RESOLUTION_SCALE={scale}"] if scale != 1 else []
        # A shader may name the modification it's for (// xe_modification <hex>).
        shader_modification = int(modification, 16)
        for line in text.splitlines():
            if line.startswith("// xe_modification "):
                shader_modification = int(line.split()[2], 16)
        variants = [(shader_modification, scale_defines)]
        if stage == "ps" and "XE_TEXTURES_PLAIN" in text:
            variants.append((shader_modification | MODIFICATION_TEXTURES_PLAIN,
                             scale_defines + ["-DXE_TEXTURES_PLAIN=1"]))
        if stage == "ps" and "XE_TEXTURES_LEVEL0" in text:
            variants += [(modification_value | MODIFICATION_TEXTURES_LEVEL0,
                          defines + ["-DXE_TEXTURES_LEVEL0=1"])
                         for modification_value, defines in list(variants)]
        for variant_modification, defines in variants:
            spv = os.path.join(set_dir, f"{shader_hash}_{stage}_{variant_modification:016X}.spv")
            subprocess.run(["glslangValidator", "-V", "--target-env", "vulkan1.2", *defines,
                            "-o", spv, source], check=True, stdout=subprocess.DEVNULL)
            data = open(spv, "rb").read()
            words = list(struct.unpack(f"<{len(data) // 4}I", data))
            words = add_float_controls(words)
            open(spv, "wb").write(struct.pack(f"<{len(words)}I", *words))
            print("built", spv)
        if bind_dir:
            shutil.copy(os.path.join(bind_dir, f"{shader_hash}_{stage}.bind"), set_dir)


def write_warmup_metadata(out_dir, request_archive=None):
    # Ship the same first-run recipes on desktop and Android. These identities
    # request a translation for the current GPU; they never bless an old module
    # with another translator's version or source hash.
    output = Path(out_dir)
    modifications = set()
    if request_archive:
        # Mobile needs the variant identities, not another GPU's compiled
        # translations. Read names only, then rebuild from the owner's code
        # with the device's current translator and capabilities on first run.
        with tarfile.open(request_archive) as archive:
            for member in archive:
                match = re.fullmatch(r"([0-9A-Fa-f]{16})_(vs|ps)_([0-9A-Fa-f]{16})\.spv", Path(member.name).name)
                if member.isfile() and match:
                    modifications.add(f"{match[1].upper()} {match[2]} {match[3].upper()}")
        if not modifications:
            raise ValueError("The requested archive has no shader warmup identities")
    for file in output.rglob("*.spv"):
        match = re.fullmatch(r"([0-9A-Fa-f]{16})_(vs|ps)_([0-9A-Fa-f]{16})\.spv", file.name)
        if match:
            modifications.add(f"{match[1].upper()} {match[2]} {match[3].upper()}")
    if not modifications:
        raise ValueError("The shader set has no warmup variants")
    (output / "warmup-modifications.txt").write_text(
        "# Shader hash, stage and modification; rebuilt for the current GPU.\n" +
        "\n".join(sorted(modifications)) + "\n", encoding="utf-8")
    recipes = Path(__file__).resolve().parents[1]
    for name in ("warmup-pipelines.txt", "warmup-vertex-patches.txt"):
        if (recipes / name).resolve() != (output / name).resolve():
            shutil.copy2(recipes / name, output / name)
    return len(modifications)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("out_dir")
    parser.add_argument("modification")
    parser.add_argument("bind_dir", nargs="?", default=os.path.dirname(os.path.abspath(__file__)))
    parser.add_argument("--warmup-archive", help="Read variant identities without copying compiled translations")
    args = parser.parse_args()
    out_dir, modification, bind_dir = args.out_dir, args.modification, args.bind_dir
    for scale in RESOLUTION_SCALES:
        set_dir = out_dir if scale == 1 else os.path.join(out_dir, f"scale{scale}x{scale}")
        build_set(set_dir, modification, bind_dir, scale)
    print("staged", write_warmup_metadata(out_dir, args.warmup_archive), "shader warmup variants")


if __name__ == "__main__":
    main()
