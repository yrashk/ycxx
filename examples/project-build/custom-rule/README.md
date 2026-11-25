# Generate a retry limit with a custom rule

The workspace imports `rules/package` as `policy`. Its [package definition](rules/build.ycxx)
builds a C++ shared library with `cxx()`, which tries ycxx first and can fall back to a host C++
compiler on `PATH`. `-Iinputs/sdk` makes the staged rule SDK available to the package.

The [`policy.header` rule](rules/retry_policy.cc) reads `retry-limit.txt` through the SDK. The
workspace declares that file in `reads` so the engine can track changes. The rule takes a digit
from 1 to 9 with optional surrounding whitespace and emits an `action.generate` declaration.
The action passes the digit and declared output path to a fixed `/bin/sh` command that writes
`retry_limit.hh`. The app requests the `header` artifact with `generated_headers` and returns
its `retry_limit` constant as the process exit status.

## Run and inspect

With a current `ycxx` on `PATH`, run from this directory. Copying the example lets you try the
edits in a separate workspace.

```sh
example=$(mktemp -d "${TMPDIR:-/tmp}/ycxx-custom-rule.XXXXXX")
cp -R . "$example"
cd "$example"

ycxx build app --trace-engine=json 2> first.jsonl
./.ycxx/out/app || printf 'app exit status %s\n' "$?"
ycxx show app
```

The app exits with `3`. The first trace records `RulePackage` for `rules/package`, `RuleWorker`
and `ExternalAction` for `retry_policy`, and native compilation and linking. `ycxx show app`
shows the resolved generator, its `header` output and the `artifact("retry_policy", "header")`
include edge to the app.

## Edit the tracked input

```sh
ycxx build app --trace-engine=json 2> unchanged.jsonl

printf '  3 \n' > retry-limit.txt
ycxx build app --trace-engine=json 2> whitespace.jsonl
./.ycxx/out/app || printf 'app exit status %s\n' "$?"

printf '5\n' > retry-limit.txt
ycxx build app --trace-engine=json 2> changed.jsonl
./.ycxx/out/app || printf 'app exit status %s\n' "$?"
```

- `unchanged.jsonl` ends with `actions_executed` equal to `0`.
- `whitespace.jsonl` records one action, the `retry_policy` worker. The digit is still `3`, so
  the engine reuses the generator result, compilation and link. The app still exits with `3`.
- `changed.jsonl` records the worker and `ExternalAction` for `retry_policy`. The new header
  value recompiles `main.cc` and relinks `app`. The app exits with `5`.

Run `ycxx build app --watch --trace-engine=interactive` to follow the same edits from another
terminal. Press Ctrl-C to stop the watch.
