# Run a generated executable as a build tool

Put a current `ycxx` on `PATH`. Run these commands from this directory.
Copying the workspace gives you a place for the watch edit.

```sh
example=$(mktemp -d "${TMPDIR:-/tmp}/ycxx-staged-tool.XXXXXX")
cp -R . "$example"
cd "$example"

ycxx build app
status=0
.ycxx/out/app || status=$?
printf '%s\n' "$status"
test "$status" -eq 7

ycxx show app --format=html > plan.html
```

The workspace builds the [rule package](rules/build.ycxx) from C++ with
`cxx()`. The `runner` rule stages the executable [runner.sh](runner.sh)
and copies it with `/bin/cp -p`, preserving its executable mode. The
`config` action binds that named output and launches it as its tool.

```text
action.generate "config" {
    tool = input("runner")
    args = [output("header")]
    inputs = { runner = artifact("runner", "runner") }
    outputs = { header = "generated_config.hh" }
}
```

The script writes a `constexpr` value into the generated header. The app
returns that value. In **Target dependencies**, follow `app` to `config`
and then `runner`. The `tool` fact on `config` shows the planned staged
executable path and its `input("runner")` selector.

## Watch a tool edit

Run watch in one terminal.

```sh
ycxx build app --watch --trace-engine=interactive
```

Change `generated_score = 7` to `generated_score = 8` in `runner.sh` from
another terminal. Watch reruns the producer and the header action and
rebuilds the app. After it reports success, verify the new value.

```sh
status=0
.ycxx/out/app || status=$?
printf '%s\n' "$status"
test "$status" -eq 8
```

Press `q` in the trace view to stop watch.
