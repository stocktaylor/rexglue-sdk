#!/usr/bin/env python3
"""Convert the internal SPIR-V shaders to WGSL for the WebGPU backend.

The Vulkan backend's fixed shaders (texture loads, resolves, gamma ramps and so
on) exist here only as generated SPIR-V headers, src/graphics/shaders/
vulkan_spirv/*.h; their sources aren't in this repository. This converts each
one with Tint (the `tint` in the Dawn release cmake/dawn.cmake downloads) into
src/graphics/shaders/webgpu_wgsl/<name>.h, holding `const char <name>_wgsl[]`,
and an index of them all, all_shaders.h (for tests).
On the way it adapts what WebGPU lacks:

- Texel buffers (the gamma ramp shaders read their ramp through one) have no
  WebGPU equivalent, and Tint's SPIR-V reader refuses them. They are only
  fetched from, so the image type becomes 1D before conversion: the backend
  binds a 1D texture with the same texels.
- Push constants come out of Tint as `var<immediate>`, which only Chrome has
  shipped. They become a uniform buffer at binding 0 of the first bind group
  after the ones the shader uses (WebGPU guarantees four, 0 to 3).

Every result is parsed and validated by Tint again. Each header notes what the
backend must do differently from Vulkan for that shader.

    scripts/convert_shaders_wgsl.py            # regenerate
    scripts/convert_shaders_wgsl.py --check    # fail if any output is stale
"""

from __future__ import annotations

import argparse
import glob
import re
import struct
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SPIRV_DIR = ROOT / "src/graphics/shaders/vulkan_spirv"
WGSL_DIR = ROOT / "src/graphics/shaders/webgpu_wgsl"
DAWN_CMAKE = ROOT / "cmake/dawn.cmake"

PUSH_CONSTANT_BINDING = 0
MAX_BIND_GROUPS = 4  # WebGPU's default maxBindGroups

# SPIR-V opcodes and operands touched by the texel buffer patch.
OP_CAPABILITY = 17
OP_TYPE_IMAGE = 25
DIM_1D = 0
DIM_BUFFER = 5
CAPABILITY_SAMPLED_1D = 43
CAPABILITY_SAMPLED_BUFFER = 46

# Storage texture formats WebGPU allows only with texture-formats-tier1.
TIER1_STORAGE_FORMATS = {
    "r8unorm", "r8snorm", "r8uint", "r8sint", "rg8unorm", "rg8snorm", "rg8uint", "rg8sint",
    "r16uint", "r16sint", "r16float", "rg16uint", "rg16sint", "rg16float", "rgb10a2uint",
    "rgb10a2unorm", "rg11b10ufloat",
}

# MSVC caps a single string literal at 16 KB; stay well under per piece.
LITERAL_CHUNK = 4096


@dataclass
class Shader:
    name: str
    spirv: list[int]
    notes: list[str] = field(default_factory=list)


def dawn_revision() -> str:
    match = re.search(r'set\(REXGLUE_DAWN_REVISION "([0-9a-f]+)"\)', DAWN_CMAKE.read_text())
    return match.group(1) if match else "unknown"


def find_tint(explicit: str | None) -> Path:
    if explicit:
        return Path(explicit)
    for candidate in sorted(glob.glob(str(ROOT / "out/deps/Dawn-*/bin/tint*"))):
        if Path(candidate).name in ("tint", "tint.exe"):
            return Path(candidate)
    sys.exit("tint not found: configure with -DREXGLUE_USE_WEBGPU=ON to download Dawn, "
             "or pass --tint")


def read_spirv_header(path: Path) -> list[int]:
    text = path.read_text()
    start = text.index("{", text.index("const uint32_t")) + 1
    end = text.rindex("}")
    return [int(word, 16) for word in re.findall(r"0x[0-9A-Fa-f]+", text[start:end])]


def patch_texel_buffers(shader: Shader) -> None:
    """Turns texel buffer image types (only fetched from) into 1D images."""
    words = shader.spirv
    patched = 0
    i = 5  # past the header
    while i < len(words):
        opcode, count = words[i] & 0xFFFF, words[i] >> 16
        if count == 0:
            raise ValueError(f"{shader.name}: malformed SPIR-V at word {i}")
        if opcode == OP_TYPE_IMAGE and words[i + 3] == DIM_BUFFER:
            words[i + 3] = DIM_1D
            patched += 1
        elif opcode == OP_CAPABILITY and words[i + 1] == CAPABILITY_SAMPLED_BUFFER:
            words[i + 1] = CAPABILITY_SAMPLED_1D
        i += count
    if patched:
        shader.notes.append(
            f"{patched} texel buffer(s) became texture_1d: bind a 1D texture with the "
            "buffer's texels where Vulkan binds the texel buffer view.")


def run_tint(tint: Path, source: Path, output: Path) -> None:
    result = subprocess.run([str(tint), "--format", "wgsl", str(source), "-o", str(output)],
                            capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"tint failed on {source.name}:\n{result.stdout}{result.stderr}")


def adapt_wgsl(shader: Shader, wgsl: str) -> str:
    immediate = re.compile(r"^var<immediate> (\w+) : (\w+);$", re.MULTILINE)
    if immediate.search(wgsl):
        groups = [int(g) for g in re.findall(r"@group\((\d+)u?\)", wgsl)]
        group = max(groups, default=-1) + 1
        if group >= MAX_BIND_GROUPS:
            raise RuntimeError(f"{shader.name}: no bind group left for push constants")
        wgsl = immediate.sub(
            f"@group({group}u) @binding({PUSH_CONSTANT_BINDING}u) "
            r"var<uniform> \1 : \2;", wgsl)
        shader.notes.append(
            f"Push constants are a uniform buffer at @group({group}) "
            f"@binding({PUSH_CONSTANT_BINDING}).")
    for fmt in sorted(set(re.findall(r"texture_storage_\w+<(\w+),", wgsl))):
        if fmt in TIER1_STORAGE_FORMATS:
            shader.notes.append(f"Writes a {fmt} storage texture: needs texture-formats-tier1.")
    return wgsl


def to_header(shader: Shader, wgsl: str, revision: str) -> str:
    pieces: list[str] = []
    current = ""
    for line in wgsl.splitlines(keepends=True):
        if current and len(current) + len(line) > LITERAL_CHUNK:
            pieces.append(current)
            current = ""
        current += line
    if current:
        pieces.append(current)
    lines = [
        "// clang-format off",
        f"// Generated by scripts/convert_shaders_wgsl.py from ../vulkan_spirv/{shader.name}.h",
        f"// with Tint (Dawn {revision[:12]}). Do not edit.",
    ]
    lines += [f"// {note}" for note in shader.notes]
    lines += ["", "#pragma once", "", f"const char {shader.name}_wgsl[] ="]
    lines += [f'    R"wgsl({piece})wgsl"' for piece in pieces]
    lines[-1] += ";"
    lines += ["// clang-format on"]
    return "\n".join(lines) + "\n"


INDEX_NAME = "all_shaders.h"


def index_header(sources: list[Path], tier1: set[str]) -> str:
    lines = [
        "// clang-format off",
        "// Generated by scripts/convert_shaders_wgsl.py. Do not edit.",
        "// Every converted shader, for tests: {name, WGSL, is compute, needs",
        "// texture-formats-tier1}.",
        "",
        "#pragma once",
        "",
    ]
    lines += [f'#include "{source.stem}.h"' for source in sources]
    lines += ["", "struct WgslShaderEntry {", "  const char* name;", "  const char* wgsl;",
              "  bool compute;", "  bool needs_texture_formats_tier1;", "};", "",
              "const WgslShaderEntry kAllWgslShaders[] = {"]
    for s in sources:
        compute = "true" if s.stem.endswith("_cs") else "false"
        needs = "true" if s.stem in tier1 else "false"
        lines.append(f'    {{"{s.stem}", {s.stem}_wgsl, {compute}, {needs}}},')
    lines += ["};", "// clang-format on"]
    return "\n".join(lines) + "\n"


def convert(tint: Path, shader: Shader, revision: str, scratch: Path) -> str:
    patch_texel_buffers(shader)
    spv = scratch / f"{shader.name}.spv"
    spv.write_bytes(struct.pack(f"<{len(shader.spirv)}I", *shader.spirv))
    raw = scratch / f"{shader.name}.tint.wgsl"
    run_tint(tint, spv, raw)
    wgsl = adapt_wgsl(shader, raw.read_text())
    adapted = scratch / f"{shader.name}.wgsl"
    adapted.write_text(wgsl)
    run_tint(tint, adapted, scratch / f"{shader.name}.check.wgsl")  # validates
    return to_header(shader, wgsl, revision)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--tint", help="path to tint (default: the downloaded Dawn's)")
    parser.add_argument("--check", action="store_true",
                        help="don't write; exit 1 if any output differs")
    parser.add_argument("names", nargs="*", help="only these shaders (default: all)")
    args = parser.parse_args()

    tint = find_tint(args.tint)
    revision = dawn_revision()
    sources = sorted(SPIRV_DIR.glob("*.h"))
    if args.names:
        sources = [s for s in sources if s.stem in args.names]
    WGSL_DIR.mkdir(parents=True, exist_ok=True)

    stale: list[str] = []
    tier1: set[str] = set()
    with tempfile.TemporaryDirectory() as scratch:
        for source in sources:
            shader = Shader(source.stem, read_spirv_header(source))
            header = convert(tint, shader, revision, Path(scratch))
            if any("texture-formats-tier1" in note for note in shader.notes):
                tier1.add(shader.name)
            target = WGSL_DIR / f"{shader.name}.h"
            if target.exists() and target.read_text() == header:
                continue
            stale.append(shader.name)
            if not args.check:
                target.write_text(header)

    if args.names:  # the index needs every shader's notes
        print("index not rewritten (only some shaders converted)")
        return 0
    index = index_header(sorted(SPIRV_DIR.glob("*.h")), tier1)
    index_path = WGSL_DIR / INDEX_NAME
    if not (index_path.exists() and index_path.read_text() == index):
        stale.append(INDEX_NAME)
        if not args.check:
            index_path.write_text(index)

    expected = {s.stem for s in sorted(SPIRV_DIR.glob("*.h"))} | {Path(INDEX_NAME).stem}
    orphans = [p.name for p in WGSL_DIR.glob("*.h") if p.stem not in expected]
    if args.check:
        if stale or orphans:
            print("stale:", " ".join(stale + orphans))
            return 1
        print(f"{len(sources)} WGSL shaders up to date")
        return 0
    for orphan in orphans:
        (WGSL_DIR / orphan).unlink()
    print(f"converted {len(sources)}, {len(stale)} changed, {len(orphans)} removed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
