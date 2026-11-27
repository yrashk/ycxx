# Choose a generated library header by consumer role

The native `scanner` and the selected-target `app` both link `bridge`.
Its source includes `config.hh`, and one header binding selects a different
producer for each role.

```text
cpp.library "bridge" {
    sources = ["bridge.cc"]
    public_include_dirs = ["include"]
    generated_headers = [{
        host = artifact("host_config", "header")
        target = artifact("target_config", "header")
    }]
}
```

The two producers generate the same include name with different
`constexpr` values. The scanner returns the host value. The `config`
action writes that result into the app's generated header. The app adds
the scanner's value and the value from its own library.
[Build definition](build.ycxx)

## Build the foreign app first

Run from this directory on x86-64 Linux or arm64 macOS with `ycxx` on
`PATH`.

```sh
example=$(mktemp -d "${TMPDIR:-/tmp}/ycxx-library-header-roles.XXXXXX")
cp -R build.ycxx *.cc *.sh *_score.txt include "$example"
cd "$example"

case "$(uname -s):$(uname -m)" in
    Linux:x86_64) alternate=arm64-apple-darwin ;;
    Darwin:arm64) alternate=x86_64-pc-linux-gnu ;;
esac

ycxx build app --target="$alternate" --trace-engine=json 2> foreign.jsonl
file ".ycxx/targets/$alternate/out/app"
ycxx show app --target="$alternate" --format=html > foreign-plan.html

ycxx build app
status=0
.ycxx/out/app || status=$?
printf '%s\n' "$status"
test "$status" -eq 57
```

`file` shows Mach-O arm64 on a Linux host or ELF x86-64 on a macOS host.
The default app returns 57, from the host value 17 and target value 40.
Both bindings apply even when the default app and scanner compile for
the same triple.

Open **Graph** in the offline `foreign-plan.html`. Follow `app` through
`config` and `scanner` to the same `bridge` node the app depends on
directly. Show the dependencies of `bridge` to reveal `host_config` and
`target_config`. The two `generated_headers` edges identify the active
roles and link to their authored locations. The library details show the
host include directory `.ycxx/host/virtual/host_config` and the selected
include directory `.ycxx/targets/<triple>/virtual/target_config`.

## Watch an edit to either header

Run watch for the default app.

```sh
ycxx build app --watch --trace-engine=interactive
```

In another terminal, run each edit separately and wait for a successful
build before running the next.

| Edit | App exit status |
| --- | --- |
| `printf '%s\n' 23 > host_score.txt` | 63 |
| `printf '%s\n' 45 > target_score.txt` | 68 |

The first edit changes the scanner's library and the header generated
from the scanner. The second changes the app's library and can reuse the
scanner's results. Check the exit status after each build.

```sh
status=0
.ycxx/out/app || status=$?
printf '%s\n' "$status"
ycxx show app --format=html > updated-plan.html
```

Press `q` in the trace view to stop watch.
