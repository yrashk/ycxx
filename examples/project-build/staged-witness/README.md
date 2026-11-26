# Turn a generated analyzer's output into a typed constant

Put a current `ycxx` on `PATH`. Run these commands from this directory
to try the example in a copy of the workspace.

```sh
example=$(mktemp -d "${TMPDIR:-/tmp}/ycxx-staged-witness.XXXXXX")
cp -R . "$example"
cd "$example"

ycxx build app --trace-engine=json 2> initial.jsonl
status=0
.ycxx/out/app || status=$?
printf '%s\n' "$status"
test "$status" -eq 7

ycxx show app --format=html > plan.html
```

The [source-built rule package](rules/build.ycxx) compiles with `cxx()`.
Its `runner` action copies the executable [analyzer.sh](analyzer.sh) with
`/bin/cp -p` so the named output retains its executable mode. The package
emits this observation.

```text
action.observe "observed" {
    tool = input("analyzer")
    inputs = { analyzer = artifact("runner", "runner") }
    export = "score"
    schema = "example.score.v1"
}
```

The analyzer prints a decimal score. The classifier accepts values from 0 to
255 with surrounding ASCII whitespace and exports a single typed `value`
field in the `score` row. The consumer binds that table in `build.ycxx`.

```text
tools.header "config" {
    facts = { score = value("observed", "score") }
}
```

The header rule writes `inline constexpr int generated_score = 7;` into
`generated_config.hh`, and the app returns that value. In
**Target dependencies**, follow `app` to `config`, then `observed`, then
`runner`. The observer's `tool` fact names the staged executable and the
`input("analyzer")` selector. [Workspace definition](build.ycxx)

## Watch evidence and value edits

Start watch in the copied workspace.

```sh
ycxx build app --watch --trace-engine=interactive
```

1. In another terminal, change `'7'` to `' 7 '` in `analyzer.sh`. The
   producer and witness run again, and the classifier returns the same
   typed score. The build reuses the header and the native app.
2. Change `' 7 '` to `'8'`. The new typed score regenerates the header and
   rebuilds the app. After watch reports success, verify the new value.

```sh
status=0
.ycxx/out/app || status=$?
printf '%s\n' "$status"
test "$status" -eq 8
```

Press `q` in the trace view to stop watch.

## Track a helper executable

After stopping watch, make these changes in the copied workspace. The
staged analyzer runs the helper passed as its first argument.

```sh
cat > analyzer.sh <<'SH'
#!/bin/sh
set -eu
exec "$1"
SH

cat > score-helper.sh <<'SH'
#!/bin/sh
set -eu
printf '%s\n' '7'
SH

chmod +x analyzer.sh score-helper.sh
printf '%s/score-helper.sh\n' "$(pwd -P)"
```

Replace the `observe` declaration inside the C++ raw string in
`rules/staged_witness.cc` with the following. Replace both placeholder
paths with the absolute path printed above.

```text
action.observe "observed" {
    tool = input("analyzer")
    args = ["/absolute/path/to/copied-workspace/score-helper.sh"]
    inputs = { analyzer = artifact("runner", "runner") }
    tool_inputs = { score_helper = "/absolute/path/to/copied-workspace/score-helper.sh" }
    export = "score"
    schema = "example.score.v1"
}
```

`args` passes the path to the analyzer. `tool_inputs` tracks the helper's
identity at that path. Build once and inspect the `score_helper` tool-input
fact in the HTML plan. It shows the declared path, canonical path, and
fingerprint.

```sh
ycxx build app
ycxx show app --format=html > helper-plan.html
ycxx build app --watch --trace-engine=interactive
```

Edit only `score-helper.sh` in a second terminal.

1. Change `'7'` to `' 7 '`. Watch reruns the witness and classifier. The
   classified score stays at 7, so the build reuses the header and app.
2. Change `' 7 '` to `'8'`. Watch reruns the witness and classifier, and the
   changed score rebuilds the header and app.

After the second build, verify that the app returns 8 and refresh the plan to
see the updated helper fingerprint.

```sh
status=0
.ycxx/out/app || status=$?
printf '%s\n' "$status"
test "$status" -eq 8
ycxx show app --format=html > helper-plan.html
```

Run `chmod a-x score-helper.sh` while watch is active to see a helper
permission error. The published app still exits with 8. Run
`chmod u+x score-helper.sh` to let watch recover.
