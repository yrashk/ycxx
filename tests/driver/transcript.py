"""Command transcripts, in libycxx's format (deps/libycxx/tests/ycxxlit/transcript.py).

Every command a test runs is recorded, whether the test passes or not, as

    $ <the command, quoted for a shell>
    [<step>: exit <status>, <seconds>s]
    <what it printed>

and every check the driver makes as `[check: ...]` lines, so that a passing test shows exactly
how it was run and what it was compared with, and a failing one can be rerun by hand.
"""
import os, shlex, signal, subprocess, threading, time
from dataclasses import dataclass

# The process groups of the commands running now. Each command gets its own, so that a timeout
# kills the whole tree (g++ runs cc1plus and as); Ctrl-C reaches them through kill_all().
# After kill_all() no new command starts.
_live, _lock = set(), threading.Lock()
_stopping = False


def _killpg(pid):
    try:
        os.killpg(pid, signal.SIGKILL)
    except OSError:
        pass


def kill_all():
    global _stopping
    with _lock:
        _stopping = True
        for pid in list(_live):
            _killpg(pid)


@dataclass
class Run:
    status: int | None  # None: timed out
    stdout: str
    stderr: str
    secs: float


class Transcript:
    def __init__(self):
        self.text = []
        self.steps = []  # "syntax exit 1 0.04s", for the one-line summaries

    def note(self, line):
        self.text.append(line.rstrip('\n') + '\n')

    def check(self, what):
        self.note(f'[check: {what}]')

    def run(self, step, cmd, *, cwd=None, timeout=60, env=None, stdout_to=None):
        """Runs cmd and records it. With stdout_to (a path), stdout is written there and only its
        size is recorded (preprocessed output can be large); the Run still carries it."""
        t0 = time.monotonic()
        loc = f'(cd {shlex.quote(str(cwd))} && ' if cwd else ''
        self.note(f'$ {loc}{shlex.join(str(c) for c in cmd)}{")" if cwd else ""}')
        if _stopping:
            self.note(f'[{step}: not run: interrupted]')
            return Run(None, '', '', 0.0)
        try:
            proc = subprocess.Popen([str(c) for c in cmd], cwd=cwd, stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, text=True, errors='replace', env=env,
                                    start_new_session=True)
        except OSError as e:  # not found, not executable
            self.note(f'[{step}: exit 127, 0.00s]\n{e}')
            self.steps.append(f'{step} exit 127')
            return Run(127, '', str(e), 0.0)
        with _lock:
            _live.add(proc.pid)
            if _stopping:  # interrupted between the check and Popen
                _killpg(proc.pid)
        try:
            stdout, stderr = proc.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            _killpg(proc.pid)
            stdout, _ = proc.communicate()
            self.note(f'[{step}: TIMEOUT after {timeout}s]')
            self.steps.append(f'{step} TIMEOUT')
            return Run(None, stdout or '', '', time.monotonic() - t0)
        finally:
            with _lock:
                _live.discard(proc.pid)
        p = subprocess.CompletedProcess(proc.args, proc.returncode, stdout, stderr)
        secs = time.monotonic() - t0
        self.note(f'[{step}: exit {p.returncode}, {secs:.2f}s]')
        self.steps.append(f'{step} exit {p.returncode} {secs:.2f}s')
        if stdout_to:
            with open(stdout_to, 'w', encoding='utf-8') as f:
                f.write(p.stdout)
            self.note(f'(stdout: {p.stdout.count(chr(10))} lines in {os.path.relpath(stdout_to)})')
        elif p.stdout:
            self.text.append(p.stdout if p.stdout.endswith('\n') else p.stdout + '\n')
        if p.stderr:
            self.text.append(p.stderr if p.stderr.endswith('\n') else p.stderr + '\n')
        return Run(p.returncode, p.stdout, p.stderr, secs)

    def output(self):
        return ''.join(self.text)
