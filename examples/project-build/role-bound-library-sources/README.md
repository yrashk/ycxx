# Choose a generated library source by consumer role

The native `scanner` and the selected-target `app` both link `bridge`.
At one position in the library's source list, each role chooses its own
generated implementation.

```text
cpp.library "bridge" {
    sources = [
        "bridge.cc",
        {
            host = artifact("host_impl", "source")
            target = artifact("app_impl", "source")
        }
    ]
    public_include_dirs = ["include"]
}
```

Both generators emit `impl.cc` and a same-directory `detail.hh`. The
generated source uses a quoted include and checks its sibling's score at
compile time. The disk source `bridge.cc` checks that the sibling stays
private to its generated implementation.

The `config` action runs the scanner and writes its score into the app's
generated header. The app adds that value to the score from the library
compiled for its selected target. [Build definition](build.ycxx) ·
[Source generator](generate-impl.sh)

## Build the foreign app first

Run from this directory on x86-64 Linux or arm64 macOS with `ycxx` on
`PATH`.

```sh
example=$(mktemp -d "${TMPDIR:-/tmp}/ycxx-library-source-roles.XXXXXX")
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
Both source roles apply when the default app and scanner use the same
triple.

Open **Graph** in the offline `foreign-plan.html`. Follow `app` through
`config` and `scanner` to the shared `bridge` node. Show the dependencies
of `bridge` to reveal `host_impl` and `app_impl`. Follow the location
links on the two `sources` edges to the individual role bindings in
`build.ycxx`. The library details show
`.ycxx/host/virtual/host_impl/impl.cc` for the host and
`.ycxx/targets/<triple>/virtual/app_impl/impl.cc` for the selected target.

## Watch an edit to either implementation

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

The host edit rebuilds the scanner and changes the header it generates
for the app. The target edit rebuilds the app's library while the scanner
result can be reused. Check the exit status after each build.

```sh
status=0
.ycxx/out/app || status=$?
printf '%s\n' "$status"
ycxx show app --format=html > updated-plan.html
```

Press `q` in the trace view to stop watch.
