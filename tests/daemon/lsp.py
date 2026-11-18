#!/usr/bin/env python3
"""`ycxx lsp` through a private daemon (daemon design §6, #99).

    tests/daemon/lsp.py <ycxx>

An editor's session, scripted: a unit built once through the daemon (which remembers its command
line), then opened, edited, saved and closed in `ycxx lsp`. Checked:
- diagnostics on open, on change (an unsaved edit), and on save, at the right 0-based position;
- an unsaved header reaches the unit that includes it, and closing it falls back to the disk;
- unsaved text never reaches a build: a build after a keystroke executes nothing and its object is
  a cold compile's of the disk (§6.1, §6.3);
- `shutdown` then `exit` ends `ycxx lsp` with 0, and the daemon that answered is still the one.
"""
import json, os, queue, subprocess, sys, threading
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from process import Env, fail, failures  # noqa: E402

TIMEOUT = 120  # a debug build's first diagnosis builds the standard library's headers


class Editor:
    """An LSP client over `ycxx lsp`'s stdio."""

    def __init__(self, env):
        self.p = subprocess.Popen([env.ycxx, 'lsp'], cwd=env.scope, env=env.base, stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        env.clients.append(self.p)
        self.messages = queue.Queue()
        self.next_id = 0
        threading.Thread(target=self._read, daemon=True).start()

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


def expect(got, want, what):
    if got != want:
        fail(f'{what}: got {got}, want {want}')


def main():
    env = Env(os.path.abspath(sys.argv[1]))
    try:
        scope = Path(os.path.realpath(env.scope))  # the editor's name: /tmp is /private/tmp on macOS
        unit, header = scope / 'a.cpp', scope / 'h.hpp'
        good = '#include "h.hpp"\nint main() { return g() - 1; }\n'
        bad = '#include "h.hpp"\nint main() {\n  return gg();\n}\n'
        unit.write_text(good)
        header.write_text('int g() { return 1; }\n')

        # Served once, so the daemon knows the unit's command line.
        code, _, err = env.cxx(['-c', 'a.cpp', '-o', 'warm.o'], scope, env.daemon)
        expect((code, err), (0, b''), 'the first build')
        pid = env.pid()

        ed = Editor(env)
        init = ed.request('initialize', {'processId': os.getpid(), 'rootUri': scope.as_uri(), 'capabilities': {}})
        reply = ed.wait(lambda m: m.get('id') == init, 'initialize')
        if reply is not None:
            expect(reply['result']['capabilities']['textDocumentSync']['change'], 1, 'full text sync')
        ed.notify('initialized', {})

        doc = {'uri': unit.as_uri()}
        ed.notify('textDocument/didOpen', {'textDocument': dict(doc, languageId='cpp', version=1, text=good)})
        expect(ed.diagnostics(unit, 1, 'open'), [], 'a clean unit on open')

        # An unsaved edit: line 3, column 10 in the compiler's terms.
        ed.notify('textDocument/didChange', {'textDocument': dict(doc, version=2), 'contentChanges': [{'text': bad}]})
        expect(ed.diagnostics(unit, 2, 'change'), [(2, 9, 1, "use of undeclared identifier 'gg'")], 'an unsaved error')

        # The build reads the disk, and executes nothing the keystroke could have touched.
        code, _, err = env.cxx(['-c', 'a.cpp', '-o', 'after.o'], scope, env.daemon)
        expect((code, err), (0, b''), 'a build after an unsaved edit')
        expect(env.number('last_executed'), 0, 'cells a build executed after a keystroke')
        code, _, err = env.cxx(['-c', 'a.cpp', '-o', 'cold.o'], scope, env.cold)
        if (scope / 'after.o').read_bytes() != (scope / 'cold.o').read_bytes():
            fail('a build after an unsaved edit is not a cold compile of the disk')

        # An unsaved header reaches its includer; closing it falls back to the disk.
        ed.notify('textDocument/didChange', {'textDocument': dict(doc, version=3), 'contentChanges': [{'text': good}]})
        expect(ed.diagnostics(unit, 3, 'fixed'), [], 'the unit fixed again')
        hdoc = {'uri': header.as_uri()}
        ed.notify('textDocument/didOpen', {'textDocument': dict(hdoc, languageId='cpp', version=1, text='int h();\n')})
        got = ed.diagnostics(unit, 3, 'an unsaved header')
        expect([d[:3] for d in got or []], [(1, 20, 1)], 'an unsaved header breaks its includer')
        ed.notify('textDocument/didClose', {'textDocument': hdoc})
        expect(ed.diagnostics(unit, 3, 'the header closed'), [], 'a closed header is the disk\'s again')

        # A save: the disk now has the error too.
        ed.notify('textDocument/didChange', {'textDocument': dict(doc, version=4), 'contentChanges': [{'text': bad}]})
        expect(len(ed.diagnostics(unit, 4, 'change before save') or []), 1, 'the error before the save')
        unit.write_text(bad)
        ed.notify('textDocument/didSave', {'textDocument': doc})
        expect(ed.diagnostics(unit, 4, 'save'), [(2, 9, 1, "use of undeclared identifier 'gg'")], 'the error after the save')
        code, _, err = env.cxx(['-fsyntax-only', 'a.cpp'], scope, env.daemon)
        expect((code, err), (1, b"a.cpp:3:10: error: use of undeclared identifier 'gg'\n"), 'the saved error in a build')

        shut = ed.request('shutdown', None)
        ed.wait(lambda m: m.get('id') == shut, 'shutdown')
        ed.notify('exit', None)
        try:
            ed.p.wait(timeout=60)
        except subprocess.TimeoutExpired:
            fail('`ycxx lsp` did not exit after `exit`')
        expect(ed.p.returncode, 0, '`ycxx lsp` exit status')
        if ed.p.returncode != 0:
            print(ed.p.stderr.read().decode(errors='replace'), file=sys.stderr)
        env.same_daemon('lsp', pid)
        expect(env.number('diagnoses') >= 7, True, 'the daemon counts its diagnoses')
    finally:
        env.close()
    if failures:
        print(f'lsp: {len(failures)} failure(s)', file=sys.stderr)
        return 1
    print('lsp: all checks passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
