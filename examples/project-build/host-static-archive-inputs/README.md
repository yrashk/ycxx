# Link a generated static archive into a host tool

`scanner_object` uses `ycxx cxx -c` to compile `archive_score.cc` for the
host. `scanner_archive` stages that object and runs `ar rcs` to create
`libscanner.a`. The host executable links the named archive output.

```text
cpp.host_executable "scanner" {
    sources = ["scanner.cc"]
    link_inputs = [artifact("scanner_archive", "archive")]
}
```

The `config` generator runs the staged scanner and writes its score into a
C++ header. The app builds for the requested target and returns that score.
[Build definition](build.ycxx)

## Build the foreign app first

Run from this directory on x86-64 Linux or arm64 macOS with `ycxx` and
`ar` on `PATH`. Copy the workspace for the walkthrough.

```sh
example=$(mktemp -d "${TMPDIR:-/tmp}/ycxx-host-static-archive.XXXXXX")
cp build.ycxx *.cc generate-score.sh "$example"
cd "$example"
command -v ycxx
command -v ar
```

Set `scanner_object.tool` and `scanner_archive.tool` in the copied
`build.ycxx` to the absolute paths printed above. The object action uses
the same ycxx executable and runs its standalone compile in-process.

```sh
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
test "$status" -eq 17
```

`file` shows Mach-O arm64 on a Linux host or ELF x86-64 on a macOS host.
In the saved plan's **Graph**, start at `app` and expand `config` to
see `scanner`. Expand `scanner` to reach `scanner_archive`, then expand
the archive to see `scanner_object`. Select `scanner` and follow the
location link on its `link_inputs` edge to open the archive binding in
`build.ycxx`.

## Watch an archive edit

Run watch for the default app.

```sh
ycxx build app --watch --trace-engine=interactive
```

Change `return 17;` to `return 23;` in `archive_score.cc` from another
terminal. The trace shows a new object and archive, the host scanner's
link, and the generated config for the app. After watch reports success,
verify the new score.

```sh
status=0
.ycxx/out/app || status=$?
printf '%s\n' "$status"
test "$status" -eq 23
ycxx show app --format=html > updated-plan.html
```

Press `q` in the trace view to stop watch.
