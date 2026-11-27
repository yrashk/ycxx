# Plan a rule from a host-generated inventory

The `scanner` is a `cpp.host_executable` that reads its score from
`scanner_score.hh`. The `scan_inventory` action runs it and writes a
line such as `score=17`. An ordinary copy action publishes those bytes
as `inventory`'s named `list` output. [Build definition](build.ycxx)

```text
generate "inventory" {
    tool = "/bin/cp"
    args = [input("list"), output("list")]
    inputs = { list = artifact("scan_inventory", "list") }
    outputs = { list = "inventory.txt" }
}

tools.header "config" {
    plan_inputs = { inventory = artifact("inventory", "list") }
}
```

The source-built rule package reads `inventory` by name through the
worker API. It uses the inventory score to emit a generator for
`generated_config.hh`. The app includes the header and returns its
`constexpr` score. [Rule implementation](rules/inventory.cc)

## Build a foreign app first

Run from this directory on x86-64 Linux or arm64 macOS with `ycxx` on
`PATH`. Copy the workspace to try the edits.

```sh
example=$(mktemp -d "${TMPDIR:-/tmp}/ycxx-host-planning-inputs.XXXXXX")
cp -R build.ycxx scan-inventory.sh scanner.cc scanner_score.hh main.cc rules "$example"
cd "$example"

case "$(uname -s):$(uname -m)" in
    Linux:x86_64) alternate=arm64-apple-darwin ;;
    Darwin:arm64) alternate=x86_64-pc-linux-gnu ;;
esac

ycxx build app --target="$alternate" --trace-engine=json 2> foreign.jsonl
ycxx show app --target="$alternate" --format=html > foreign-plan.html

ycxx build app
file .ycxx/out/app ".ycxx/targets/$alternate/out/app"
status=0
.ycxx/out/app || status=$?
printf '%s\n' "$status"
test "$status" -eq 17
```

The scanner runs on the host during the foreign build. `file` shows the
Linux app as ELF and the macOS app as Mach-O. In `foreign-plan.html`,
follow `app` through `config`, `inventory`, and `scan_inventory` to
`scanner` in **Target dependencies**. The `config` card links the named
planning input to its producer and its location in `build.ycxx`.

## Change the scanner while keeping the inventory

Change `scanner_marker = 0` to `scanner_marker = 1` in `scanner_score.hh`.
An invocation with an extra argument returns the marker. The inventory
action calls the scanner without arguments, so its output remains
`score=17`. Rebuild the same foreign target and compare the traces.

```sh
ycxx build app --target="$alternate" --trace-engine=json 2> equal-inventory.jsonl
grep -E '"kind":"(ExternalAction|RuleWorker)"' foreign.jsonl equal-inventory.jsonl
```

The scanner image changes and `scan_inventory` runs again. The copy
action, `config` worker, and app reuse their results because the
inventory bytes stay equal.

## Watch the score change

Run watch for the default app in the copied workspace.

```sh
ycxx build app --watch --trace-engine=interactive
```

In another terminal, change `scanner_score = 17` to `scanner_score = 23`
in `scanner_score.hh`. That changes the inventory and the generated header.
After watch reports success, verify the app and refresh the HTML plan.

```sh
status=0
.ycxx/out/app || status=$?
printf '%s\n' "$status"
test "$status" -eq 23
ycxx show app --format=html > updated-plan.html
```

Press `q` in the trace view to stop watch.
