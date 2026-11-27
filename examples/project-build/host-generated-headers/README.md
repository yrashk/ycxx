# Give a host tool a generated header

`scanner_config` writes a C++ header from `score.txt`. Its inputs are disk
files, so it can run before the compiler builds the native scanner.

```text
generate "scanner_config" {
    tool = "/bin/sh"
    args = [input("script"), input("score"), output("header")]
    inputs = { script = "generate-scanner-header.sh" score = "score.txt" }
    outputs = { header = "scanner_config.hh" }
}

cpp.host_executable "scanner" {
    sources = ["scanner.cc"]
    generated_headers = [artifact("scanner_config", "header")]
}
```

The scanner includes `scanner_config.hh` and returns its score. The
`config` action runs the staged `artifact("scanner", "executable")` for
the host and writes `generated_config.hh` for the app.
[Build definition](build.ycxx)

## Build for the other target first

Run from this directory on x86-64 Linux or arm64 macOS with `ycxx` on
`PATH`.

```sh
example=$(mktemp -d "${TMPDIR:-/tmp}/ycxx-host-generated-headers.XXXXXX")
cp build.ycxx *.sh *.cc score.txt "$example"
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

`file` reports Mach-O arm64 for the macOS app or ELF x86-64 for the Linux
app. Open `foreign-plan.html` and follow `app` through `config` and
`scanner` to `scanner_config` under **Target dependencies**. The scanner's
`generated_headers` edge links to the header binding in `build.ycxx`.

## Change the input without changing the header

The header generator accepts surrounding spaces. Add spaces to the input,
then rebuild the same foreign target.

```sh
printf '%s\n' ' 17 ' > score.txt
ycxx build app --target="$alternate" --trace-engine=json 2> same-header.jsonl
grep -F '"kind":"ExternalAction"' foreign.jsonl same-header.jsonl
```

`scanner_config` runs again and returns identical header bytes. The
scanner build, `config` receipt, and app can reuse their results.

## Watch the score change

Run watch for the default app.

```sh
ycxx build app --watch --trace-engine=interactive
```

After the initial build, run `printf '%s\n' 23 > score.txt` in another
terminal. The generated scanner header changes, so watch rebuilds the
scanner and the app. Verify the new score and refresh the HTML plan once
watch reports success.

```sh
status=0
.ycxx/out/app || status=$?
printf '%s\n' "$status"
test "$status" -eq 23
ycxx show app --format=html > updated-plan.html
```

Press `q` in the trace view to stop watch.
