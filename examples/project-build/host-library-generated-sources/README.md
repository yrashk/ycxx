# Mix disk and generated sources in a host library

The `wire_source` action writes `wire_score.cc` and its private sibling
`wire_score.hh`. The generated source includes the sibling with a quoted
`#include`. The disk source `schema.cc` calls the generated function and
checks that the private sibling is absent from its own include search.

```text
cpp.library "schema" {
    sources = ["schema.cc", artifact("wire_source", "source")]
    public_include_dirs = ["include"]
}

cpp.host_executable "scanner" {
    sources = ["scanner.cc"]
    deps = ["schema"]
}
```

The host scanner and the selected-target app both link `schema`. The
`config` action runs the host scanner and writes a C++ constant. The app
checks that constant against the score from its own linked library.
[Build definition](build.ycxx) · [Source generator](generate-wire.sh)

## Build the foreign app first

Run from this directory on x86-64 Linux or arm64 macOS with `ycxx` on
`PATH`.

```sh
example=$(mktemp -d "${TMPDIR:-/tmp}/ycxx-host-library-sources.XXXXXX")
cp -R build.ycxx *.sh *.cc score.txt include "$example"
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
test "$status" -eq 17
```

`file` shows Mach-O arm64 on a Linux host or ELF x86-64 on a macOS host.
In `foreign-plan.html`, open **Target dependencies** and follow `app`
through `config` and `scanner` to `schema`. The app's own `deps` edge
opens the same library card. The `sources` link leads from `schema` to
`wire_source`, and its location link opens the authored source binding.

## Reuse equal generated sources

Add surrounding spaces to the input score, then rebuild the same
foreign target.

```sh
printf '%s\n' ' 17 ' > score.txt
ycxx build app --target="$alternate" --trace-engine=json 2> same-source.jsonl
grep -F '"kind":"ExternalAction"' foreign.jsonl same-source.jsonl
```

`wire_source` runs again and produces the same C++ source and sibling
header byte for byte. The library and scanner builds can reuse their
results, and the app can reuse the scanner-derived configuration.

## Watch the generated score change

Run watch for the default app.

```sh
ycxx build app --watch --trace-engine=interactive
```

After the initial build, run `printf '%s\n' 23 > score.txt` in another
terminal. The generated source and sibling header change. Watch rebuilds
the scanner and app with the new schema. Verify the new score and refresh
the HTML plan after watch reports success.

```sh
status=0
.ycxx/out/app || status=$?
printf '%s\n' "$status"
test "$status" -eq 23
ycxx show app --format=html > updated-plan.html
```

Press `q` in the trace view to stop watch.
