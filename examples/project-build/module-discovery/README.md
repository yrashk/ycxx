# Discover named C++ module dependencies

The project declares three module interfaces. `main.cc` imports `demo.score`
on both targets. The compiler's `__linux__` and `__APPLE__` macros select
`demo.linux` or `demo.darwin`. The importer also contains import spellings in
a comment and a raw string to exercise discovery through the preprocessor.

From this directory, with `ycxx` on `PATH`, copy the workspace and request both
target builds. Discovery runs before the current native module diagnostics.
Expect a nonzero build status while module compilation proceeds in
[#319](https://github.com/yrashk/ycxx/issues/319).

```sh
example=$(mktemp -d "${TMPDIR:-/tmp}/ycxx-module-discovery.XXXXXX")
cp build.ycxx main.cc score.cc bonus_linux.cc bonus_darwin.cc "$example"
cd "$example"

for target in x86_64-pc-linux-gnu arm64-apple-darwin; do
    status=0
    ycxx build app --target="$target" --trace-engine=json 2> "$target-build.jsonl" || status=$?
    printf '%s build status %s\n' "$target" "$status"
    ycxx show app --target="$target" --format=json > "$target-plan.json"
    ycxx show app --target="$target" --format=html > "$target-plan.html"
done
```

Compare the two plans. The `module-unit` facts show the three interfaces, and
the `module-import` facts show the imports and their source locations. The
Linux plan connects `main.cc` to `demo.score` and `demo.linux`. The Darwin
plan connects it to `demo.score` and `demo.darwin`.

## Watch a module source edit

Start watch for the Linux configuration in a second terminal in the same
workspace.

```sh
ycxx build app --target=x86_64-pc-linux-gnu --watch --trace-engine=interactive
```

After watch reports the initial build, change the function body in `score.cc`
from the first terminal.

```sh
sed 's/return 35;/return 36;/' score.cc > score.cc.next
mv score.cc.next score.cc
```

After the next watch build, press `/` in the trace to filter on
`kind:ModuleDirectives` for per-source scans or `kind:ModuleGraph` for the
aggregate result. Compare the plan with the snapshot from before the edit to
see the same module providers and imports.

```sh
ycxx show app --target=x86_64-pc-linux-gnu --against x86_64-pc-linux-gnu-plan.json \
    --format=html > linux-edit.html
```

Press `q` in the trace view to stop watch.
