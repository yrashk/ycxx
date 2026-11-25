# Generate a header for the selected target

The `target_header` action passes `target_triple()` to a host shell script as
one argument. The script generates `build_target.hh` with the score from
`score.txt`. A `static_assert` checks the architecture and platform macros
of the compiler that consumes the header. This connects the generator argument
to the actual target of the native build.

From this directory, with `ycxx` on `PATH`, copy the workspace and build both
target configurations.

```sh
example=$(mktemp -d "${TMPDIR:-/tmp}/ycxx-target-action.XXXXXX")
cp build.ycxx generate-header.sh main.cc score.txt "$example"
cd "$example"

ycxx build app --target=x86_64-pc-linux-gnu --trace-engine=json 2> linux.jsonl
ycxx build app --target=arm64-apple-darwin --trace-engine=json 2> darwin.jsonl
file .ycxx/targets/x86_64-pc-linux-gnu/out/app .ycxx/targets/arm64-apple-darwin/out/app

ycxx show app --target=x86_64-pc-linux-gnu --format=html > linux-plan.html
ycxx show app --target=arm64-apple-darwin --format=html > darwin-plan.html
```

Open both plans to see the resolved target argument and the generated header
input on `app`. Each explicit build produces an executable in its own target
directory. Rebuilding either target with the same inputs reuses its action.

On x86-64 Linux, run `.ycxx/targets/x86_64-pc-linux-gnu/out/app`. On arm64
macOS, run `.ycxx/targets/arm64-apple-darwin/out/app`. Each exits with status
`42` on its matching host.

## Watch the host target

Run watch in a second terminal in the copied workspace. The default build
also passes its canonical target triple to the generator.

```sh
ycxx build app --watch --trace-engine=interactive
```

After watch reports success, run `.ycxx/out/app` on the matching host. It
exits with status `42`. Change the score in the first terminal to see watch
regenerate the header and rebuild the app.

```sh
printf '%s\n' 43 > score.txt
```

When watch reports success again, check the new status.

```sh
status=0
.ycxx/out/app || status=$?
printf '%s\n' "$status"
test "$status" -eq 43
```

Press `q` in the trace view to stop watch.
