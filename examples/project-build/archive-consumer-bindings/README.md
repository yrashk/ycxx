# Give a shared library host and target archives

The native `scanner` and the selected-target `app` both link
`archive_bridge`. One public link input binds a named archive for each
consumer role.

```text
cpp.library "archive_bridge" {
    sources = ["archive_bridge.cc"]
    public_include_dirs = ["include"]
    public_link_inputs = [{
        host = artifact("host_archive", "archive")
        target = artifact("target_archive", "archive")
    }]
}
```

The object actions compile with `ycxx cxx` and pass `host_triple()` or
`target_triple()` explicitly. The packing script runs `llvm-ar` with the
matching GNU or Darwin archive format and an index for the one object.
The scanner calls the host archive's score function. The `config` action
writes that score into a C++ header, and the app adds the score from its
target archive. [Build definition](build.ycxx)

## Build the foreign app first

Run from this directory on x86-64 Linux or arm64 macOS with a current
`ycxx` and a cross-format `llvm-ar` supporting `--format=gnu` and
`--format=darwin`.

```sh
example=$(mktemp -d "${TMPDIR:-/tmp}/ycxx-archive-bindings.XXXXXX")
cp -R build.ycxx *.cc *.sh include "$example"
cd "$example"
command -v ycxx
command -v llvm-ar
```

Set the `/usr/local/bin/ycxx` and `/usr/local/bin/llvm-ar` spellings
in the copied `build.ycxx` to their absolute executable paths. A versioned
Linux LLVM install may supply `llvm-ar-23`. On macOS,
`xcrun --find llvm-ar` can locate the Xcode tool. For an unpacked Linux
LLVM installation, set the archive actions' `LD_LIBRARY_PATH` entries to
the directory containing its runtime libraries.

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
test "$status" -eq 57
```

The foreign build pairs the host archive with an archive for the other
platform. The default build selects both roles on the host architecture
and still returns the sum of two distinct scores, 17 and 40.

Open **Graph** in the offline `foreign-plan.html`. Select `archive_bridge`
and show its dependencies to see `host_archive` and `target_archive`.
The relationship details show the active role of each
`public_link_inputs` occurrence and link to its binding in `build.ycxx`.
Expand `config` to find the native `scanner`, which reaches the same
library node as the app.

## Watch the two archive edits

Run watch for the default app.

```sh
ycxx build app --watch --trace-engine=interactive
```

Make each edit separately in another terminal, and wait for a successful
build before moving to the next.

| Edit | App exit status |
| --- | --- |
| Change `return 17;` to `return 23;` in `host_score.cc` | 63 |
| Change `return 40;` to `return 45;` in `target_score.cc` | 68 |

The first edit relinks the scanner and changes the generated header. The
second edit relinks the app and can reuse the scanner and its header.
Check the status after each build and refresh the HTML plan.

```sh
status=0
.ycxx/out/app || status=$?
printf '%s\n' "$status"
ycxx show app --format=html > updated-plan.html
```

Press `q` in the trace view to stop watch.
