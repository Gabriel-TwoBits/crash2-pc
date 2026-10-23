#!/usr/bin/env python3
"""The macOS build's shaders: a GLSL shader (shaders/) as MSL for SDL_GPU's Metal, which takes no SPIR-V. glslang's
SPIR-V translated by SPIRV-Cross, its resources at their SPIR-V bindings' indices, where SDL_GPU binds them (a vertex
shader's uniform buffers in set 1, a fragment shader's samplers in set 2), then HEADER: the MSL as a C string, NAME_msl.
The MSL stays beside it, HEADER with .metal, for Metal's compiler to check (.github/workflows/macos.yml).

    python tools/msl_shader.py GLSLANG SPIRV_CROSS SHADER NAME HEADER
"""
import subprocess
import sys
from pathlib import Path

ARGUMENTS = 5


def c_string(text):
    lines = text.replace("\\", "\\\\").replace('"', '\\"').splitlines()
    return "\n".join(f'    "{line}\\n"' for line in lines)


def main():
    if len(sys.argv) != ARGUMENTS + 1:
        sys.exit(__doc__)
    else:
        glslang, spirv_cross, shader, name, header = sys.argv[1:]
    header = Path(header)
    spirv, metal = header.with_suffix(".spv"), header.with_suffix(".metal")
    subprocess.run([glslang, "-V", "--quiet", "-o", str(spirv), shader], check=True)
    subprocess.run([spirv_cross, "--msl", "--msl-decoration-binding", "--output", str(metal), str(spirv)], check=True)
    header.write_text(f"static const char {name}_msl[] =\n{c_string(metal.read_text())};\n")


if __name__ == "__main__":
    main()
else:
    # Imported: its functions only
    pass
