# Inherit a generated archive through a host library

The native `scanner` depends on `left` and `right`. Both libraries call
`archive_bridge`, which exposes the generated archive as a public link input.

```text
cpp.library "archive_bridge" {
    sources = ["archive_bridge.cc"]
    public_include_dirs = ["include"]
    public_link_inputs = [artifact("scanner_archive", "archive")]
}

cpp.host_executable "scanner" {
    sources = ["scanner.cc"]
    deps = ["left", "right"]
}
```

Both paths reach the same library and its single authored archive binding.
`scanner_object` uses `ycxx cxx -c` with its default host triple.
`scanner_archive` packs that object with `ar rcs` as one named output.
The scanner links it on the host. The `config` action runs the scanner and
generates the C++ header consumed by the selected app.
[Build definition](build.ycxx)

## Build the foreign app first

Run from this directory on x86-64 Linux or arm64 macOS with `ycxx` and
`ar` on `PATH`.

```sh
example=$(mktemp -d "${TMPDIR:-/tmp}/ycxx-host-library-archive.XXXXXX")
cp -R build.ycxx *.cc generate-score.sh include "$example"
cd "$example"
command -v ycxx
command -v ar
```

Set `scanner_object.tool` and `scanner_archive.tool` in the copied
`build.ycxx` to the absolute executable paths printed above.

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
Open **Graph** in the saved `foreign-plan.html`. Expand `config` to find
`scanner`, then expand the scanner and its two library branches. Both lead
to the same `archive_bridge` node. Expand that node for `scanner_archive`.
The `public_link_inputs` edge's location link opens the authored binding
in `build.ycxx`. The graph works offline.

## Watch an archive edit

Run watch for the default app.

```sh
ycxx build app --watch --trace-engine=interactive
```

Change `return 17;` to `return 23;` in `archive_score.cc` from another
terminal. Watch regenerates the object and archive, relinks the scanner,
and refreshes the configuration for the app. The scanner and library
translation units can reuse their previous results. After watch reports
success, verify the new app score.

```sh
status=0
.ycxx/out/app || status=$?
printf '%s\n' "$status"
test "$status" -eq 23
ycxx show app --format=html > updated-plan.html
```

Press `q` in the trace view to stop watch.
