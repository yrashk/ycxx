# Package-defined probe

The workspace imports `rules/package` as `codec`. The package's `profile` rule
stages `wire.cfg` and `rules/check-mode.sh` for a `/bin/sh` check and exports
`fast-mode`. The check reports the raw profile on stdout. The classifier interprets
the exit status and evidence as a typed support state. An exit status of 7 and
the expected stderr message mean the tool rejects fast mode.

`value("wire", "fast-mode")` passes the state to `builtin:capabilities`.
The application uses the generated `codec_config.hh` with `if constexpr`.
Run these commands with a current `ycxx` on `PATH` to try the edits in a copy.

```sh
example=$(mktemp -d "${TMPDIR:-/tmp}/ycxx-package-probe.XXXXXX")
cp -R . "$example"
cd "$example"

ycxx show app
ycxx build app --trace-engine=json 2> first.jsonl
ycxx build app --trace-engine=json 2> unchanged.jsonl
./.ycxx/out/app

printf '  enabled \n' > wire.cfg
ycxx show app
ycxx build app --trace-engine=json 2> evidence.jsonl
./.ycxx/out/app

printf 'disabled\n' > wire.cfg
ycxx build app --trace-engine=json 2> changed.jsonl
./.ycxx/out/app || printf 'app exit status %s\n' "$?"
ycxx show app --format=html > plan.html
```

The first trace shows the `RuleWitness` and `RuleClassifier` actions and the
`RuleValue` that the header consumes. The unchanged request launches zero
actions. The whitespace edit changes the witness evidence while `fast-mode`
remains `supported`. The engine reuses the header and executable, and the
application still exits with `0`. The `disabled` edit produces `unsupported`,
regenerates the header and rebuilds the application, which now exits with `10`.
An unrecognized profile produces `indeterminate` and exit status `20`.

`ycxx show app` reports the observation as unresolved before a build and after
an input edit has made it stale. After the build, the plan lists the export and
its provenance. Run `ycxx build app --watch` to see a missing `wire.cfg` fail a
build and a restored file trigger recovery.
