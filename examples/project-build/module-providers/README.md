# Resolve named module providers

`demo.top` imports `demo.left` and `demo.right`. Each branch imports
`demo.shared`. The compiler's platform macros select `shared_linux.cc` as
the provider on Linux and `shared_darwin.cc` on Darwin. `main.cc` also imports
`demo.extension`, which the provider report classifies as external.

From this directory, with `ycxx` on `PATH`, copy the workspace and request both
targets with the same `build.ycxx`. Expect build status `1` after provider
reporting. Named-module compilation and BMI support are tracked in [#319](https://github.com/yrashk/ycxx/issues/319).

```sh
example=$(mktemp -d "${TMPDIR:-/tmp}/ycxx-module-providers.XXXXXX")
cp build.ycxx *.cc "$example"
cd "$example"

for target in x86_64-pc-linux-gnu arm64-apple-darwin; do
    status=0
    ycxx build app --target="$target" --trace-engine=json 2> "$target-build.jsonl" || status=$?
    printf '%s build status %s\n' "$target" "$status"
    test "$status" -eq 1
    ycxx show app --target="$target" --format=json > "$target-plan.json"
    ycxx show app --target="$target" --format=html > "$target-plan.html"
done

ycxx show app --target=arm64-apple-darwin --against x86_64-pc-linux-gnu-plan.json \
    --format=html > linux-to-darwin.html
```

## Follow provider links in HTML

Open `x86_64-pc-linux-gnu-plan.html` in a browser and find **Module graph**.
In the panels for `left.cc` and `right.cc`, follow each `demo.shared` import's
candidate link to its declaration at `shared_linux.cc:2:1`. In
`arm64-apple-darwin-plan.html`, both links land at `shared_darwin.cc:2:1`.
The import sites are `left.cc:3:1` and `right.cc:3:1` in both reports.

Open `linux-to-darwin.html` to see what changed. **Plan changes** compares the
saved Linux snapshot with the current Darwin plan. **Module graph** below it
shows the current Darwin graph.

## Compare provider edges in JSON

Run this in the copied workspace after the builds above. The comparison records
each changed import with its original `before` and `after` data. Python 3 uses
the Linux snapshot for `before` candidates and the Darwin snapshot for `after`
candidates. Unit IDs and directive indexes refer to their own snapshots.

```sh
ycxx show app --target=arm64-apple-darwin --against x86_64-pc-linux-gnu-plan.json \
    --format=json > linux-to-darwin.json

python3 - <<'PY'
import json
from pathlib import Path

def read_json(path):
    return json.loads(Path(path).read_text(encoding='utf-8'))

def declarations(path):
    graph = read_json(path)['module_graph']
    return {
        (unit['id'], declaration['directive_index']): declaration['location']
        for unit in graph['units']
        for declaration in unit['declarations']
    }

def site(location):
    return f'{location["path"]}:{location["line"]}:{location["column"]}'

linux = declarations('x86_64-pc-linux-gnu-plan.json')
darwin = declarations('arm64-apple-darwin-plan.json')
changes = read_json('linux-to-darwin.json')['module_changes']
for row in changes['imports']:
    if row['change'] != 'changed' or row['after']['name'] != 'demo.shared':
        continue
    before, after = row['before'], row['after']
    print(f'{after["name"]} at {site(after["location"])}')
    for target, edge, sites in [('Linux', before, linux), ('Darwin', after, darwin)]:
        for candidate in edge['candidates']:
            key = candidate['unit_id'], candidate['directive_index']
            print(f'  {target}: {site(sites[key])} ({edge["status"]})')
PY
```

Both `demo.shared` imports are `resolved` in each variant. For each import on
line 3 of `left.cc` or `right.cc`, the candidate changes from
`shared_linux.cc:2:1` to `shared_darwin.cc:2:1`.

## Watch a provider body edit

In a second terminal in the copied workspace, start watch for Linux.

```sh
ycxx build app --target=x86_64-pc-linux-gnu --watch --trace-engine=interactive
```

After the initial build, change the function body in the selected provider.

```sh
sed 's/return 7;/return 8;/' shared_linux.cc > shared_linux.cc.next
mv shared_linux.cc.next shared_linux.cc
```

After watch sees the edit, press `/` and filter on `kind:ModuleProviders` to
inspect the semantic provider result or `kind:ModuleProviderReport` to inspect
the report with source sites. The body edit lets the build reuse the provider
resolution. Compare the plan with the saved Linux snapshot.

```sh
ycxx show app --target=x86_64-pc-linux-gnu --against x86_64-pc-linux-gnu-plan.json \
    --format=html > linux-body-edit.html
ycxx show app --target=x86_64-pc-linux-gnu --against x86_64-pc-linux-gnu-plan.json \
    --format=json > linux-body-edit.json
```

The body edit leaves all four `module_changes` arrays empty. Press `q` in the
trace view to stop watch.

## See ambiguous providers

In the copied workspace, enable the Darwin provider's declaration on Linux as
well. The Linux plan now lists both candidates for each `demo.shared` import.

```sh
sed 's/defined(__APPLE__)/defined(__APPLE__) || defined(__linux__)/' \
    shared_darwin.cc > shared_darwin.cc.next
mv shared_darwin.cc.next shared_darwin.cc

status=0
ycxx build app --target=x86_64-pc-linux-gnu --trace-engine=json 2> ambiguous-build.jsonl || status=$?
test "$status" -eq 1
ycxx show app --target=x86_64-pc-linux-gnu --against x86_64-pc-linux-gnu-plan.json \
    --format=html > ambiguous-plan.html
```

Open `ambiguous-plan.html` and find **Module graph**. Each `demo.shared` import
now shows an ambiguous status with links to `shared_linux.cc:2:1` and
`shared_darwin.cc:2:1`. The `demo.shared` conflict links to both declarations.
