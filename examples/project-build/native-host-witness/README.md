# Classify a native host executable as a build witness

The project builds `scanner.cc` for the host. The rule package runs the
staged executable as a witness and classifies its exit status as a typed
score. The app consumes that score through a generated C++ header.

The workspace declares `cpp.host_executable "scanner"` and the package
emits the witness action.

```text
action.observe "observed" {
    tool = input("scanner")
    inputs = { scanner = artifact("scanner", "executable") }
    export = "score"
    schema = "example.score.v1"
}
```

The scanner returns the constant from `scanner_score.hh`. The classifier
accepts normal exits 17 and 23 and exports a row named `score` with one
unsigned `value` field. The `config` rule binds it with
`facts = { score = value("observed", "score") }` and writes
`inline constexpr int generated_score = 17;` for the starting score.
[Workspace definition](build.ycxx) · [Rule package](rules/native_witness.cc)

Run these commands from this directory on x86-64 Linux or arm64 macOS
with `ycxx` on `PATH`.

```sh
example=$(mktemp -d "${TMPDIR:-/tmp}/ycxx-native-host-witness.XXXXXX")
cp -R build.ycxx main.cc scanner.cc scanner_score.hh rules "$example"
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

`file` should show ELF for the Linux executable and Mach-O for the macOS
executable. The scanner runs on the host in both configurations. Read the
`RuleWitness` and `RuleClassifier` events in the trace to see observation
and classification.

Open `alternate-plan.html` to inspect the foreign app's plan. In
**Target dependencies**, follow `app` to `config`, `observed`, and
`scanner`. **Value bindings** shows the exported score consumed by
`config`, and the observer's tool fact gives the staged scanner path.

## Watch a scanner header edit

Run watch for the default target.

```sh
ycxx build app --watch --trace-engine=interactive
```

Change `scanner_score = 17` to `scanner_score = 23` in
`scanner_score.hh` from another terminal. Watch should rebuild the scanner,
run the witness and classifier, and rebuild the generated header and app.
After it reports success, verify the new score and refresh the plan.

```sh
status=0
.ycxx/out/app || status=$?
printf '%s\n' "$status"
test "$status" -eq 23
ycxx show app --format=html > updated-plan.html
```

Press `q` in the trace view to stop watch.
