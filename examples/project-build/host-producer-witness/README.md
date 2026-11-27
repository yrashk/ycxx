# Run a witness through a generated tool chain

The C++ scanner builds for the host and returns the score in
`scanner_score.hh`. The `analyzer_source` action runs the scanner and writes
an executable script that returns the same score. The `analyzer` action
copies the script with `/bin/cp -p` to preserve its executable mode.
[Build definition](build.ycxx)

```text
generate "analyzer_source" {
    tool = "/bin/sh"
    args = [input("script"), input("scanner"), output("executable"), "/bin/chmod"]
    inputs = { script = "generate-analyzer.sh" scanner = artifact("scanner", "executable") }
    tool_inputs = { chmod = "/bin/chmod" }
    outputs = { executable = "analyzer.sh" }
}

generate "analyzer" {
    tool = "/bin/cp"
    args = ["-p", input("program"), output("executable")]
    inputs = { program = artifact("analyzer_source", "executable") }
    outputs = { executable = "analyzer.sh" }
}
```

The rule package emits the witness and runs the staged copy.

```text
action.observe "observed" {
    tool = input("analyzer")
    inputs = { analyzer = artifact("analyzer", "executable") }
    export = "score"
    schema = "example.score.v1"
}
```

The package classifies exit status 17 or 23 into a typed score. The
`config` rule consumes it with
`facts = { score = value("observed", "score") }` and generates a
`constexpr` header for the app. [Rule package](rules/witness.cc)

## Build for the other target

From this directory on x86-64 Linux or arm64 macOS, with `ycxx` on `PATH`,
copy the workspace and build the foreign app first.

```sh
example=$(mktemp -d "${TMPDIR:-/tmp}/ycxx-producer-witness.XXXXXX")
cp -R build.ycxx generate-analyzer.sh scanner.cc scanner_score.hh main.cc rules "$example"
cd "$example"

case "$(uname -s):$(uname -m)" in
    Linux:x86_64) alternate=arm64-apple-darwin ;;
    Darwin:arm64) alternate=x86_64-pc-linux-gnu ;;
esac

ycxx build app --target="$alternate" --trace-engine=json 2> cross.jsonl
ycxx build app --target="$alternate" --trace-engine=json 2> cross-warm.jsonl
ycxx show app --target="$alternate" --format=html > cross-plan.html

ycxx build app
file .ycxx/out/app ".ycxx/targets/$alternate/out/app"
status=0
.ycxx/out/app || status=$?
printf '%s\n' "$status"
test "$status" -eq 17
```

The scanner and the shell actions execute on the host during the foreign
build. `file` identifies ELF for the Linux app and Mach-O for the macOS app.
Compare the generator and witness events in the two traces. The unchanged
foreign build reuses the receipts.

```sh
grep -E '"kind":"(ExternalAction|RuleWitness)"' cross.jsonl cross-warm.jsonl
```

Open `cross-plan.html` and follow `app` through `config`, `observed`,
`analyzer`, and `analyzer_source` to `scanner` in **Target dependencies**.
The plan shows the staged analyzer tool and the `score` value binding.

## Watch the score change

Run watch for the default app in the copied workspace.

```sh
ycxx build app --watch --trace-engine=interactive
```

From another terminal, change `scanner_score = 17` to `scanner_score = 23`
in `scanner_score.hh`. The updated scanner changes the generated script,
the witness score, and the app's header. After watch reports success,
check the new app status and refresh the HTML plan.

```sh
status=0
.ycxx/out/app || status=$?
printf '%s\n' "$status"
test "$status" -eq 23
ycxx show app --format=html > updated-plan.html
```

Press `q` in the trace view to stop watch.
