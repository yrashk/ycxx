# Build a native leaf rule package

The package pins the current driver with `tool = cxx("ycxx")` and builds a
freestanding shared library for the host. `-Iinputs/sdk` makes the staged rule
API available. The library uses the worker's C ABI callbacks to emit build
definitions. It contains immutable strings and code with no C++ runtime calls.
[Package definition](rules/build.ycxx) · [Rule entry point](rules/leaf.cc)

The workspace uses `leaf.header` to generate `leaf_score.hh` and
`leaf.executable` to compile `main.cc` with that header. The generated constant
is the app's exit status. [Workspace definition](build.ycxx)

## Run and inspect

From this directory, run the following with a current `ycxx` on `PATH` on
arm64 macOS or x86-64 Linux. On macOS the package uses the platform SDK's C
headers.

```sh
example=$(mktemp -d "${TMPDIR:-/tmp}/ycxx-native-leaf-rule.XXXXXX")
cp -R . "$example"
cd "$example"

ycxx build app --probe-toolchain --trace-engine=json 2> first.jsonl
status=0
./.ycxx/out/app || status=$?
printf 'app exit status %s\n' "$status"
test "$status" -eq 17
ycxx show app --format=json > plan.json
ycxx show app --format=html > plan.html
ycxx build app --probe-toolchain --trace-engine=json 2> warm.jsonl
```

In `first.jsonl`, `RulePackage` records the compiler used for `rules/package`.
`RuleWorker` records the `app` and `score` expansions. Open `plan.html` and
find `Selected native compiler` beside `rules/package`. The probes
`object.host-shared`, `link.host-shared`, and `link.host-shared.load` cover the
PIC object, shared library link, and a host loader call to the rule entry
point. The plan also shows the `score` header edge to the app.

With the same inputs, the resident daemon reuses the package, probe results,
expansions and app. The final record in `warm.jsonl` reports
`actions_executed` equal to `0`.
