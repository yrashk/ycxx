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

The `module-provider` facts show the resolved or external status and source
provenance. Open `linux-to-darwin.html` to compare both branches of the
diamond. Each branch resolves `demo.shared` to the same file for a given
target.

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
```

Press `q` in the trace view to stop watch.

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
