"""The compilers a run uses: the compiler under test and the oracles (DECISIONS §2.3, §5.4).

Clang 23 and GCC 16 come from $YCXX_CLANGXX and $YCXX_GXX, else from the toolchains.env that
libycxx's tools/toolchain/provision writes, else clang++-23 and g++-16 on PATH. ycxx comes
from $YCXX, else build/bin/ycxx, else build/ycxx.

Each compiler has features, the kinds of work it can do: `syntax` (accept/reject), `preprocess`
(-E), `run` (build and run a program; for ycxx, the interpreter first) and `incremental` (the
daemon's incremental mode, DECISIONS §4.2). The oracles have all of them (their incremental mode
is emulated by a cold run on the edited files). ycxx has what tests/features/ycxx.txt lists, so
that a test needing what ycxx cannot do yet is UNSUPPORTED with that reason; when ycxx learns
to do it, the line is added and the tests start counting.
"""
import os, platform, re, shutil, subprocess, sys
from dataclasses import dataclass, field
from pathlib import Path

FEATURES = ('syntax', 'preprocess', 'run', 'incremental')
ORACLES = ('clang', 'gcc')

# file:line:col: error: ... (GCC and Clang, and the form ycxx's c++-compatible front emits,
# DECISIONS §4.7). Notes and warnings are not errors; WARNING finds the warnings.
DIAG = re.compile(r'^(?P<file>[^:\n]+):(?P<line>\d+):(?P<col>\d+): (?:fatal )?error: ', re.M)
WARNING = re.compile(r'^(?P<file>[^:\n]+):(?P<line>\d+):(?P<col>\d+): warning: ', re.M)


@dataclass
class Compiler:
    name: str
    exe: str | None
    features: set = field(default_factory=set)
    xfail: dict = field(default_factory=dict)  # test id -> reason (ycxx: tests/features/ycxx.xfail)
    version: str = ''
    why_missing: str = ''

    @property
    def available(self):
        return self.exe is not None

    def _diag_flags(self):
        # The oracles in their strictest conformance mode (GCC only warns about some ill-formed
        # code, such as narrowing, without -pedantic-errors), with plain diagnostics.
        return {'gcc': ['-pedantic-errors', '-fdiagnostics-plain-output'],
                'clang': ['-pedantic-errors', '-fno-color-diagnostics']}.get(self.name, [])

    def syntax(self, src, flags=()):
        return [self.exe, '-std=c++26', '-fsyntax-only', *self._diag_flags(), *flags, src]

    def preprocess(self, src, flags=()):
        return [self.exe, '-std=c++26', '-E', *flags, src]

    def build(self, src, out, flags=()):
        # ycxx: the same c++-compatible command line; its interpreter will sit behind it (§4.6).
        return [self.exe, '-std=c++26', *self._diag_flags(), *flags, src, '-o', out]

    def lacks(self, feature):
        """Why this compiler cannot do `feature`, or '' when it can."""
        if feature not in self.features:
            if self.name == 'ycxx':
                return f'ycxx cannot do "{feature}" yet (tests/features/ycxx.txt)'
            return f'{self.name} has no "{feature}"'
        if not self.available:
            return f'{self.name} not found ({self.why_missing})'
        return ''


def errors_in(text, cwd, pattern=DIAG):
    """The error locations (with pattern=WARNING, the warnings') in a compiler's output:
    [(absolute path, line, col)]."""
    out = []
    for m in pattern.finditer(text):
        p = Path(m['file'])
        p = p if p.is_absolute() else Path(cwd) / p
        out.append((os.path.realpath(p), int(m['line']), int(m['col'])))
    return out


def load_toolchains_env():
    """libycxx's toolchains.env, for the variables not already set (as its env.sh does)."""
    base = os.environ.get('YCXX_TOOLCHAINS') or os.path.join(
        os.environ.get('XDG_DATA_HOME') or os.path.expanduser('~/.local/share'), 'ycxx', 'toolchains')
    conf = Path(base) / 'toolchains.env'
    if not conf.is_file():
        return None
    for line in conf.read_text().splitlines():
        k, sep, v = line.partition('=')
        if sep and re.fullmatch(r'YCXX_[A-Z_]+', k) and not os.environ.get(k):
            os.environ[k] = v
    if sys.platform == 'darwin' and not os.environ.get('SDKROOT') and os.environ.get('YCXX_SDKROOT'):
        os.environ['SDKROOT'] = os.environ['YCXX_SDKROOT']
    return conf


def _version(exe):
    try:
        p = subprocess.run([exe, '--version'], capture_output=True, text=True, timeout=20)
        return (p.stdout or p.stderr).splitlines()[0].strip() if (p.stdout or p.stderr) else ''
    except (OSError, subprocess.TimeoutExpired):
        return ''


def _find(var, names):
    if os.environ.get(var):
        exe = os.environ[var]
        return (shutil.which(exe) or (exe if os.access(exe, os.X_OK) else None)), f'${var}={exe} is not executable'
    for n in names:
        if shutil.which(n):
            return shutil.which(n), ''
    return None, f'set ${var}, or provision with deps/libycxx/tools/toolchain/provision'


# The host, as tests/features/ycxx.txt names it: `darwin-arm64`, `linux-x86_64`.
HOST = '{}-{}'.format({'darwin': 'darwin', 'linux': 'linux'}.get(sys.platform, sys.platform),
                      platform.machine().lower().replace('aarch64', 'arm64'))


def ycxx_features(repo, host=HOST):
    """tests/features/ycxx.txt (or $YCXX_FEATURES, for the driver's self-test): one feature per
    line, optionally followed by the hosts it is limited to (`run darwin-arm64`: ycxx builds
    executables only for the target it has)."""
    path = Path(os.environ['YCXX_FEATURES']) if os.environ.get('YCXX_FEATURES') else repo / 'tests' / 'features' / 'ycxx.txt'
    feats = set()
    for line in path.read_text().splitlines() if path.is_file() else ():
        words = line.split('#', 1)[0].split()
        if words:
            if words[0] not in FEATURES:
                raise ValueError(f'{path}: unknown feature "{words[0]}" (known: {", ".join(FEATURES)})')
            if len(words) == 1 or host in words[1:]:
                feats.add(words[0])
    return feats


def ycxx_xfails(repo, host=HOST):
    """Read `<test id>[@<host>] <reason>` from tests/features/ycxx.xfail. A host qualifier
    limits the expected failure to that host using the same names as ycxx.txt."""
    path = repo / 'tests' / 'features' / 'ycxx.xfail'
    out = {}
    for line in path.read_text().splitlines() if path.is_file() else ():
        line = line.split('#', 1)[0].strip()
        if line:
            entry, _, why = line.partition(' ')
            test, qualified, entry_host = entry.partition('@')
            if not qualified or entry_host == host:
                out[test] = why.strip() or '(no reason given)'
    return out


def discover(repo):
    """{name: Compiler} for ycxx, clang and gcc."""
    load_toolchains_env()
    out = {}
    exe, why = _find('YCXX', ())
    if not exe:
        for cand in (repo / 'build' / 'bin' / 'ycxx', repo / 'build' / 'ycxx'):
            if os.access(cand, os.X_OK):
                exe = str(cand)
                break
        else:
            why = 'set $YCXX or build it (build/bin/ycxx)'
    out['ycxx'] = Compiler('ycxx', exe, ycxx_features(repo), ycxx_xfails(repo), why_missing=why)
    for name, var, names in (('clang', 'YCXX_CLANGXX', ('clang++-23',)), ('gcc', 'YCXX_GXX', ('g++-16',))):
        exe, why = _find(var, names)
        out[name] = Compiler(name, exe, set(FEATURES), why_missing=why)
    for c in out.values():
        if c.available:
            c.version = _version(c.exe) or '(no --version)'
    return out
