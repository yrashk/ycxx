# Link a library built with Make

Use Linux x86-64 or macOS arm64 with `ycxx` on `PATH`, GNU Make, and the system C
and C++ tools installed. If `make`, `cc`, or `ar` is installed elsewhere, set its
absolute path in `build.ycxx`. Run these commands from this directory.

```sh
example=$(mktemp -d "${TMPDIR:-/tmp}/ycxx-foreign-make.XXXXXX")
cp -R . "$example"
cd "$example"

ycxx build app --trace-engine=json 2> first.jsonl
status=0
.ycxx/out/app || status=$?
printf '%s\n' "$status"
test "$status" -eq 42

ycxx show app --format=html > plan.html
ycxx build app --trace-engine=json 2> unchanged.jsonl
```

The `foreign_score` action runs the vendor Makefile's `archive` target. The
rule package stages the Makefile and the paths in `vendor/files.txt`. Inventory
paths are relative to the Makefile's directory. Make compiles `score.c` with
its two headers and writes a host-format indexed static archive. The `bridge`
library calls the archive's C function and exports `libforeign.a` to the app
through `public_link_inputs`. The app returns the score, initially `35 + 7`.

Open `plan.html` to see the Make command, the staged vendor files, the `cc`
and `ar` helper executables, and the app's link input inherited from `bridge`.
The first trace includes the `foreign_score` external action and the native
link. The unchanged trace reports zero actions.

The vendor Makefile also runs independently with `make -C vendor archive`. It
creates `vendor/libforeign.a`. When it runs through ycxx, the action tracks
the Make binary, the declared arguments and environment, and the staged
files. Make receives the compiler and archiver paths in `CC` and `AR`. The
rule also declares both paths in `tool_inputs`, so their executable contents
enter the action key and watch follows changes at those paths.

## Watch a helper executable edit

In the copied workspace, create a compiler wrapper that delegates to the
system compiler.

```sh
cat > cc-helper <<'SH'
#!/bin/sh
exec /usr/bin/cc "$@"
SH
chmod +x cc-helper
printf '%s\n' "$PWD/cc-helper"
```

Set `cc` in `build.ycxx` to the absolute path printed above. Start watch in a
second terminal and wait for the initial build to finish.

```sh
ycxx build app --watch --trace-engine=interactive
```

From the first terminal, change the wrapper at the same path.

```sh
printf '%s\n' '# Same compiler invocation.' >> cc-helper
```

Watch reruns `foreign_score`. The wrapper invokes the same compiler with the
same arguments, so the archive bytes and native link remain reusable. Run
`ycxx show app` to see the declared wrapper path and its updated fingerprint.
Press `q` to stop watch.

## Watch a vendor header edit

Start watch in a second terminal in the copied workspace.

```sh
ycxx build app --watch --trace-engine=interactive
```

After the initial build, change `SCORE_BONUS` from `7` to `8` in
`vendor/include/score_config.h`. Watch reruns `foreign_score` and links the new
archive. The native translation units reuse their results. When watch reports
success, the executable exits with status `43`.

```sh
status=0
.ycxx/out/app || status=$?
printf '%s\n' "$status"
test "$status" -eq 43
```

Add a comment to the header to see Make run again. If the archive bytes are
equal, the native link also reuses its result. When a new source or header
joins the Make build, add it to `vendor/files.txt` so watch follows it.

Append `#error bad vendor edit` to `vendor/src/score.c` to see Make fail. The
last successful executable still exits with status `43`. Remove the line to
recover, then press `q` in the trace view to stop watch.
