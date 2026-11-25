"""A project watch rebuilds on native, planning, and generator changes (#1263)."""

from collections import deque
import json
from pathlib import Path
from queue import Empty, Queue
import signal
import subprocess
import sys
import tempfile
from threading import Thread
import time

from project_build import (
    action_keys, check_cold_actions, check_program, check_wire_action, env_for, prepare, tu_outcomes,
)
from runtime_dir import runtime_root


class Watch:
    def __init__(self, ycxx, scope, env):
        self.process = subprocess.Popen(
            [ycxx, 'build', 'app', '--watch', '--trace-engine=json'],
            cwd=scope, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
            text=True, encoding='utf-8',
        )
        self.lines = Queue()
        self.tail = deque(maxlen=12)
        self.reader = Thread(target=self.read_stderr, daemon=True)
        self.reader.start()

    def read_stderr(self):
        try:
            for line in self.process.stderr:
                self.lines.put(line)
        finally:
            self.lines.put(None)

    def record(self, deadline):
        line = self.lines.get(timeout=max(0, deadline - time.monotonic()))
        assert line is not None, ('watch exited', self.process.poll(), list(self.tail))
        self.tail.append(line.rstrip()[-500:])
        try:
            record = json.loads(line)
        except ValueError as error:
            raise AssertionError(('watch emitted invalid JSON Lines', line)) from error
        assert isinstance(record, dict), record
        return record

    def build(self, label, *, succeeds=True):
        deadline = time.monotonic() + 25
        events = []
        summary = None
        try:
            while True:
                record = self.record(deadline)
                if 'actions_executed' in record:
                    assert summary is None, (label, 'unexpected second build', record)
                    summary = record
                elif 'kind' in record:
                    events.append(record)
                elif 'watching' in record:
                    assert summary is not None, (label, 'watch wait before build summary', record)
                    assert (summary['status'] == 0) == succeeds, (label, summary)
                    assert isinstance(record['watching'], list), record
                    return summary, events, record['watching']
                elif 'error' in record:
                    raise AssertionError((label, record))
        except Empty as error:
            raise AssertionError((label, 'watch timed out', list(self.tail))) from error

    def idle(self, label):
        deadline = time.monotonic() + 1.0
        try:
            while True:
                record = self.record(deadline)
                assert not {'build', 'actions_executed', 'error'} & record.keys(), (label, record)
                if time.monotonic() >= deadline:
                    return
        except Empty:
            pass

    def close(self):
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGINT)
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=5)
        self.reader.join(timeout=5)
        self.process.stderr.close()


def replace_source(scope, relative, text):
    # Present a complete input to the daemon in one rename, with the temporary file
    # under the output directory that workspace observations ignore.
    temporary = scope / '.ycxx' / 'watch-test-input'
    temporary.write_text(text, encoding='utf-8')
    temporary.replace(scope / relative)


def check_watch(scope, watch):
    executable = scope / '.ycxx' / 'out' / 'app'
    manifest = (scope / 'build.ycxx').read_text(encoding='utf-8')
    summary, events, watched = watch.build('initial build')
    check_cold_actions(summary, events)
    watched_names = {Path(path).name for path in watched}
    assert {'build.ycxx', 'schema.txt', 'main.cc'} <= watched_names, watched
    check_program(executable, 17)
    installed_at = executable.stat().st_mtime_ns
    (scope / 'unrelated' / 'scratch.txt').write_text('ignored\n', encoding='utf-8')
    watch.idle('unrelated file and artifact publication')
    assert executable.stat().st_mtime_ns == installed_at, 'idle watch replaced the executable'

    replace_source(scope, 'core.cc', 'int core() { return 4; }\n')
    summary, events, _ = watch.build('ordinary source edit')
    assert summary['actions_executed'] == 0, summary
    assert 'executed' in tu_outcomes(events, 'core.cc'), 'ordinary source was not recompiled'
    check_program(executable, 19)
    watch.idle('ordinary source edit was observed once without an inspection cache')

    replace_source(scope, 'include/config.h', '#define CONFIG 13\n')
    summary, events, _ = watch.build('new earlier header')
    assert summary['actions_executed'] == 0, summary
    assert 'executed' in tu_outcomes(events, 'main.cc'), 'new earlier header did not invalidate main.cc'
    check_program(executable, 29)

    replace_source(scope, 'schema.txt', '11\n')
    summary, events, _ = watch.build('generator input edit')
    check_wire_action(summary, events)
    assert 'executed' in tu_outcomes(events, 'wire.cc'), 'changed generator output was not recompiled'
    assert 'executed' not in tu_outcomes(events, 'main.cc'), 'unchanged generated header rebuilt main.cc'
    check_program(executable, 33)

    replace_source(scope, 'build.ycxx', manifest.replace('["include", "fallback"]', '["fallback", "include"]'))
    summary, events, _ = watch.build('build definition edit')
    assert summary['actions_executed'] == 1, summary
    assert action_keys(events, 'RuleWorker') == ['core'], events
    check_program(executable, 23)

    replace_source(scope, 'modules/extra.cc', 'int extra() { return 42; }\n')
    summary, events, _ = watch.build('new match in an initially empty glob')
    assert summary['actions_executed'] == 0, summary
    assert 'executed' in tu_outcomes(events, 'extra.cc'), 'new glob match was not compiled'
    check_program(executable, 23)

    previous = executable.read_bytes()
    replace_source(scope, 'schema.txt', 'rejected\n')
    summary, events, _ = watch.build('generator failure', succeeds=False)
    check_wire_action(summary, events, 'failed')
    assert any('schema rejected' in diagnostic for diagnostic in summary['diagnostics']), summary
    assert executable.read_bytes() == previous, 'failed watch build replaced the previous executable'
    check_program(executable, 23)

    replace_source(scope, 'schema.txt', '17\n')
    summary, events, _ = watch.build('recover after generator failure')
    check_wire_action(summary, events)
    check_program(executable, 29)
    watch.idle('successful recovery and artifact publication')


def main():
    ycxx = Path(sys.argv[1]).resolve()
    with (
        tempfile.TemporaryDirectory(prefix='ycxx-project-watch-') as workspace,
        tempfile.TemporaryDirectory(prefix='yb', dir=runtime_root('yb')) as runtime,
    ):
        scope = Path(workspace).resolve()
        prepare(scope)
        # A regular file blocks snapshot storage irrespective of test user permissions.
        rules_cache = scope / '.ycxx' / 'rules'
        rules_cache.write_text('inspection cache unavailable\n', encoding='utf-8')
        (scope / 'modules').mkdir()
        (scope / 'unrelated').mkdir()
        manifest = scope / 'build.ycxx'
        manifest.write_text(
            manifest.read_text(encoding='utf-8').replace('["core.cc"]', '["core.cc", glob("modules/*.cc")]'),
            encoding='utf-8',
        )
        env = env_for(runtime)
        watch = Watch(ycxx, scope, env)
        try:
            check_watch(scope, watch)
            assert rules_cache.read_text(encoding='utf-8') == 'inspection cache unavailable\n'
        finally:
            watch.close()
            subprocess.run([ycxx, 'ctl', 'shutdown'], cwd=scope, env=env, capture_output=True, timeout=10)


if __name__ == '__main__':
    main()
