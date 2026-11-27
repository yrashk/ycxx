"""Report observable watch milestones from timestamped raw stderr and file edits."""

import json
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


root = Path(sys.argv[1])
phases = {}
for phase in ('serial', 'parallel'):
    observations = []
    for path in sorted((root / phase / 'watch').glob('*.json')):
        source = json.loads(path.read_text(encoding='utf-8'))
        observations.append({'script': source['script'], 'attempts': [
            timing for watch in source['watches'] for timing in summarize(watch)
        ]})
    phases[phase] = observations
(root / 'milestones.json').write_text(json.dumps(phases, indent=2) + '\n', encoding='utf-8')
for phase, observations in phases.items():
    for fixture in observations:
        for attempt in fixture['attempts']:
            print('YCXX_WATCH_TIMING ' + json.dumps({
                'phase': phase, 'script': fixture['script'],
                **{k: attempt.get(k) for k in ('label', 'build', 'outcome', 'status', 'seconds')},
            }, separators=(',', ':')))
    expected_observers = 2 if phase == 'serial' else 5
    if sum(bool(fixture['attempts']) for fixture in observations) != expected_observers:
        print(f'::warning::{phase} observer saw fewer than {expected_observers} watch fixtures')
