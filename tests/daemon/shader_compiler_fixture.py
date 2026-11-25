"""A deterministic stand-in for glslc with the example's command line."""

import hashlib
from pathlib import Path
import struct
import sys


def instruction(opcode, *operands):
    words = 1 + len(operands)
    return struct.pack(f'<{words}I', (words << 16) | opcode, *operands)


def spirv(stage, source):
    """Return a minimal SPIR-V module with a debug string derived from the source."""
    fragment = stage == 'fragment'
    digest = hashlib.sha256(source).hexdigest().encode('ascii') + b'\0' * 4
    return b''.join((
        struct.pack('<5I', 0x07230203, 0x00010000, 0, 6, 0),  # SPIR-V 1.0 header
        instruction(17, 1),  # OpCapability Shader
        instruction(14, 0, 1),  # OpMemoryModel Logical GLSL450
        instruction(15, 4 if fragment else 0, 1, 0x6e69616d, 0),  # OpEntryPoint "main"
        instruction(16, 1, 7) if fragment else b'',  # OpExecutionMode OriginUpperLeft
        instruction(7, 5, *struct.unpack('<17I', digest)),  # OpString
        instruction(19, 2),  # OpTypeVoid
        instruction(33, 3, 2),  # OpTypeFunction
        instruction(54, 2, 1, 0, 3),  # OpFunction
        instruction(248, 4),  # OpLabel
        instruction(253),  # OpReturn
        instruction(56),  # OpFunctionEnd
    ))


def main():
    stage_flag, input_path, output_flag, output_path = sys.argv[1:]
    stages = {'-fshader-stage=vertex': 'vertex', '-fshader-stage=fragment': 'fragment'}
    assert stage_flag in stages and output_flag == '-o', sys.argv
    source = Path(input_path).read_bytes()
    output = Path(output_path)
    if b'#error' in source:
        output.write_bytes(b'incomplete shader')
        print('shader rejected by fixture', file=sys.stderr)
        return 7
    output.write_bytes(spirv(stages[stage_flag], source))
    return 0


if __name__ == '__main__':
    sys.exit(main())
