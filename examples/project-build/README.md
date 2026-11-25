# Project build examples

Each directory is a workspace with its own `build.ycxx`. Run the commands from
that directory with a current `ycxx` on `PATH`. You can also copy a workspace
out of the checkout and build it there.

| Project | Features |
| --- | --- |
| [Native C++](native) | A library, source glob with an exclusion, and a declared generator |
| [Custom rule](custom-rule) | A source-built rule package, a tracked read and a generated input |
| [Typed capability header](typed-capabilities) | Compiler probes, a named header artifact and `if constexpr` |

All three projects build a target named `app`. The resident daemon can reuse
work from an earlier request in the same workspace.

```sh
ycxx build app
ycxx build app --trace-engine=interactive
ycxx show app --format=html > plan.html
```

The trace shows requests and reuse. Open `plan.html` to read the resolved plan
alongside its build definition. Use `ycxx build app --watch` to rerun on edits.

Read the [build design](../../docs/design/build.md) for the rule and artifact model.
