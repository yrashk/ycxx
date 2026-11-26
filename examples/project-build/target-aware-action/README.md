# Generate headers for the host and selected target

Both generators run on the host. The `host_header` action passes
`host_triple()` to a shell command and records it as a C++ constant in
`build_host.hh`. The `target_header` action passes `target_triple()` to
`generate-header.sh` to write the selected target and the score from
`score.txt` into `build_target.hh`. A `static_assert` in that header checks
that the consuming compiler's architecture and platform match the selected
target. [Build definition](build.ycxx)

From this directory on x86-64 Linux or arm64 macOS with `ycxx` on `PATH`,
copy the workspace and build for the host and the other supported target.

```sh
example=$(mktemp -d "${TMPDIR:-/tmp}/ycxx-target-action.XXXXXX")
cp build.ycxx generate-header.sh main.cc score.txt "$example"
cd "$example"

case "$(uname -s):$(uname -m)" in
    Linux:x86_64) alternate=arm64-apple-darwin ;;
    Darwin:arm64) alternate=x86_64-pc-linux-gnu ;;
esac

ycxx build app --trace-engine=json 2> host.jsonl
ycxx build app --target="$alternate" --trace-engine=json 2> alternate.jsonl
file .ycxx/out/app ".ycxx/targets/$alternate/out/app"

ycxx show app --format=html > host-plan.html
ycxx show app --target="$alternate" --format=html > alternate-plan.html
```

Open both plans and find `host_header` and `target_header` in **Plan facts**.
The `argv` rows show the resolved triple and link its selector to the
manifest. The host triple is the same in both reports. The selected target
switches in `alternate-plan.html`.

Compare the generator trace events.

```sh
grep '"kind":"ExternalAction"' host.jsonl alternate.jsonl
```

The first build runs both generators. The second reuses the
`host_header` receipt and runs `target_header` with the other triple. The
default executable at `.ycxx/out/app` runs on the host and exits with status
`42`. The explicit executable runs on its selected platform.

## Watch the host target

Run watch in a second terminal in the copied workspace. The default build
passes the canonical host triple to both generators.

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
