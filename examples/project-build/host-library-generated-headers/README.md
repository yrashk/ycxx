# Generate a header for a shared host library

The `wire_schema` action reads `score.txt` and writes `wire_schema.hh`.
The `schema` library includes that header in `schema.cc` and exposes the
score through its public API.

```text
cpp.library "schema" {
    sources = ["schema.cc"]
    public_include_dirs = ["include"]
    generated_headers = [artifact("wire_schema", "header")]
}

cpp.host_executable "scanner" {
    sources = ["scanner.cc"]
    deps = ["schema"]
}
```

The scanner links `schema` for the host. The `config` action runs the
scanner and writes the score into `generated_config.hh`. The app also
links `schema` for its selected target and checks that its library score
matches the host-derived constant. [Build definition](build.ycxx)

## Build the foreign app first

Run from this directory on x86-64 Linux or arm64 macOS with `ycxx` on
`PATH`.

```sh
example=$(mktemp -d "${TMPDIR:-/tmp}/ycxx-host-library-headers.XXXXXX")
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

The foreign app is Mach-O arm64 on a Linux host or ELF x86-64 on a macOS
host. Open `foreign-plan.html` and find **Target dependencies**. Follow
`app` through `config` and `scanner` to `schema`. The app's own `deps`
edge opens the same `schema` card. Follow its `generated_headers` edge to
`wire_schema`, or open the source link to the binding in `build.ycxx`.

## Reuse equal generated headers

Add surrounding spaces to the score and rebuild the same foreign target.

```sh
printf '%s\n' ' 17 ' > score.txt
ycxx build app --target="$alternate" --trace-engine=json 2> same-header.jsonl
grep -F '"kind":"ExternalAction"' foreign.jsonl same-header.jsonl
```

`wire_schema` runs again and emits the same header byte for byte. The
host and foreign library compilations can reuse their results, and the
app can reuse the scanner's generated score.

## Watch a changed header

Run watch for the default app.

```sh
ycxx build app --watch --trace-engine=interactive
```

After the initial build, run `printf '%s\n' 23 > score.txt` in another
terminal. The generated header changes the schema compiled into the
scanner and the app. After watch reports success, verify the new score
and refresh the HTML plan.

```sh
status=0
.ycxx/out/app || status=$?
printf '%s\n' "$status"
test "$status" -eq 23
ycxx show app --format=html > updated-plan.html
```

Press `q` in the trace view to stop watch.
