"""Portable glslc and spirv-cross fixtures for the shader reflection example."""

import hashlib
import json
import os
from pathlib import Path
import re
import signal
import struct
import sys


MAGIC = 0x07230203
REJECT = 'fixture:fail-reflection'


def instruction(opcode, *operands):
    count = 1 + len(operands)
    return struct.pack(f'<{count}I', (count << 16) | opcode, *operands)


def string_words(text):
    text = text.encode('ascii') + b'\0'
    count = (len(text) + 3) // 4
    return struct.unpack(f'<{count}I', text.ljust(4 * count, b'\0'))


def words_string(words):
    return struct.pack(f'<{len(words)}I', *words).split(b'\0', 1)[0].decode('ascii')


def digest(data):
    return hashlib.sha256(data).hexdigest()


def spirv(stage, source):
    """Encode a shader and its descriptor decorations in SPIR-V words."""
    fragment = stage == 'fragment'
    marker = (' ' + REJECT) if REJECT.encode('ascii') in source else ''
    debug = digest(source) + marker
    if fragment:
        layout = re.search(rb'layout\(set\s*=\s*(\d+),\s*binding\s*=\s*(\d+)\)\s*'
                           rb'uniform\s+(\w+)\s*\{[^}]+\}\s*(\w+);', source)
        assert layout, source
        descriptor_set, binding = map(int, layout.groups()[:2])
        block, instance = (name.decode('ascii') for name in layout.groups()[2:])
        stage_info = (
            instruction(16, 1, 7),  # OpExecutionMode OriginUpperLeft
            instruction(7, 5, *string_words(debug)),  # OpString
            instruction(5, 8, *string_words(block)),  # OpName
            instruction(5, 10, *string_words(instance)),
            instruction(6, 8, 0, *string_words('tint')),  # OpMemberName
            instruction(71, 8, 2),  # OpDecorate Block
            instruction(72, 8, 0, 35, 0),  # OpMemberDecorate Offset
            instruction(71, 10, 34, descriptor_set),  # OpDecorate DescriptorSet
            instruction(71, 10, 33, binding),  # OpDecorate Binding
            instruction(71, 16, 30, 0),  # OpDecorate Location
        )
        stage_types = (
            instruction(30, 8, 7),  # OpTypeStruct vec4
            instruction(32, 9, 2, 8),  # OpTypePointer Uniform
            instruction(59, 9, 10, 2),  # OpVariable Uniform
            instruction(21, 11, 32, 0),  # OpTypeInt uint
            instruction(43, 11, 12, 0),  # OpConstant uint 0
            instruction(32, 13, 2, 7),  # OpTypePointer Uniform vec4
        )
        body = (
            instruction(65, 13, 14, 10, 12),  # OpAccessChain material.tint
            instruction(61, 7, 15, 14),  # OpLoad
            instruction(62, 16, 15),  # OpStore color
        )
    else:
        stage_info = (
            instruction(7, 5, *string_words(debug)),  # OpString
            instruction(71, 16, 11, 0),  # OpDecorate BuiltIn Position
        )
        stage_types = (
            instruction(43, 6, 8, 0),  # OpConstant float 0
            instruction(43, 6, 9, 0x3f800000),  # OpConstant float 1
            instruction(44, 7, 10, 8, 8, 8, 9),  # OpConstantComposite vec4
        )
        body = (instruction(62, 16, 10),)  # OpStore Position

    return b''.join((
        struct.pack('<5I', MAGIC, 0x00010000, 0, 18, 0),  # SPIR-V 1.0 header
        instruction(17, 1),  # OpCapability Shader
        instruction(14, 0, 1),  # OpMemoryModel Logical GLSL450
        instruction(15, 4 if fragment else 0, 1, *string_words('main'), 16),  # OpEntryPoint
        *stage_info,
        instruction(19, 2),  # OpTypeVoid
        instruction(33, 3, 2),  # OpTypeFunction
        instruction(22, 6, 32),  # OpTypeFloat
        instruction(23, 7, 6, 4),  # OpTypeVector vec4
        *stage_types,
        instruction(32, 17, 3, 7),  # OpTypePointer Output vec4
        instruction(59, 17, 16, 3),  # OpVariable Output
        instruction(54, 2, 1, 0, 3),  # OpFunction
        instruction(248, 4),  # OpLabel
        *body,
        instruction(253),  # OpReturn
        instruction(56),  # OpFunctionEnd
    ))


def reflect(module):
    """Read descriptor sets and bindings from a staged SPIR-V module."""
    words = struct.unpack(f'<{len(module) // 4}I', module)
    assert words[0] == MAGIC
    cursor = 5
    names = {}
    decorations = {}
    uniform_variables = {}
    pointers = {}
    while cursor < len(words):
        count, opcode = words[cursor] >> 16, words[cursor] & 0xffff
        assert 0 < count <= len(words) - cursor, (opcode, count, cursor)
        operands = words[cursor + 1:cursor + count]
        if opcode == 5:  # OpName
            names[operands[0]] = words_string(operands[1:])
        elif opcode == 7 and REJECT in words_string(operands[1:]):  # OpString
            os.kill(os.getpid(), signal.SIGTERM)
        elif opcode == 71 and operands[1] in (33, 34):  # OpDecorate Binding or DescriptorSet
            target, kind, value = operands
            decorations.setdefault(target, {})[kind] = value
        elif opcode == 32 and operands[1] == 2:  # OpTypePointer Uniform
            pointers[operands[0]] = operands[2]
        elif opcode == 59 and operands[2] == 2:  # OpVariable Uniform
            uniform_variables[operands[1]] = operands[0]
        cursor += count

    types = {}
    ubos = []
    for target, pointer in uniform_variables.items():
        block = pointers[pointer]
        name = names[block]
        descriptor = decorations[target]
        types[f'_{block}'] = {'name': name, 'members': [
            {'name': 'tint', 'type': 'vec4', 'offset': 0},
        ]}
        ubos.append({'type': f'_{block}', 'name': name, 'block_size': 16,
                     'set': descriptor[34], 'binding': descriptor[33]})
    return {'entryPoints': [{'name': 'main', 'mode': 'frag'}], 'types': types, 'ubos': ubos}


def main():
    tool, journal, *args = sys.argv[1:]
    record = {'tool': tool}
    if tool == 'glslc':
        stage_flag, input_path, output_flag, output_path = args
        stages = {'-fshader-stage=vertex': 'vertex', '-fshader-stage=fragment': 'fragment'}
        assert stage_flag in stages and output_flag == '-o', args
        stage = stages[stage_flag]
        source = Path(input_path).read_bytes()
        record.update(stage=stage, source=digest(source))
    elif tool == 'spirv-cross':
        input_path, reflect_flag = args
        assert reflect_flag == '--reflect', args
        module = Path(input_path).read_bytes()
        record.update(path=str(Path(input_path)), module=digest(module))
    with Path(journal).open('a', encoding='utf-8') as log:
        log.write(json.dumps(record) + '\n')

    if tool == 'glslc':
        output = Path(output_path)
        if b'#error' in source:
            output.write_bytes(b'incomplete shader')
            print('shader rejected by fixture', file=sys.stderr)
            return 7
        output.write_bytes(spirv(stage, source))
    elif tool == 'spirv-cross':
        print(json.dumps(reflect(module)))
    else:
        assert tool == 'python3', tool
        os.execv(sys.executable, [sys.executable, *args])
    return 0


if __name__ == '__main__':
    sys.exit(main())
