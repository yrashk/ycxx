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

## Pass the triples in the action environment

Use a fresh copy of the workspace and set `alternate` as above. Replace the
`target_header` declaration in the copy's `build.ycxx`.

```text
generate "target_header" {
    tool = "/bin/sh"
    args = [input("script"), input("score"), output("header")]
    environment = {
        BUILD_HOST = host_triple()
        BUILD_TARGET = target_triple()
    }
    inputs = { script = "generate-header.sh", score = "score.txt" }
    outputs = { header = "build_target.hh" }
}
```

In `generate-header.sh`, set `target=$BUILD_TARGET` in place of `target=$1`.
Read the score from `"$1"` and write the header to `"$2"` because the
target triple now comes from the environment. Add this line to the header
output block to record the tool's host.

```sh
printf 'inline constexpr char generator_host[] = "%s";\n' "$BUILD_HOST"
```

Build and inspect both variants.

```sh
ycxx build app --trace-engine=json 2> env-host.jsonl
ycxx build app --target="$alternate" --trace-engine=json 2> env-alternate.jsonl
ycxx show app --format=html > env-host-plan.html
ycxx show app --target="$alternate" --format=html > env-alternate-plan.html
grep -F '"kind":"ExternalAction"' env-host.jsonl env-alternate.jsonl
```

Find `target_header` in **Plan facts**. The two `environment` rows show
the resolved strings and their selectors. `BUILD_HOST` stays the same
across both plans and `BUILD_TARGET` follows the requested variant.
The alternate build's trace shows `target_header` running again. The
build reuses the host-only header's receipt.

To see how receipt identity works on the default build, replace just the
two selectors in `target_header.environment` with the quoted values shown
in `env-host-plan.html`. Rebuild in the same workspace and inspect the
generator count in the trace.

```sh
ycxx build app --trace-engine=json 2> env-literals.jsonl
grep -Fc '"kind":"ExternalAction"' env-literals.jsonl || :
```

The generator count is zero because the resolved environment and the
other declared inputs match. Restore the selectors, then run
`ycxx build app --watch --trace-engine=interactive` and make the `score.txt`
edit from the watch walkthrough above. The app returns `43` after the
target header regenerates.
