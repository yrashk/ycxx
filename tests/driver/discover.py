"""Finding tests, their kinds, identities and directives (docs/TESTING.md, "Writing tests").

A test's identity is its path under tests/ (`own/smoke/ok.accept.cpp`). It is what the state
(durations, last failures), sharding, selection and reports key on, so it is stable as long as
the file is not moved.
"""
import re
from dataclasses import dataclass, field
from pathlib import Path

KINDS = {  # file suffix -> kind
    '.accept.cpp': 'accept', '.reject.cpp': 'reject', '.pp.cpp': 'preprocess',
    '.run.cpp': 'run', '.diff.cpp': 'diff', '.incr': 'incremental',
}
# The suites (top-level directories of tests/) and the stage of the fail-fast pyramid each
# belongs to (DECISIONS §9.2): the core oracles, then the wider suites. Others: "rest".
SUITE_STAGE = {'draft': 'core', 'own': 'core', 'diff': 'wider', 'incremental': 'wider'}
NOT_SUITES = {'driver', 'features', 'bootstrap'}  # bootstrap: #4's toolchain smoke test, run by tools/build

DIRECTIVE = re.compile(r'^\s*//\s*(REQUIRES|XFAIL|FLAGS|TIMEOUT|EXIT|STDOUT|PP-COMPARE|ORACLE|DIFF|ACTION|MAIN|TIER|PEER|PEER-WITH):(.*)$')
# An expected error: `//! error` on its line, `//! error@+1` / `@-2` relative, `@12` absolute,
# and an optional column, `//! error@+1:5`. `//! warning` expects a warning the same way.
MARKER = re.compile(r'//!\s*(error|warning)(?:@([+-]?\d+))?(?::(\d+))?')


@dataclass
class Test:
    id: str
    path: Path
    suite: str
    kind: str
    directives: dict = field(default_factory=dict)  # key -> [values]

    @property
    def stage(self):
        return SUITE_STAGE.get(self.suite, 'rest')

    def one(self, key, default=None):
        v = self.directives.get(key)
        return v[-1] if v else default


def kind_of(path):
    for suffix, kind in KINDS.items():
        if path.name.endswith(suffix):
            return kind
    return None


def directives_of(text, incremental=False):
    out = {}
    for line in text.splitlines():
        if incremental and (line.startswith('--- ') or line.startswith('=== ')):
            break  # an incremental test's directives are in its header
        m = DIRECTIVE.match(line)
        if m:
            out.setdefault(m[1], []).append(m[2].strip())
    return out


def markers_in(text, severity='error'):
    """[(line, col or None)] of the errors (or warnings) a file's `//!` markers expect."""
    out = []
    for n, line in enumerate(text.splitlines(), 1):
        for m in MARKER.finditer(line):
            if m[1] != severity:
                continue
            at = m[2]
            target = n if at is None else (n + int(at) if at[0] in '+-' else int(at))
            out.append((target, int(m[3]) if m[3] else None))
    return out


def discover(tests_dir):
    out = []
    for p in sorted(tests_dir.rglob('*')):
        rel = p.relative_to(tests_dir)
        if not p.is_file() or rel.parts[0] in NOT_SUITES or len(rel.parts) < 2:
            continue
        kind = kind_of(p)
        if not kind:
            continue
        text = p.read_text(encoding='utf-8', errors='replace')
        out.append(Test(rel.as_posix(), p, rel.parts[0], kind, directives_of(text, kind == 'incremental')))
    return out
