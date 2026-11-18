#!/usr/bin/env python3
"""`ycxx lsp` through a private daemon (daemon design §6, #99).

    tests/daemon/lsp.py <ycxx>

Editors' sessions, scripted against one daemon:
- editing: a unit built once through the daemon (which remembers its `-Iinc` command line), then
  opened, edited, saved and closed. Diagnostics on open, on an unsaved change and on save, at the
  right 0-based position; an unsaved header reaches its includer, and closing it falls back to
  the disk; a build after a keystroke executes and validates nothing, and its object is a cold
  compile's of the disk (§6.1, §6.3).
- symlinked: the same unit opened through a symbolic link to the scope: its command line and its
  header's buffer are found by real path, and the answers carry the editor's URI.
- concurrent_build: a build sent while a `didChange` of its unit is being diagnosed compiles the
  saved file, and the diagnostics are the buffer's.
- crossing: answers and edits larger than the socket buffers, crossing: both arrive.
- exit_status: `exit` without `shutdown` ends `ycxx lsp` with 1, after it with 0.
- shutdown_with_session: `ctl shutdown` with a session open ends the session and the daemon, and
  the next build starts a new one.
"""
import json, os, queue, subprocess, sys, threading, time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from process import Env, fail, failures, gone, slow_source  # noqa: E402

TIMEOUT = 120  # a debug build's first diagnosis builds the standard library's headers
UNDECLARED = "use of undeclared identifier 'gg'"


class Editor:
    """An LSP client over `ycxx lsp`'s stdio."""

    def __init__(self, env, root):
        self.p = subprocess.Popen([env.ycxx, 'lsp'], cwd=root, env=env.base, stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        env.clients.append(self.p)
        self.messages = queue.Queue()
        self.next_id = 0
        threading.Thread(target=self._read, daemon=True).start()
        init = self.request('initialize', {'processId': os.getpid(), 'rootUri': Path(root).as_uri(), 'capabilities': {}})
        reply = self.wait(lambda m: m.get('id') == init, 'initialize')
        if reply is not None and reply['result']['capabilities']['textDocumentSync']['change'] != 1:
            fail('initialize: the server does not ask for full-text sync')
        self.notify('initialized', {})

    def _read(self):
        out = self.p.stdout
        while True:
            length = None
            while True:
                line = out.readline()
                if not line:
                    self.messages.put(None)
                    return
                line = line.strip()
                if not line:
                    break
                name, _, value = line.decode().partition(':')
                if name.lower() == 'content-length':
                    length = int(value)
            self.messages.put(json.loads(out.read(length)))

    def send(self, message):
        body = json.dumps(dict(message, jsonrpc='2.0')).encode()
        self.p.stdin.write(b'Content-Length: %d\r\n\r\n' % len(body) + body)
        self.p.stdin.flush()

    def notify(self, method, params):
        self.send({'method': method, 'params': params})

    def request(self, method, params):
        self.next_id += 1
        self.send({'id': self.next_id, 'method': method, 'params': params})
        return self.next_id

    def open(self, path, text, version=1):
        self.notify('textDocument/didOpen', {'textDocument': {'uri': path.as_uri(), 'languageId': 'cpp', 'version': version,
                                                              'text': text}})

    def change(self, path, text, version):
        self.notify('textDocument/didChange', {'textDocument': {'uri': path.as_uri(), 'version': version},
                                               'contentChanges': [{'text': text}]})

    def wait(self, pred, what):
        """The first message `pred` accepts; earlier ones are dropped. None (and a failure) when
        the stream ends or nothing comes in time."""
        while True:
            try:
                m = self.messages.get(timeout=TIMEOUT)
            except queue.Empty:
                fail(f'{what}: nothing came in {TIMEOUT} s')
                return None
            if m is None:
                fail(f'{what}: `ycxx lsp` closed its output')
                return None
            if pred(m):
                return m

    def diagnostics(self, path, version, what):
        """The diagnostics published for `path` at `version`, as (line, character, severity,
        message) tuples."""
        uri = path.as_uri()
        m = self.wait(lambda m: m.get('method') == 'textDocument/publishDiagnostics' and m['params']['uri'] == uri and
                      m['params'].get('version') == version, what)
        if m is None:
            return None
        return [(d['range']['start']['line'], d['range']['start']['character'], d['severity'], d['message'])
                for d in m['params']['diagnostics']]

    def finish(self, shutdown=True):
        """Ends the session; `ycxx lsp`'s exit status."""
        if shutdown:
            shut = self.request('shutdown', None)
            self.wait(lambda m: m.get('id') == shut, 'shutdown')
        self.notify('exit', None)
        try:
            return self.p.wait(timeout=60)
        except subprocess.TimeoutExpired:
            fail('`ycxx lsp` did not exit after `exit`')
            return None


def expect(got, want, what):
    if got != want:
        fail(f'{what}: got {got}, want {want}')


GOOD = '#include "h.hpp"\nint main() { return g() - 1; }\n'
BAD = '#include "h.hpp"\nint main() {\n  return gg();\n}\n'


def project(env, name):
    """A unit `a.cpp` that needs `-Iinc` for its header, by real path, built once through the daemon."""
    d = Path(os.path.realpath(env.scope)) / name  # an editor's name: /tmp is /private/tmp on macOS
    (d / 'inc').mkdir(parents=True)
    (d / 'a.cpp').write_text(GOOD)
    (d / 'inc' / 'h.hpp').write_text('int g() { return 1; }\n')
    code, _, err = env.cxx(['-Iinc', '-c', 'a.cpp', '-o', 'warm.o'], d, env.daemon)
    expect((code, err), (0, b''), f'{name}: the first build')
    return d


def editing(env):
    d = project(env, 'editing')
    unit, header = d / 'a.cpp', d / 'inc' / 'h.hpp'
    ed = Editor(env, d)
    ed.open(unit, GOOD)
    expect(ed.diagnostics(unit, 1, 'open'), [], 'a clean unit on open')

    # An unsaved edit: line 3, column 10 in the compiler's terms.
    ed.change(unit, BAD, 2)
    expect(ed.diagnostics(unit, 2, 'change'), [(2, 9, 1, UNDECLARED)], 'an unsaved error')

    # The build reads the disk, and neither executes nor deep-verifies a cell (§6.3). The first
    # build after the editor's unit appeared verifies the `low` inputs that opening it set, once.
    env.cxx(['-Iinc', '-c', 'a.cpp', '-o', 'after.o'], d, env.daemon)
    ed.change(unit, BAD + '\n', 3)
    expect(len(ed.diagnostics(unit, 3, 'another keystroke') or []), 1, 'the error after another keystroke')
    validated = env.number('validated')
    code, _, err = env.cxx(['-Iinc', '-c', 'a.cpp', '-o', 'after.o'], d, env.daemon)
    expect((code, err), (0, b''), 'a build after an unsaved edit')
    expect(env.number('last_executed'), 0, 'cells a build executed after a keystroke')
    expect(env.number('validated') - validated, 0, 'cells a build validated after a keystroke')
    env.cxx(['-Iinc', '-c', 'a.cpp', '-o', 'cold.o'], d, env.cold)
    if (d / 'after.o').read_bytes() != (d / 'cold.o').read_bytes():
        fail('a build after an unsaved edit is not a cold compile of the disk')

    # An unsaved header reaches its includer; closing it falls back to the disk.
    ed.change(unit, GOOD, 4)
    expect(ed.diagnostics(unit, 4, 'fixed'), [], 'the unit fixed again')
    ed.open(header, 'int h();\n')
    got = ed.diagnostics(unit, 4, 'an unsaved header')
    expect([x[:3] for x in got or []], [(1, 20, 1)], 'an unsaved header breaks its includer')
    ed.notify('textDocument/didClose', {'textDocument': {'uri': header.as_uri()}})
    expect(ed.diagnostics(unit, 4, 'the header closed'), [], 'a closed header is the disk\'s again')

    # A save: the disk now has the error too.
    ed.change(unit, BAD, 5)
    expect(len(ed.diagnostics(unit, 5, 'change before save') or []), 1, 'the error before the save')
    unit.write_text(BAD)
    ed.notify('textDocument/didSave', {'textDocument': {'uri': unit.as_uri()}})
    expect(ed.diagnostics(unit, 5, 'save'), [(2, 9, 1, UNDECLARED)], 'the error after the save')
    code, _, err = env.cxx(['-Iinc', '-fsyntax-only', 'a.cpp'], d, env.daemon)
    expect((code, err), (1, f'a.cpp:3:10: error: {UNDECLARED}\n'.encode()), 'the saved error in a build')
    expect(ed.finish(), 0, '`ycxx lsp` after shutdown and exit')


def symlinked(env):
    d = project(env, 'symlinked')
    link = Path(env.runtime) / 'link'  # removed with the runtime directory
    link.symlink_to(d)
    unit, header = link / 'a.cpp', link / 'inc' / 'h.hpp'
    ed = Editor(env, link)
    ed.open(unit, GOOD)
    expect(ed.diagnostics(unit, 1, 'open through a link'), [], 'the unit through a link finds its command line')
    ed.open(header, 'int h();\n')
    expect([x[:3] for x in ed.diagnostics(unit, 1, 'a header through a link') or []], [(1, 20, 1)],
           'a header opened through a link reaches its includer')
    expect(ed.finish(), 0, '`ycxx lsp` through a link')


def concurrent_build(env):
    d = Path(os.path.realpath(env.scope)) / 'concurrent'
    d.mkdir()
    saved = slow_source('main', 0)
    unit = d / 's.cpp'
    unit.write_text(saved)
    env.cxx(['-c', 's.cpp', '-o', 'warm.o'], d, env.daemon)
    ed = Editor(env, d)
    ed.open(unit, saved)
    expect(ed.diagnostics(unit, 1, 'open the slow unit'), [], 'the slow unit on open')
    ed.change(unit, saved + 'int z = gg;\n', 2)
    build = subprocess.Popen([env.ycxx, 'cxx', '-c', 's.cpp', '-o', 'during.o'], cwd=d, env=env.daemon,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    env.clients.append(build)
    got = ed.diagnostics(unit, 2, 'the change during a build')
    expect([x[3] for x in got or []], [UNDECLARED], 'the diagnosis during a build is the buffer\'s')
    out, err = build.communicate(timeout=300)
    expect((build.returncode, err), (0, b''), 'a build during a didChange')
    env.cxx(['-c', 's.cpp', '-o', 'cold.o'], d, env.cold)
    if (d / 'during.o').read_bytes() != (d / 'cold.o').read_bytes():
        fail('a build during a didChange is not a cold compile of the saved file')
    expect(ed.finish(), 0, '`ycxx lsp` after a concurrent build')


def crossing(env):
    """A 600-error answer (tens of KB) is being written while an edit as large is sent: neither the
    tunnel nor the daemon may block one direction on the other."""
    d = Path(os.path.realpath(env.scope)) / 'crossing'
    d.mkdir()
    unit = d / 'x.cpp'
    unit.write_text('int main() { return 0; }\n')
    many = lambda tag: ''.join(f'int {tag}{i} = gg;\n' for i in range(600)) + 'int main() { return 0; }\n'
    ed = Editor(env, d)
    ed.open(unit, unit.read_text())
    expect(ed.diagnostics(unit, 1, 'open'), [], 'a clean unit')
    ed.change(unit, many('a'), 2)
    time.sleep(0.02)
    ed.change(unit, many('b'), 3)
    expect(len(ed.diagnostics(unit, 3, 'large messages crossing') or []), 600, 'every error of the last edit')
    expect(ed.finish(), 0, '`ycxx lsp` after large messages')


def exit_status(env):
    ed = Editor(env, env.scope)
    expect(ed.finish(shutdown=False), 1, '`ycxx lsp` after `exit` without `shutdown`')


def shutdown_with_session(env, pid):
    ed = Editor(env, env.scope)
    subprocess.run([env.ycxx, 'ctl', 'shutdown'], cwd=env.scope, env=env.base, capture_output=True, timeout=30)
    try:
        ed.p.wait(timeout=30)
    except subprocess.TimeoutExpired:
        fail('`ycxx lsp` still runs after the daemon was shut down')
    try:
        os.kill(pid, 0)
        time.sleep(5)
        os.kill(pid, 0)
        fail('the daemon still runs 5 s after `ctl shutdown` with an LSP session open')
    except ProcessLookupError:
        pass
    (env.scope / 'next.cpp').write_text('int main() { return 0; }\n')
    code, _, err = env.cxx(['-fsyntax-only', 'next.cpp'], env.scope, env.daemon)
    expect((code, err), (0, b''), 'a build after the shutdown starts a new daemon')


def main():
    env = Env(os.path.abspath(sys.argv[1]))
    try:
        (env.scope / 'first.cpp').write_text('int main() { return 0; }\n')
        if env.cxx(['-fsyntax-only', 'first.cpp'], env.scope, env.daemon)[0] != 0 or env.pid() is None:
            fail('the first request through the daemon failed: no test can run')
            return 1
        for test in (editing, symlinked, concurrent_build, crossing, exit_status):
            t0 = time.monotonic()
            pid = env.pid()
            if pid is None:
                break
            test(env)
            env.same_daemon(test.__name__, pid)
            print(f'{test.__name__}: {time.monotonic() - t0:.1f}s')
        if not failures:
            expect(env.number('diagnoses') >= 15, True, 'the daemon counts its diagnoses')
            shutdown_with_session(env, env.pid())
    finally:
        env.close()
    if failures:
        print(f'daemon.lsp: {len(failures)} failure(s)', file=sys.stderr)
        return 1
    print('daemon.lsp: editing, a symlinked path, a concurrent build, crossing messages, exit status and shutdown')
    return 0


if __name__ == '__main__':
    sys.exit(main())
