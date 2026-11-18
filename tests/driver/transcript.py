"""Command transcripts, in libycxx's format (deps/libycxx/tests/ycxxlit/transcript.py).

Every command a test runs is recorded, whether the test passes or not, as

    $ <the command, quoted for a shell>
    [<step>: exit <status>, <seconds>s]
    <what it printed>

and every check the driver makes as `[check: ...]` lines, so that a passing test shows exactly
how it was run and what it was compared with, and a failing one can be rerun by hand.
"""
import os, shlex, subprocess, time
from dataclasses import dataclass


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
        try:
            p = subprocess.run([str(c) for c in cmd], cwd=cwd, capture_output=True, text=True,
                               errors='replace', timeout=timeout, env=env)
        except subprocess.TimeoutExpired as e:
            secs = time.monotonic() - t0
            self.note(f'[{step}: TIMEOUT after {timeout}s]')
            self.steps.append(f'{step} TIMEOUT')
            out = e.stdout.decode(errors='replace') if isinstance(e.stdout, bytes) else (e.stdout or '')
            return Run(None, out, '', secs)
        except OSError as e:  # not found, not executable
            self.note(f'[{step}: exit 127, 0.00s]\n{e}')
            self.steps.append(f'{step} exit 127')
            return Run(127, '', str(e), 0.0)
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
