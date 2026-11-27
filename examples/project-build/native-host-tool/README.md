# Build a native tool for the host

The build compiles `scanner.cc` for the host and stages the executable as
an input to `config`. A shell driver runs the staged tool and writes its
result into the C++ header used by `app`.

```text
cpp.host_executable "scanner" {
    sources = ["scanner.cc"]
}

generate "config" {
    tool = "/bin/sh"
    args = [input("driver"), input("scanner"), output("header")]
    inputs = { driver = "generate-score.sh" scanner = artifact("scanner", "executable") }
    outputs = { header = "generated_config.hh" }
}
```

Run these commands from this directory on x86-64 Linux or arm64 macOS
with `ycxx` on `PATH`.

```sh
example=$(mktemp -d "${TMPDIR:-/tmp}/ycxx-native-host-tool.XXXXXX")
cp build.ycxx generate-score.sh scanner.cc scanner_score.hh main.cc "$example"
cd "$example"

case "$(uname -s):$(uname -m)" in
    Linux:x86_64) alternate=arm64-apple-darwin ;;
    Darwin:arm64) alternate=x86_64-pc-linux-gnu ;;
esac

ycxx build app --trace-engine=json 2> host.jsonl
status=0
.ycxx/out/app || status=$?
printf '%s\n' "$status"
test "$status" -eq 17

ycxx build app --target="$alternate" --trace-engine=json 2> alternate.jsonl
file .ycxx/out/app ".ycxx/targets/$alternate/out/app"
ycxx show app --target="$alternate" --format=html > alternate-plan.html
```

`file` identifies the Linux executable as ELF and the macOS executable as
Mach-O. Both builds use the host scanner. The resident engine can reuse the
scanner build across both targets. The `config` action can reuse its receipt
because it receives the same executable and arguments. Inspect the compiler activity
for `scanner` and `app` in the trace. Compare generator executions with
`grep -F '"kind":"ExternalAction"' host.jsonl alternate.jsonl`.

Open `alternate-plan.html` and follow `app` to `config`, then `scanner`
under **Target dependencies**. The scanner remains on the canonical host
triple. The report comes from read-only inspection. [Build definition](build.ycxx)

## Watch a scanner header edit

Run watch for the default target in the copied workspace.

```sh
ycxx build app --watch --trace-engine=interactive
```

Change `scanner_score = 17` to `scanner_score = 23` in
`scanner_score.hh` in another terminal. The scanner rebuilds, the shell
driver writes the new value, and the app rebuilds. After watch reports
success, verify the app's new exit status.

```sh
status=0
.ycxx/out/app || status=$?
printf '%s\n' "$status"
test "$status" -eq 23
```

Press `q` in the trace view to stop watch.
