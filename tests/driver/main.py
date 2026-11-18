"""tools/test: ycxx's single test driver (DECISIONS §5.5, §9). Usage: tools/test --help."""
import argparse, concurrent.futures as cf, os, platform, re, shlex, shutil, subprocess, sys, time
import traceback
from pathlib import Path

from . import discover, kinds, report, state, toolchain, transcript

REPO = Path(__file__).resolve().parents[2]
TESTS = REPO / 'tests'
STAGES = ('policy', 'build', 'failed', 'affected', 'core', 'wider', 'rest')
TEST_STAGES = ('failed', 'affected', 'core', 'wider', 'rest')
TIERS = {
    'fast': ('policy', 'build', 'failed', 'affected', 'core'),
    'pr': ('policy', 'build', 'failed', 'affected', 'core', 'wider'),
    'main': STAGES,
    'nightly': STAGES,
}
GOOD = {'PASS', 'XFAIL'}
EXIT_OK, EXIT_FAIL, EXIT_USAGE, EXIT_INTERNAL, EXIT_INTERRUPTED = 0, 1, 2, 3, 130

HELP = """\
ycxx's test driver: runs the stages of the fail-fast pyramid (DECISIONS §9.2), shows each
command and live progress, lists every test as it finishes with the steps it ran, and writes
build/test-logs/report.html (every test with its transcript, passing ones too; copy buttons for
a person or an agent), report.md, report.tsv and report.json. docs/TESTING.md has the details.

  tools/test                     every stage, without stopping between them
  tools/test --fast              what a session runs before pushing (= the PR tier's first stages:
                                 policy build failed affected core), stopping at the first failing stage
  tools/test --tier pr           a CI tier, stopping at the first failing stage
  tools/test core own/smoke      stages and path filters, in any order

stages, in order:
  policy    the driver's self-test, then tools/check-policy if it exists
  build     tools/build if it exists (#4)
  failed    tests that failed in the last run of this branch (of main for a new branch)
  affected  tests affected by the change since --base
  core      the core oracles: tests/draft and tests/own
  wider     differential and incremental suites: tests/diff, tests/incremental
  rest      every other suite
A test runs in the first stage that selects it.

exit status: 0 all passed (XFAIL and UNSUPPORTED included); 1 a test or stage failed; 2 usage or
configuration error; 3 internal driver error; 130 interrupted (Ctrl-C).
"""


def parse_args(argv):
    p = argparse.ArgumentParser(prog='tools/test', description=HELP, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('what', nargs='*', help='stages and test path filters (prefixes of test ids or paths)')
    p.add_argument('--fast', action='store_true', help='the PR tier\'s first stages, stopping at the first failure')
    p.add_argument('--tier', choices=('pr', 'main', 'nightly'), help='a CI tier (stops at the first failing stage)')
    p.add_argument('-x', '--fail-fast', action='store_true', help='stop after the first stage with a failure')
    p.add_argument('--failed-first', action='store_true', help='add the failed stage if the stages chosen lack it')
    p.add_argument('--only-failed', action='store_true', help='run only the tests that failed last time')
    p.add_argument('--cxx', default=os.environ.get('YCXX_TEST_CXX', 'ycxx'), choices=('ycxx', 'clang', 'gcc'),
                   help='the compiler under test (default ycxx; clang and gcc check the tests themselves)')
    p.add_argument('--oracle', default='clang', choices=toolchain.ORACLES, help='default oracle of -E tests')
    p.add_argument('--require-oracles', action='store_true', help='a missing oracle is an error, not UNSUPPORTED')
    p.add_argument('--shard', help='I/N: run the I-th of N shards, split by recorded duration')
    p.add_argument('--state', default=str(REPO / 'build' / 'test-state'), help='fail-fast state directory')
    p.add_argument('--branch', help='branch whose last failures to use and update (default: from CI or git)')
    p.add_argument('--base', help='revision the affected stage compares with (default: merge-base with origin/main)')
    p.add_argument('--no-state', action='store_true', help='do not update the state')
    p.add_argument('--merge-state', nargs='+', metavar='DIR', help='merge shard state DIRs into --state, then exit')
    p.add_argument('--list', action='store_true', help='list the selected tests by stage, with durations, and exit')
    p.add_argument('-j', '--jobs', type=int, default=os.cpu_count() or 4)
    p.add_argument('--timeout', type=int, default=60, help='seconds per command (TIMEOUT: overrides)')
    p.add_argument('-v', '--verbose', action='store_true', help='print every test\'s transcript')
    p.add_argument('-q', '--quiet', action='store_true', help='list only tests that did not pass')
    return p.parse_args(argv)


# --- terminal ---------------------------------------------------------------------------------

class UI:
    def __init__(self):
        mode = os.environ.get('YCXX_COLOR', 'auto')
        on = mode == 'always' or (mode == 'auto' and not os.environ.get('NO_COLOR') and (
            sys.stdout.isatty() or os.environ.get('GITHUB_ACTIONS') == 'true' or os.environ.get('FORCE_COLOR')))
        self.tty = sys.stdout.isatty() and os.environ.get('TERM', 'dumb') != 'dumb'
        self.gha = os.environ.get('GITHUB_ACTIONS') == 'true'
        c = lambda code: f'\033[{code}m' if on else ''
        self.red, self.green, self.yellow, self.blue = c(31), c(32), c(33), c(34)
        self.magenta, self.cyan, self.bold, self.dim, self.reset = c(35), c(36), c(1), c(2), c(0)
        utf = 'utf' in (sys.stdout.encoding or '').lower()
        self.sym = dict(ok='✔', fail='✘', skip='⊘', run='▶', info='•') if utf else \
            dict(ok='+', fail='x', skip='-', run='>', info='*')
        self.status_shown = False

    def clear(self):
        if self.status_shown:
            sys.stdout.write('\r\033[K')
            self.status_shown = False

    def line(self, text=''):
        self.clear()
        print(text, flush=True)

    def status(self, text):
        if self.tty:
            cols = shutil.get_terminal_size((100, 20)).columns
            sys.stdout.write('\r\033[K' + text[:cols - 1])
            sys.stdout.flush()
            self.status_shown = True

    def title(self, t): self.line(f'\n{self.bold}{self.magenta}== {t} =={self.reset}')
    def section(self, t): self.line(f'\n{self.bold}{self.blue}{self.sym["run"]} {t}{self.reset}')
    def info(self, k, v=''): self.line(f'  {self.cyan}{self.sym["info"]}{self.reset} {k}' + (f' {self.dim}{v}{self.reset}' if v else ''))
    def ok(self, k, v=''): self.line(f'  {self.green}{self.sym["ok"]}{self.reset} {k}' + (f' {self.dim}{v}{self.reset}' if v else ''))
    def skip(self, k, v=''): self.line(f'  {self.yellow}{self.sym["skip"]} {k}{self.reset}' + (f' {self.dim}{v}{self.reset}' if v else ''))
    def fail(self, k, v=''): self.line(f'  {self.red}{self.bold}{self.sym["fail"]} {k}{self.reset}' + (f' {self.red}{v}{self.reset}' if v else ''))
    def warn(self, k, v=''): self.line(f'  {self.yellow}{self.bold}! {k}{self.reset}' + (f' {self.yellow}{v}{self.reset}' if v else ''))

    def block(self, title, text):
        if self.gha:
            self.line(f'::group::{title}')
            self.line(text.rstrip())
            self.line('::endgroup::')
        else:
            for l in text.rstrip().splitlines():
                self.line(f'    {self.dim}│{self.reset} {l}')


def dur(s):
    s = int(s)
    return f'{s // 60}m{s % 60:02d}s' if s >= 60 else f'{s}s'


# --- stages -----------------------------------------------------------------------------------

def command_stage(ui, name, label, cmd, logs, records):
    """Runs one command of a policy or build stage with its output in build/test-logs/<name>.log."""
    log = logs / f'{name}.log'
    ui.line(f'  {ui.dim}$ {shlex.join(cmd)}{ui.reset}')
    t0 = time.monotonic()
    with open(log, 'w') as f:
        proc = subprocess.Popen(cmd, cwd=REPO, stdout=f, stderr=subprocess.STDOUT)
        spin = '⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏'
        i = 0
        while proc.poll() is None:
            ui.status(f'  {spin[i % len(spin)]} {label} ({dur(time.monotonic() - t0)})')
            i += 1
            time.sleep(0.2)
    secs = time.monotonic() - t0
    if proc.returncode == 0:
        ui.ok(label, f'({dur(secs)})')
        records.append({'name': label, 'status': 'ok', 'secs': secs, 'detail': shlex.join(cmd)})
        return True
    ui.fail(label, f'(exit {proc.returncode}, {dur(secs)})')
    text = log.read_text(errors='replace')
    ui.block(f'{label}: log', '\n'.join(text.splitlines()[-40:]) if not ui.gha else text)
    ui.line(f'    {ui.dim}full log: {log}{ui.reset}')
    records.append({'name': label, 'status': 'FAIL', 'secs': secs, 'detail': f'{shlex.join(cmd)} (log: {log})',
                    'log': text[-20000:]})
    return False


def policy_stage(ui, logs, records):
    ok = command_stage(ui, 'policy-driver-selftest', 'driver self-test (tests/driver/selftest.py)',
                       [sys.executable, '-m', 'unittest', '-q', 'tests.driver.selftest'], logs, records)
    script = REPO / 'tools' / 'check-policy'
    if script.exists():
        ok = command_stage(ui, 'policy', 'policy checks (tools/check-policy)', [str(script)], logs, records) and ok
    else:
        ui.skip('policy checks', '(no tools/check-policy yet: #4)')
        records.append({'name': 'policy checks', 'status': 'skip', 'secs': 0, 'detail': 'no tools/check-policy yet'})
    return ok


def build_stage(ui, logs, records, jobs):
    script = REPO / 'tools' / 'build'
    if not script.exists():
        ui.skip('build', '(no tools/build yet: #4)')
        records.append({'name': 'build', 'status': 'skip', 'secs': 0, 'detail': 'no tools/build yet'})
        return True
    return command_stage(ui, 'build', 'build (tools/build)', [str(script), '-j', str(jobs)], logs, records)


# --- running tests ----------------------------------------------------------------------------

def summary_error(code, output):
    for prefix in ('FAIL: ', 'UNRESOLVED: ', 'TIMEOUT: ', 'unsupported: ', 'XPASS: ', 'expected to fail: '):
        for l in reversed(output.splitlines()):
            if l.startswith(prefix):
                return l[len(prefix):][:200]
    return ''


def run_tests(ui, args, stage, tests, ctx, results, shown_failures, interrupted):
    """Runs tests in parallel; returns False on any bad result. Fills results[id]."""
    total, done, counts = len(tests), 0, {}
    t0 = time.monotonic()
    expected = sum(t.expected for t in tests)
    running = {}
    detail_limit = int(os.environ.get('YCXX_FAIL_DETAILS', '10'))
    ok = True
    pool = cf.ThreadPoolExecutor(max_workers=max(1, args.jobs))
    try:
        futs = {}
        for t in tests:
            f = pool.submit(lambda t=t: (running.__setitem__(t.id, time.monotonic()), kinds.execute(t, ctx))[1])
            futs[f] = t
        pending = set(futs)
        while pending:
            finished, pending = cf.wait(pending, timeout=0.2, return_when=cf.FIRST_COMPLETED)
            for f in finished:
                t = futs[f]
                started = running.pop(t.id, time.monotonic())
                secs = time.monotonic() - started
                try:
                    code, output, steps = f.result()
                except Exception:  # a bug in the driver: the test is unresolved, the run goes on
                    code, output, steps = 'UNRESOLVED', 'UNRESOLVED: driver error\n' + traceback.format_exc(), []
                done += 1
                counts[code] = counts.get(code, 0) + 1
                if len(steps) > 4:  # the transcript has them all
                    steps = steps[:3] + [f'+{len(steps) - 3} more steps']
                err = summary_error(code, output)
                results[t.id] = {'name': t.id, 'code': code, 'secs': round(secs, 3), 'steps': steps,
                                 'error': err, 'output': output, 'stage': stage, 'kind': t.kind, 'link': ''}
                good = code in GOOD or code == 'UNSUPPORTED'
                ok = ok and good
                if not (args.quiet and good):
                    col = ui.green if code in GOOD else ui.yellow if code == 'UNSUPPORTED' else ui.red
                    tail = '; '.join(steps) if code in GOOD else (err or '; '.join(steps))
                    ui.line(f'  {col}{code:<11}{ui.reset} {t.id}  {ui.dim}{tail}{ui.reset}')
                if args.verbose or (not good and shown_failures[0] < detail_limit):
                    if not good:
                        shown_failures[0] += 1
                    ui.block(f'{code}: {t.id}', output)
                if not good and ui.gha:
                    ui.line(f'::error file=tests/{t.id},title={code} {t.id}::{err or code}')
            el = time.monotonic() - t0
            done_exp = sum(x.expected for x in tests if x.id in results)
            pace = min(4.0, max(0.25, el / max(done_exp / max(1, args.jobs), 0.01))) if done_exp else 1.0
            left = (expected - done_exp) / max(1, args.jobs) * pace
            now = time.monotonic()
            slow = max(list(running.items()), key=lambda kv: now - kv[1], default=None)
            ui.status(f'  {ui.cyan}{stage}{ui.reset} {done}/{total} ' +
                      ' '.join(f'{c.lower()} {n}' for c, n in counts.items()) +
                      f' · {dur(el)} · ETA ~{dur(left)}' +
                      (f' · running {slow[0]} ({dur(now - slow[1])})' if slow else ''))
    except KeyboardInterrupt:
        interrupted[0] = True
        pool.shutdown(wait=False, cancel_futures=True)
        transcript.kill_all()  # the commands run in their own process groups
        pool.shutdown(wait=True)
        ui.clear()
        ui.warn('interrupted (Ctrl-C)', f'{done} of {total} tests finished in this stage')
        return False
    pool.shutdown(wait=True)
    ui.clear()
    return ok


def detect_branch(args):
    if args.branch:
        return args.branch
    for v in ('GITHUB_HEAD_REF', 'GITHUB_REF_NAME'):
        if os.environ.get(v):
            return os.environ[v]
    b = state.git(REPO, 'rev-parse', '--abbrev-ref', 'HEAD')
    return b if b and b != 'HEAD' else 'detached'


def main(argv=None):
    try:
        return _main(argv)
    except SystemExit:
        raise
    except Exception:
        traceback.print_exc()
        print('tools/test: internal error (exit 3)', file=sys.stderr)
        return EXIT_INTERNAL


def _main(argv):
    args = parse_args(sys.argv[1:] if argv is None else argv)
    ui = UI()
    started = time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())
    t_start = time.monotonic()
    if args.merge_state:
        state.merge(args.state, args.merge_state)
        ui.ok('merged', f'{" ".join(args.merge_state)} into {args.state}')
        return EXIT_OK

    stages = [w for w in args.what if w in STAGES]
    filters = []
    for w in (w for w in args.what if w not in STAGES):
        p = Path(w)
        if p.is_absolute() or p.exists():  # a path: relative to tests/
            p = Path(os.path.relpath(p.resolve(), TESTS))
        filters.append(p.as_posix().removeprefix('tests/').rstrip('/').removeprefix('./'))
    stop_on_fail = args.fail_fast
    tier = 'fast' if args.fast else args.tier
    if tier:
        if stages:
            print('tools/test: give stages or a tier, not both', file=sys.stderr)
            return EXIT_USAGE
        stages = list(TIERS[tier])
        stop_on_fail = True
    if args.only_failed:
        stages = ['failed']
    if not stages:
        stages = list(STAGES)
    if args.failed_first and 'failed' not in stages:
        stages.insert(next((i for i, s in enumerate(stages) if s in TEST_STAGES), len(stages)), 'failed')
    stages = [s for s in STAGES if s in stages]  # always in pyramid order
    shard = None
    if args.shard:
        m = re.fullmatch(r'(\d+)/(\d+)', args.shard)
        if not m or not 1 <= int(m[1]) <= int(m[2]):
            print(f'tools/test: --shard wants I/N with 1 <= I <= N, not {args.shard}', file=sys.stderr)
            return EXIT_USAGE
        shard = (int(m[1]), int(m[2]))

    try:
        compilers = toolchain.discover(REPO)
    except ValueError as e:
        print(f'tools/test: {e}', file=sys.stderr)
        return EXIT_USAGE
    cut = compilers[args.cxx]
    branch = detect_branch(args)
    st = state.State(args.state, branch, args.cxx)
    commit = state.git(REPO, 'rev-parse', '--short=12', 'HEAD') or 'unknown'
    dirty = state.git(REPO, 'status', '--porcelain')
    if dirty:
        commit += f' (+{len(dirty.splitlines())} uncommitted changes)'

    # Selection: every test, by path filter and tier, then each into the first stage claiming it.
    alltests = discover.discover(TESTS)
    for t in alltests:
        t.expected = st.secs(t.id)
    for f in filters:
        if f == '.':
            continue
        if not any(t.id == f or t.id.startswith(f) for t in alltests):
            print(f'tools/test: "{f}" is neither a stage ({" ".join(STAGES)}) nor a prefix of a test', file=sys.stderr)
            return EXIT_USAGE
    filters = [f for f in filters if f != '.']
    sel = [t for t in alltests if not filters or any(t.id == f or t.id.startswith(f) for f in filters)]
    if tier != 'nightly':
        sel = [t for t in sel if t.one('TIER') != 'nightly']
    picked, plan, notes = set(), {}, {}
    loads = [0.0] * shard[1] if shard else None
    changed = None
    for s in stages:
        if s == 'failed':
            ts = [t for t in sel if t.id in st.failing]
            notes[s] = f'{len(st.failing)} failing in the last run of ' + ('main (new branch)' if st.inherited else branch)
        elif s == 'affected':
            changed = state.changed_files(REPO, args.base)
            ts, notes[s] = state.affected(changed, sel)
        elif s in TEST_STAGES:
            ts = [t for t in sel if t.stage == s]
        else:
            continue
        ts = [t for t in ts if t.id not in picked]
        picked |= {t.id for t in ts}  # before sharding: every shard must see the same stage lists
        if shard:
            ts, _ = state.shard(ts, st.secs, *shard, loads)
            notes[s] = (notes.get(s, '') + f'; shard {shard[0]}/{shard[1]}').lstrip('; ')
        ts.sort(key=lambda t: (-t.expected, t.id))  # longest first: the pool finishes together
        plan[s] = ts

    if args.list:
        for s, ts in plan.items():
            print(f'# {s}: {len(ts)} tests, ~{sum(t.expected for t in ts):.1f}s' + (f' ({notes[s]})' if s in notes else ''))
            for t in ts:
                print(f'{t.id}\t{t.expected:.2f}s')
        return EXIT_OK

    ui.title(f'ycxx tests: {" ".join(stages)}' + (f' (tier {tier})' if tier else ''))
    ui.info('repository', f'{REPO} @ {commit}, branch {branch}')
    ui.info('platform', f'{platform.system()} {platform.machine()}')
    meta = {'command': 'tools/test ' + shlex.join(sys.argv[1:] if argv is None else argv), 'started': started,
            'commit': commit, 'branch': branch, 'host': f'{platform.system()} {platform.release()} {platform.machine()}',
            'compiler under test': args.cxx}
    for c in compilers.values():
        if c.available:
            ui.info(c.name + (' (under test)' if c is cut else ''), f'{c.exe}: {c.version}' +
                    (f'; features: {" ".join(sorted(c.features)) or "none yet"}' if c.name == 'ycxx' else ''))
            meta[c.name] = f'{c.exe}: {c.version}'
        else:
            ui.warn(c.name + (' (under test)' if c is cut else ''), f'not found: {c.why_missing}')
            meta[c.name] = f'not found ({c.why_missing})'
    if args.require_oracles and not all(compilers[o].available for o in toolchain.ORACLES):
        print('tools/test: --require-oracles: an oracle is missing', file=sys.stderr)
        return EXIT_USAGE
    if args.cxx != 'ycxx' and not cut.available:
        print(f'tools/test: the compiler under test, {args.cxx}, is not found ({cut.why_missing})', file=sys.stderr)
        return EXIT_USAGE
    if args.cxx == 'ycxx' and cut.features and not cut.available:
        print(f'tools/test: tests/features/ycxx.txt lists features but ycxx is not built ({cut.why_missing})',
              file=sys.stderr)
        return EXIT_USAGE
    if shard:
        meta['shard'] = f'{shard[0]}/{shard[1]}'
    ui.info('state', f'{args.state}' + (' (no failures recorded for this branch: using main\'s)' if st.inherited else ''))

    if not args.no_state:  # what this run alone learned, for --merge-state; none yet
        (Path(args.state) / 'last-run.json').unlink(missing_ok=True)
    logs = REPO / 'build' / 'test-logs'
    logs.mkdir(parents=True, exist_ok=True)
    ctx = kinds.Ctx(REPO, cut, compilers, args.oracle, REPO / 'build' / 'test-work' / args.cxx, args.timeout)
    records, results, shown, interrupted = [], {}, [0], [False]
    failed_stage = None
    for s in stages:
        t0 = time.monotonic()
        if interrupted[0] or (failed_stage and stop_on_fail):
            why = 'interrupted' if interrupted[0] else f'stopped after the failing {failed_stage} stage'
            ui.section(s)
            ui.skip(s, f'(not run: {why})')
            records.append({'name': s, 'status': 'skip', 'secs': 0, 'detail': f'not run: {why}'})
            continue
        try:
            if s == 'policy':
                ui.section('policy checks')
                ok = policy_stage(ui, logs, records)
            elif s == 'build':
                ui.section('build')
                ok = build_stage(ui, logs, records, args.jobs)
            else:
                ts = plan[s]
                ui.section(f'{s}: {len(ts)} tests' + (f' ({notes[s]})' if s in notes else ''))
                ok = run_tests(ui, args, s, ts, ctx, results, shown, interrupted) if ts else True
                bad = sum(1 for t in ts if t.id in results and results[t.id]['code'] not in GOOD | {'UNSUPPORTED'})
                records.append({'name': s, 'status': 'interrupted' if interrupted[0] else 'ok' if ok else 'FAIL',
                                'secs': time.monotonic() - t0,
                                'detail': f'{len(ts)} tests, {bad} not passing' + (f'; {notes[s]}' if s in notes else '')})
        except KeyboardInterrupt:
            interrupted[0] = True
            ok = False
        if not ok and not interrupted[0]:
            failed_stage = failed_stage or s

    # The state: only what finished before any Ctrl-C (a test cut short proves nothing).
    if not args.no_state and results:
        kept = {k: (v['code'], v['secs']) for k, v in results.items()
                if not (interrupted[0] and v['code'] not in GOOD | {'UNSUPPORTED'})}
        st.update(kept, commit)

    # Summary and report.
    tests = list(results.values())
    for r in records:  # failed policy and build steps are failures in the report, with their logs
        if r['status'] == 'FAIL' and 'log' in r:
            tests.append({'name': f'[{r["name"]}]', 'code': 'FAIL', 'secs': r['secs'], 'steps': [r['detail']],
                          'error': 'see the log', 'output': r['log'], 'stage': 'policy/build', 'kind': 'command', 'link': ''})
    ui.title('Summary')
    counts = report.counts_of(tests)
    for r in records:
        if r['name'] in STAGES or r['status'] != 'ok':
            {'ok': ui.ok, 'skip': ui.skip, 'interrupted': ui.warn}.get(r['status'], ui.fail)(
                r['name'], f'({dur(r["secs"])}) {r["detail"]}')
    title = f'ycxx tests ({args.cxx})' + (f', tier {tier}' if tier else '') + (f', shard {args.shard}' if shard else '')
    meta['stages'] = ' '.join(stages)
    meta['time'] = dur(time.monotonic() - t_start)
    clean = {k: v for k, v in meta.items()}
    for r in records:
        r.pop('log', None)
    page = report.write(logs / 'report', title, clean, records, tests, REPO)
    ui.line()
    total_bad = sum(n for c, n in counts.items() if c not in GOOD | {'UNSUPPORTED'})
    summary = ', '.join(f'{c} {n}' for c, n in counts.items()) or 'no tests ran'
    if interrupted[0]:
        ui.warn(f'interrupted after {meta["time"]}', summary)
    elif failed_stage:
        ui.fail(f'failed in {meta["time"]}', f'{summary}; first failing stage: {failed_stage}')
    else:
        ui.ok(f'passed in {meta["time"]}', summary)
    unsup = counts.get('UNSUPPORTED', 0)
    if unsup:
        ui.info(f'{unsup} unsupported', ', '.join(f'{n} × {r}' for r, n in report.unsupported_reasons(tests).items()))
    ui.info('report', str(page or logs / 'report.md') + ' (and .md, .tsv, .json)')
    if interrupted[0]:
        return EXIT_INTERRUPTED
    return EXIT_FAIL if failed_stage or total_bad else EXIT_OK
