"""The report of a run: build/test-logs/report.{html,md,tsv,json}.

The HTML page is libycxx's suite report (deps/libycxx/tools/lib/test_report.py's page and
report_common.py's styles, theme switch and copy buttons): every test with its result, steps
and full transcript, passing ones too, filters, search, and copy buttons that put
self-contained Markdown on the clipboard for a person or an agent. The page embeds the whole
report as Markdown, which is also written as report.md. report.tsv has one line per test, and
report.json the run and every test, for tools.
"""
import html, json, os, re, sys
from pathlib import Path

GOOD = {'PASS', 'XFAIL'}
SKIP = {'UNSUPPORTED'}
ORDER = ['PASS', 'XFAIL', 'FAIL', 'XPASS', 'UNRESOLVED', 'TIMEOUT', 'UNSUPPORTED']


def fence(text):
    f = '```'
    while f in text:
        f += '`'
    return f'{f}\n{text.rstrip()}\n{f}'


def counts_of(tests):
    c = {}
    for t in tests:
        c[t['code']] = c.get(t['code'], 0) + 1
    return dict(sorted(c.items(), key=lambda kv: ORDER.index(kv[0]) if kv[0] in ORDER else len(ORDER)))


def unsupported_reasons(tests):
    r = {}
    for t in tests:
        if t['code'] == 'UNSUPPORTED':
            m = re.search(r'^unsupported: (.*)$', t['output'], re.M)
            k = m[1] if m else '?'
            r[k] = r.get(k, 0) + 1
    return dict(sorted(r.items(), key=lambda kv: -kv[1]))


def markdown(title, meta, stages, tests):
    bad = [t for t in tests if t['code'] not in GOOD | SKIP]
    out = [f'# {title}', '']
    out += [f'- {k}: {v}' for k, v in meta.items()]
    out += ['', 'Results: ' + (', '.join(f'{c} {n}' for c, n in counts_of(tests).items()) or 'no tests')
            + f' (of {len(tests)})', '']
    if stages:
        out += ['## Stages', '', '| stage | result | time | detail |', '|---|---|---:|---|']
        out += [f'| {s["name"]} | {s["status"]} | {s["secs"]:.1f}s | {s["detail"]} |' for s in stages]
        out.append('')
    reasons = unsupported_reasons(tests)
    if reasons:
        out += ['## Why tests were unsupported', '']
        out += [f'- {n} × {r}' for r, n in reasons.items()]
        out.append('')
    if bad:
        out += [f'## Failures ({len(bad)})', '']
        for t in bad:
            out += [f'### {t["code"]}: {t["name"]}', '', f'- source: tests/{t["name"]}',
                    f'- steps: {"; ".join(t["steps"]) or "none recorded"}', '',
                    fence(t['output'] or '(no output)'), '']
    else:
        out += ['No failures.', '']
    out += [f'## All tests ({len(tests)})', '', 'Each line: result, test, stage, the steps it ran.', '']
    out += [f'- {t["code"]} {t["name"]} [{t["stage"]}] — {"; ".join(t["steps"]) or t["error"]}' for t in tests]
    return '\n'.join(out) + '\n'


def github_summary(title, meta, tests, report_path, limit=50):
    bad = [t for t in tests if t['code'] not in GOOD | SKIP]
    out = [f'### {title}', '', ', '.join(f'{c} {n}' for c, n in counts_of(tests).items()) or 'no tests', '']
    for t in bad[:limit]:
        out.append(f'- **{t["code"]}** `{t["name"]}`: {t["error"]}')
    if len(bad) > limit:
        out.append(f'- ... {len(bad) - limit} more')
    out += ['', f'Full report with every transcript: `{report_path}` (in the test-logs artifact).', '']
    return '\n'.join(out)


def write(base, title, meta, stages, tests, repo):
    """tests: dicts with name, code, secs, steps, error, output, stage, kind."""
    base = Path(base)
    base.parent.mkdir(parents=True, exist_ok=True)
    tests = sorted(tests, key=lambda t: (t['code'] in GOOD or t['code'] in SKIP, t['name']))
    md = markdown(title, meta, stages, tests)
    base.with_suffix('.md').write_text(md)
    with open(base.with_suffix('.tsv'), 'w', encoding='utf-8') as f:
        f.write('result\tseconds\ttest\tstage\tkind\tsteps\terror\n')
        for t in tests:
            f.write(f"{t['code']}\t{t['secs']:.3f}\t{t['name']}\t{t['stage']}\t{t['kind']}\t"
                    f"{'; '.join(t['steps'])}\t{t['error']}\n")
    base.with_suffix('.json').write_text(json.dumps(
        {'title': title, 'meta': meta, 'stages': stages, 'counts': counts_of(tests), 'tests': tests}, indent=1))
    if os.environ.get('GITHUB_STEP_SUMMARY'):
        with open(os.environ['GITHUB_STEP_SUMMARY'], 'a', encoding='utf-8') as f:
            f.write(github_summary(title, meta, tests, os.path.relpath(base.with_suffix('.html'), repo)))
    lib = repo / 'deps' / 'libycxx' / 'tools' / 'lib'
    if not (lib / 'test_report.py').is_file():
        return None  # the submodule is not checked out: Markdown, TSV and JSON only
    sys.path.insert(0, str(lib))
    import report_common, test_report  # libycxx's

    def embed(value):
        return json.dumps(value, separators=(',', ':')).replace('</', '<\\/')

    rows = ''.join(f'<tr><th>{html.escape(k)}</th><td>{html.escape(str(v))}</td></tr>' for k, v in meta.items())
    for s in stages:
        rows += (f'<tr><th>stage {html.escape(s["name"])}</th><td>{html.escape(s["status"])}, '
                 f'{s["secs"]:.1f}s: {html.escape(s["detail"])}</td></tr>')
    chips = ''.join(
        f'<button class="chip {"good" if c in GOOD else "skip" if c in SKIP else "bad"}" data-code="{c}">'
        f'{c} <b>{n}</b></button>' for c, n in counts_of(tests).items())
    page = test_report.PAGE.replace('libycxx test report', 'ycxx test report')
    # Long step lists (incremental tests) must not squeeze the test's name.
    page = page.replace('</style>', '.steps { max-width: 40ch; }\n</style>', 1)
    page = page.replace('@CSS@', report_common.CSS).replace('@THEMEHEAD@', report_common.THEME_HEAD)
    page = page.replace('@COPYJS@', report_common.JS)
    page = page.replace('@TITLE@', html.escape(title)).replace('@META@', rows).replace('@CHIPS@', chips)
    page = page.replace('@TOTAL@', str(len(tests))).replace('@DATA@', embed(tests))
    page = page.replace('@RUN@', embed({'title': title, 'meta': meta, 'root': str(repo / 'tests')}))
    page = page.replace('@MD@', re.sub(r'</(script)', r'<\\/\1', md, flags=re.I))
    base.with_suffix('.html').write_text(page)
    return base.with_suffix('.html')
