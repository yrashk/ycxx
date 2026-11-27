"""Report observable watch milestones from timestamped raw stderr and file edits."""

import json
from pathlib import Path
import sys


def seconds(clock, start, end):
    if start not in clock or end not in clock:
        return None
    return round((clock[end] - clock[start]) / 1_000_000_000, 4)


def summarize(watch):
    # The client can deliver the next build header while the fixture is still
    # returning from the previous acknowledgement. Group the wire stream by
    # those explicit headers rather than by Python call boundaries.
    frames, calls, edits = [], [], []
    for timestamp, event, *detail in sorted(watch['events'], key=lambda record: record[0]):
        if event == 'edit':
            edits.append({'operation': detail[0], 'path': detail[1], 'before_ns': detail[2], 'after_ns': timestamp})
        elif event == 'await':
            calls.append({'label': detail[0], 'load_at_await': detail[1], 'edits': edits, 'await': timestamp})
            edits = []
        elif event == 'stderr' and detail[0]:
            try:
                record = json.loads(detail[0])
            except ValueError:
                continue
            if 'build' in record:
                frames.append({'build': record['build'], 'start': timestamp, 'records': []})
            elif frames:
                frames[-1]['records'].append((timestamp, record))
        elif event in ('return', 'error'):
            calls[-1].update(finish=timestamp, outcome=event, error=detail if event == 'error' else None)

    result = []
    for index, call in enumerate(calls):
        outcome = call.get('outcome', 'incomplete')
        clock = {key: call[key] for key in ('await', 'finish') if key in call}
        if call['edits']:
            clock['edit'] = call['edits'][-1]['after_ns']
        if index == 0:
            clock['launch'] = watch['events'][0][0]
        metadata = {}
        late_stderr = {}
        if index < len(frames):
            frame = frames[index]
            metadata['build'] = frame['build']
            if frame['start'] > clock.get('finish', frame['start']):
                late_stderr['start'] = frame['start']
            else:
                clock['start'] = frame['start']
            for timestamp, record in frame['records']:
                late = timestamp > clock.get('finish', timestamp)
                if 'kind' in record and not late:
                    clock.setdefault('first_trace', timestamp)
                    clock['last_trace'] = timestamp
                    metadata['last_trace_event'] = {k: record[k] for k in ('kind', 'key', 'outcome') if k in record}
                elif 'summary' in record and not late:
                    metadata['engine_summary'] = record['summary']
                elif 'actions_executed' in record:
                    if late:
                        late_stderr['summary'] = timestamp
                    else:
                        clock['summary'] = timestamp
                        metadata['status'] = record['status']
                        metadata['actions_executed'] = record['actions_executed']
                elif 'watching' in record:
                    if late:
                        late_stderr['watching'] = timestamp
                    else:
                        clock['watching'] = timestamp
                        metadata['watched_count'] = len(record['watching'])
        first = 'edit' if 'edit' in clock else 'launch'
        result.append({
            **metadata, 'label': call['label'], 'outcome': outcome, 'error': call.get('error'),
            'watch_timeout': any('watch timed out' in part for part in call.get('error') or []),
            'load_at_await': call['load_at_await'], 'edits': call['edits'], 'time_ns': clock, 'late_stderr': late_stderr,
            'seconds': {
                'edit_or_launch_to_start': seconds(clock, first, 'start'),
                'start_to_first_trace': seconds(clock, 'start', 'first_trace'),
                'start_to_summary': seconds(clock, 'start', 'summary'),
                'summary_to_watching': seconds(clock, 'summary', 'watching'),
                'summary_to_finish': seconds(clock, 'summary', 'finish'),
                'await_to_finish': seconds(clock, 'await', 'finish'),
            },
        })
    if result and len(frames) > len(calls):
        result[-1]['unmatched_build_headers'] = [
            {'build': frame['build'], 'at_ns': frame['start']} for frame in frames[len(calls):]
        ]
    return result


def main(root):
    phases = {}
    print((root / 'provenance.txt').read_text(encoding='utf-8'))
    for phase in ('serial', 'parallel'):
        observations = []
        phase_root = root / phase
        if not phase_root.is_dir():
            print(f'::warning::{phase} run directory is missing')
            continue
        load_file = phase_root / 'load.jsonl'
        loads = [json.loads(line) for line in load_file.read_text().splitlines()] if load_file.is_file() else []
        for name in ('exit.txt', 'binary.txt'):
            file = phase_root / name
            if file.is_file():
                print(f'{phase} {name} {file.read_text().strip()}')
        for path in sorted((phase_root / 'watch').glob('*.json')):
            source = json.loads(path.read_text(encoding='utf-8'))
            observations.append({'script': source['script'], 'attempts': [
                timing for watch in source['watches'] for timing in summarize(watch)
            ]})
        phases[phase] = observations
        metadata = {'phase': phase, 'samples': len(loads)}
        if loads:
            metadata.update(cpu_count=loads[0]['logical_cpus'], min_load_1m=min(x['load'][0] for x in loads),
                            max_load_1m=max(x['load'][0] for x in loads))
        print('YCXX_PHASE_LOAD ' + json.dumps(metadata, separators=(',', ':')))
        for fixture in observations:
            for attempt in fixture['attempts']:
                print('YCXX_WATCH_TIMING ' + json.dumps({
                    'phase': phase, 'script': fixture['script'],
                    **{k: attempt.get(k) for k in ('label', 'build', 'outcome', 'status', 'seconds', 'late_stderr',
                                                   'watch_timeout', 'unmatched_build_headers',
                                                   'load_at_await', 'last_trace_event', 'engine_summary')},
                }, separators=(',', ':')))
        expected_observers = 2 if phase == 'serial' else 5
        if sum(bool(fixture['attempts']) for fixture in observations) != expected_observers:
            print(f'::warning::{phase} observer saw fewer than {expected_observers} watch fixtures')
    (root / 'milestones-corrected.json').write_text(json.dumps(phases, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main(Path(sys.argv[1]))
