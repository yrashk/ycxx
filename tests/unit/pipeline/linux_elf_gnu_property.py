"""Exercise x86-64 ELF GNU properties in selected archive members (#1367)."""

import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

FEATURE_1_AND = 0xC0000002
ISA_1_NEEDED = 0xC0008002

ENTRY = """
.text
.globl _start
.type _start,@function
_start:
    endbr64
    call foreign_score_bonus
    xorl %edi, %edi
    cmpl $7, %eax
    setne %dil
    movl $60, %eax
    syscall
.size _start, .-_start
"""

SCORE = """
.text
.globl {symbol}
.type {symbol},@function
{symbol}:
    endbr64
    movl $7, %eax
    ret
.size {symbol}, .-{symbol}
"""


def property_note(value, *, kind=FEATURE_1_AND, desc_size=16, data_size=4):
    return f"""
.section .note.gnu.property,"a",@note
.p2align 3
.long 4, {desc_size}, 5
.asciz "GNU"
.long {kind}, {data_size}, {value}
.long 0
"""


def main():
    ycxx, cc, ar, readelf = sys.argv[1:]
    env = dict(os.environ, LC_ALL='C', YCXX_NO_DAEMON='1', YCXX_THREADS='2')
    env.pop('YCXX_DAEMON', None)
    env.pop('YCXX_ROOT', None)
    with tempfile.TemporaryDirectory(prefix='ycxx-gnu-property-', dir='.') as temp:
        work = Path(temp)
        (work / '.ycxx').touch()

        def invoke(args):
            return subprocess.run(args, cwd=work, env=env, text=True, capture_output=True, timeout=20)

        def checked(args):
            result = invoke(args)
            assert result.returncode == 0, (
                f'{args} exited {result.returncode}\n{result.stdout}{result.stderr}'
            )
            return result.stdout

        def assemble(stem, code, note=''):
            source = f'{stem}.s'
            obj = f'{stem}.o'
            stack_marker = '\n.section .note.GNU-stack,"",@progbits\n'
            (work / source).write_text(code + note + stack_marker, encoding='ascii')
            checked([cc, '-c', '-o', obj, source])
            return obj

        def archive(stem, *members):
            output = f'lib{stem}.a'
            checked([ar, 'rcs', output, *members])
            return output

        entry = assemble('entry', ENTRY)
        marked_entry = assemble('entry_cet', ENTRY, property_note(3))
        score = SCORE.format(symbol='foreign_score_bonus')
        marked_scores = {
            value: assemble(f'score_{value}', score, property_note(value)) for value in (1, 2, 3)
        }
        archives = {value: archive(f'score_{value}', obj) for value, obj in marked_scores.items()}

        # The descriptor claims 24 bytes with only 16 present.
        truncation = property_note(3, desc_size=24)
        # The full descriptor is present with the wrong width for this property type.
        wrong_width = property_note(3, data_size=8)
        isa = property_note(1, kind=ISA_1_NEEDED)
        unknown_flag = property_note(0x80000003)
        unused_malformed = assemble('unused_malformed', SCORE.format(symbol='unused_malformed'), truncation)
        unused_isa = assemble('unused_isa', SCORE.format(symbol='unused_isa'), isa)
        # Place the unreferenced members before the member the entry calls.
        unused = archive('unused', unused_malformed, unused_isa, marked_scores[3])

        cases = [(f'mixed-feature-{value}', entry, member, ()) for value, member in archives.items()]
        cases.append(('unused-malformed-and-isa', entry, unused, ()))
        for stem, note in (
            ('truncated-descriptor', truncation),
            ('wrong-feature-width', wrong_width),
            ('isa-needed', isa),
            ('unknown-feature-bit', unknown_flag),
        ):
            obj = assemble(stem, score, note)
            cases.append((stem, entry, archive(stem, obj), ('gnu property',)))

        # Nonzero intersections require GNU property output support (#1376).
        cases.extend([
            ('all-marked-intersection', marked_entry, archives[1], ('nonzero', 'gnu property')),
            ('all-marked-ibt-and-shstk', marked_entry, archives[3], ('nonzero', 'gnu property')),
        ])

        failures = []
        for name, start, member, diagnostic in cases:
            output = f'{name}.elf'
            link = [ycxx, 'cxx', '--target=x86_64-pc-linux-gnu', '-nostdlib', start, member, '-o', output]
            try:
                if diagnostic:
                    result = invoke(link)
                    report = result.stderr + result.stdout
                    assert result.returncode == 1 and any(
                        all(word in line.lower() for word in diagnostic) for line in report.splitlines()
                    ), f'expected diagnostic mentioning {diagnostic}, link exit {result.returncode}\n{report}'
                    assert not (work / output).exists(), f'failed link wrote {output}'
                else:
                    checked(link)
                    checked([f'./{output}'])
                    report = checked([readelf, '-SWl', output])
                    assert '.note.gnu.property' not in report, report
                    assert not re.search(r'^\s*GNU_PROPERTY\b', report, re.MULTILINE), report
                print(f'{name}: passed')
            except (AssertionError, subprocess.TimeoutExpired) as error:
                failures.append(f'{name}: {error}')

        if failures:
            sys.exit('\n'.join(failures))


if __name__ == '__main__':
    main()
