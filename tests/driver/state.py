"""The state fail-fast CI needs (DECISIONS §9.6), kept between runs in build/test-state/:

  durations.json            {compiler: {test id: seconds of its last run}}
  failures/<branch>.json    {compiler: {"failing": [test ids], "commit": ..., "updated": ...}}
  last-run.json             this run's results alone, for merge()

A run updates only the tests it ran: a test stays failing until it passes, whichever subset,
shard or stage it runs in. A branch without its own record starts from main's.

Shards of one run must agree on the durations they partition by, so CI restores the same state
into every shard and merges the shards' states afterwards (merge()).
"""
import json, os, re, subprocess, time
from pathlib import Path

DEFAULT_SECS = 1.0  # a test never timed


def _read(p):
    try:
        return json.loads(Path(p).read_text())
    except (OSError, ValueError):
        return {}


def _write(p, data):
    p = Path(p)
    p.parent.mkdir(parents=True, exist_ok=True)
    tmp = p.with_suffix('.tmp')
    tmp.write_text(json.dumps(data, indent=1, sort_keys=True) + '\n')
    os.replace(tmp, p)


def branch_file(state_dir, branch):
    return Path(state_dir) / 'failures' / (re.sub(r'[^A-Za-z0-9._-]+', '_', branch) + '.json')


class State:
    def __init__(self, state_dir, branch, cxx):
        self.dir, self.branch, self.cxx = Path(state_dir), branch, cxx
        self.durations = _read(self.dir / 'durations.json')
        own = _read(branch_file(state_dir, branch)).get(cxx)
        self.inherited = own is None and branch != 'main'
        rec = own if own is not None else _read(branch_file(state_dir, 'main')).get(cxx, {})
        self.failing = set(rec.get('failing', []))

    def secs(self, test_id):
        return self.durations.get(self.cxx, {}).get(test_id, DEFAULT_SECS)

    def update(self, results, commit, last_run=True):
        """results: {test id: (code, seconds)}. Bad codes become failing, PASS and XFAIL clear;
        UNSUPPORTED says nothing (a missing oracle must not forget a failure)."""
        d = self.durations.setdefault(self.cxx, {})
        for tid, (code, secs) in results.items():
            if code != 'UNSUPPORTED':
                d[tid] = round(secs, 3)
            if code in ('PASS', 'XFAIL'):
                self.failing.discard(tid)
            elif code != 'UNSUPPORTED':
                self.failing.add(tid)
        _write(self.dir / 'durations.json', self.durations)
        bf = branch_file(self.dir, self.branch)
        data = _read(bf)
        data[self.cxx] = {'failing': sorted(self.failing), 'commit': commit,
                          'updated': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}
        _write(bf, data)
        if last_run:
            _write(self.dir / 'last-run.json', {'branch': self.branch, 'cxx': self.cxx, 'commit': commit,
                                                'results': {k: list(v) for k, v in results.items()}})


def merge(target, sources):
    """Merges the states of a run's shards into target, which holds the state they all started
    from: each shard's own results (its last-run.json) are applied as its run applied them."""
    for s in sources:
        last = _read(Path(s) / 'last-run.json')
        if last:
            State(target, last['branch'], last['cxx']).update(
                {tid: tuple(v) for tid, v in last['results'].items()}, last['commit'], last_run=False)


def shard(tests, secs, index, count, loads=None):
    """The index-th (1-based) of count shards: longest-processing-time-first over the recorded
    durations, ties broken by id, so every shard computes the same partition. loads (one per
    shard, updated) carries the balance over from earlier stages."""
    loads = [0.0] * count if loads is None else loads
    mine = []
    for t in sorted(tests, key=lambda t: (-secs(t.id), t.id)):
        k = min(range(count), key=lambda i: (loads[i], i))
        loads[k] += secs(t.id)
        if k == index - 1:
            mine.append(t)
    return mine, loads[index - 1]


# --- tests affected by a change ---------------------------------------------------------------

def git(repo, *args):
    p = subprocess.run(['git', '-C', str(repo), *args], capture_output=True, text=True)
    return p.stdout.strip() if p.returncode == 0 else None


def changed_files(repo, base):
    """Files changed since base (a revision; default: the merge-base with origin/main), committed
    or not. None when git cannot tell."""
    if not base:
        base = git(repo, 'merge-base', 'HEAD', 'origin/main') or git(repo, 'merge-base', 'HEAD', 'main')
    if not base:
        return None
    committed = git(repo, 'diff', '--name-only', f'{base}...HEAD')
    local = git(repo, 'status', '--porcelain', '--untracked-files=all')
    if committed is None or local is None:
        return None
    files = set(committed.splitlines())
    files |= {l[3:].split(' -> ')[-1] for l in local.splitlines() if l[3:]}
    return sorted(files)


def affected(changed, tests):
    """The hook selecting the tests a change can affect: (tests, why).

    Today: a changed test (or its .stdout) selects itself; a change to the driver or to the
    features file selects every test; documentation selects nothing. Changes to the compiler
    select nothing here (the core stage runs the core suites anyway) until dependency
    information exists: first coarse source-area-to-suite maps, then ycxx's own query engine
    answering which tests read a changed input (DECISIONS §9.2.3)."""
    if changed is None:
        return [], 'git cannot tell what changed'
    by_path = {f'tests/{t.id}': t for t in tests}
    picked, other = {}, []
    for f in changed:
        if f.startswith('tests/driver/') or f in ('tools/test', 'tests/features/ycxx.txt'):
            return list(tests), f'the driver or ycxx\'s features changed ({f})'
        t = by_path.get(f) or by_path.get(f.removesuffix('.stdout'))
        if t:
            picked[t.id] = t
        elif not (f.endswith('.md') or f.startswith('docs/')):
            other.append(f)
    why = f'{len(picked)} changed test(s)'
    if other:
        why += f'; {len(other)} other changed file(s) without dependency information yet'
    return list(picked.values()), why
