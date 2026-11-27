"""Observe the exact watch test helpers without changing their deadlines or checks."""

import atexit
import json
import os
from pathlib import Path
import sys
from time import monotonic_ns, time_ns


FIXTURES = {
    'project_host_producer_witness.py',
    'project_staged_witness.py',
    'project_witness_helpers.py',
    'project_package_probe.py',
    'project_shader_reflection.py',
}


def install(directory, script):
    sys.path.insert(0, str(script.resolve().parent))
    from project_build_watch import Watch

    observations = []
    active = None
    initialize, reader, build, close = Watch.__init__, Watch.read_stderr, Watch.build, Watch.close

    def on_init(self, ycxx, scope, env, *args):
        nonlocal active
        self.observe = {'scope': str(scope), 'events': []}
        active = self.observe
        observations.append(self.observe)
        self.observe['events'].append([monotonic_ns(), 'launch'])
        return initialize(self, ycxx, scope, env, *args)

    def on_reader(self):
        queue_put = self.lines.put
        events = self.observe['events']

        def on_put(line):
            events.append([monotonic_ns(), 'stderr', line])
            return queue_put(line)

        self.lines.put = on_put
        try:
            return reader(self)
        finally:
            self.lines.put = queue_put

    def on_build(self, label, *, succeeds=True):
        events = self.observe['events']
        events.append([monotonic_ns(), 'await', label, os.getloadavg()])
        try:
            result = build(self, label, succeeds=succeeds)
        except BaseException as error:
            events.append([monotonic_ns(), 'error', type(error).__name__, str(error)])
            raise
        events.append([monotonic_ns(), 'return', label])
        return result

    def on_close(self):
        nonlocal active
        self.observe['events'].append([monotonic_ns(), 'close'])
        try:
            return close(self)
        finally:
            if active is self.observe:
                active = None

    def observe_edit(name):
        operation = getattr(Path, name)

        def edit(self, *args, **kwargs):
            trace = active
            target = Path(args[0] if args else kwargs['target']) if name == 'replace' else self
            relative = None
            if trace is not None:
                try:
                    relative = target.relative_to(trace['scope'])
                except ValueError:
                    pass
            if relative is None or relative.parts[:1] == ('.ycxx',):
                return operation(self, *args, **kwargs)
            before = monotonic_ns()
            result = operation(self, *args, **kwargs)
            trace['events'].append([monotonic_ns(), 'edit', name, str(relative), before])
            return result

        setattr(Path, name, edit)

    def write_observations():
        path = directory / f'{script.stem}-{os.getpid()}.json'
        path.write_text(json.dumps({
            'script': script.name, 'clock': 'monotonic_ns', 'flushed_at_utc_ns': time_ns(),
            'flushed_at_monotonic_ns': monotonic_ns(), 'watches': observations,
        }, separators=(',', ':')) + '\n', encoding='utf-8')

    Watch.__init__ = on_init
    Watch.read_stderr = on_reader
    Watch.build = on_build
    Watch.close = on_close
    for method in ('replace', 'unlink', 'chmod', 'write_text', 'touch'):
        observe_edit(method)
    atexit.register(write_observations)


directory = os.environ.get('YCXX_WATCH_EVIDENCE')
script = Path(sys.argv[0])
if directory and script.name in FIXTURES:
    install(Path(directory), script)
