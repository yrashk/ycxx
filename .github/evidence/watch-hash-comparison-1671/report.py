"""Report observable watch milestones from timestamped raw stderr and file edits."""

import json
from collections import defaultdict
from pathlib import Path
import sys


def seconds(clock, start, end):
    if start not in clock or end not in clock:
        return None
    return round((clock[end] - clock[start]) / 1_000_000_000, 4)


def summarize(watch):
    clock = {'launch': watch['events'][0][0]}
    result, edits, metadata = [], [], {}
    for timestamp, event, *detail in sorted(watch['events'], key=lambda record: record[0]):
        if event == 'edit':
            clock['edit'] = timestamp
            edits.append({'operation': detail[0], 'path': detail[1], 'before_ns': detail[2], 'after_ns': timestamp})
        elif event == 'await':
            clock['await'] = timestamp
            metadata.update(label=detail[0], load_at_await=detail[1])
        elif event == 'stderr' and detail[0]:
            try:
                record = json.loads(detail[0])
            except ValueError:
                continue
            if 'build' in record:
                clock['start'] = timestamp
                metadata['build'] = record['build']
            elif 'kind' in record:
                clock.setdefault('first_trace', timestamp)
                clock['last_trace'] = timestamp
                metadata['last_trace_event'] = {k: record[k] for k in ('kind', 'key', 'outcome') if k in record}
            elif 'summary' in record:
                metadata['engine_summary'] = record['summary']
            elif 'actions_executed' in record:
                clock['summary'] = timestamp
                metadata['status'] = record['status']
                metadata['actions_executed'] = record['actions_executed']
            elif 'watching' in record:
                clock['watching'] = timestamp
                metadata['watched_count'] = len(record['watching'])
        elif event in ('return', 'error'):
            clock['finish'] = timestamp
            first = 'edit' if 'edit' in clock else 'launch'
            result.append({
                **metadata, 'outcome': event, 'error': detail[0] if event == 'error' else None,
                'edits': edits, 'time_ns': clock,
                'seconds': {
                    'edit_or_launch_to_start': seconds(clock, first, 'start'),
                    'start_to_first_trace': seconds(clock, 'start', 'first_trace'),
                    'start_to_summary': seconds(clock, 'start', 'summary'),
                    'summary_to_watching': seconds(clock, 'summary', 'watching'),
                    'summary_to_finish': seconds(clock, 'summary', 'finish'),
                    'await_to_finish': seconds(clock, 'await', 'finish'),
                },
            })
            clock, edits, metadata = {}, [], {}
    return result



def measurements(path):
    rows = []
    for line in path.read_text(encoding='utf-8').splitlines():
        kind, ended, cwd, *fields = line.split('\t')
        row = {'kind': kind, 'ended_utc_ns': int(ended), 'cwd': cwd}
        if kind == 'discovery':
            package, count, elapsed = fields
            row.update(package=package, candidates=int(count), discovery_ns=int(elapsed))
        elif kind == 'hash':
            spelling, path, size, read, hashed, digest = fields
            row.update(spelling=spelling, resolved=path, bytes=int(size),
                       read_ns=int(read), hash_ns=int(hashed), fingerprint=digest)
        elif kind == 'cache-hit':
            spelling, path, elapsed, digest = fields
            row.update(spelling=spelling, resolved=path,
                       cache_hit_check_ns=int(elapsed), fingerprint=digest)
        else:
            raise ValueError(kind)
        rows.append(row)
    return rows


def aggregate(rows):
    discovered = [row for row in rows if row['kind'] == 'discovery']
    packages = defaultdict(list)
    for row in discovered:
        packages[f"{row['cwd']}:{row['package']}"].append(row)
    tools = defaultdict(list)
    for row in rows:
        if row['kind'] in ('hash', 'cache-hit'):
            tools[row['resolved']].append(row)
    return {
        'discovery_calls': len(discovered),
        'discovery_seconds': round(sum(row['discovery_ns'] for row in discovered) / 1e9, 6),
        'discovery_by_package': {
            path: {'calls': len(items), 'candidates': sorted({row['candidates'] for row in items})}
            for path, items in sorted(packages.items())
        },
        'executables': {
            path: {
                'reads': sum(row['kind'] == 'hash' for row in observations),
                'cache_hits': sum(row['kind'] == 'cache-hit' for row in observations),
                'bytes': sum(row.get('bytes', 0) for row in observations),
                'read_seconds': round(sum(row.get('read_ns', 0) for row in observations) / 1e9, 6),
                'hash_seconds': round(sum(row.get('hash_ns', 0) for row in observations) / 1e9, 6),
                'cache_hit_check_seconds': round(
                    sum(row.get('cache_hit_check_ns', 0) for row in observations) / 1e9, 6),
                'fingerprints': sorted({row['fingerprint'] for row in observations}),
                'spellings': sorted({row['spelling'] for row in observations}),
            }
            for path, observations in sorted(tools.items())
        },
    }


def main(root):
    hashes = measurements(root / 'compiler-hash.tsv')
    recorded = []
    for path in sorted((root / 'watch').glob('*.json')):
        source = json.loads(path.read_text(encoding='utf-8'))
        utc_offset = source['flushed_at_utc_ns'] - source['flushed_at_monotonic_ns']
        for watch in source['watches']:
            for attempt in summarize(watch):
                clock = attempt['time_ns']
                begin = clock.get('start', clock['await']) + utc_offset
                end = clock['finish'] + utc_offset
                attempt['compiler_observations'] = aggregate([
                    row for row in hashes
                    if begin <= row['ended_utc_ns'] <= end
                    and Path(row['cwd']).is_relative_to(Path(watch['scope']))
                ])
                recorded.append(attempt)
        packages = source['rule_packages']
    assert recorded and hashes, 'No staged witness instrumentation captured.'
    changed = [entry for entry in recorded if entry['label'] == 'change the typed score']
    assert len(changed) == 1, changed
    metrics = {'all': aggregate(hashes), 'staged_witness': changed[0],
               'watch_attempts': recorded, 'rule_packages': packages}
    candidates = json.loads((root / 'candidate-files.json').read_text(encoding='utf-8'))
    clang, = (entry for entry in candidates if entry['name'] == 'clang++')
    baseline_sha256 = 'a22ab66d53cf70dfee8544eabdeb36f075bf942861cb8d59f98a3fadc7a65c26'
    if clang['sha256'] == baseline_sha256:
        actual = metrics['all']['executables'][clang['resolved']]['fingerprints']
        expected = ['04da3044ce23b32b9ec66cdb758f665e']
        assert actual == expected, (clang, actual, expected)
        metrics['baseline_compiler_digest_preserved'] = True
    else:
        metrics['baseline_compiler_digest_preserved'] = None
    (root / 'comparison.json').write_text(json.dumps(metrics, indent=2) + '\n', encoding='utf-8')
    print('YCXX_HASH_COMPARISON ' + json.dumps(changed[0], separators=(',', ':')))
    print('YCXX_RULE_PACKAGES ' + json.dumps(
        [{key: row[key] for key in ('kind', 'key', 'outcome')} for row in packages],
        separators=(',', ':')))


if __name__ == '__main__':
    main(Path(sys.argv[1]))
