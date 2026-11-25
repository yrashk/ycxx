# Project build examples

Each directory is a workspace with its own `build.ycxx`. Run the commands from
that directory with a current `ycxx` on `PATH`. You can also copy a workspace
out of the checkout and build it there.

| Project | What to try |
| --- | --- |
| [Native C++](native) | Watch a generator input change and inspect the library dependency and source glob. |
| [Custom rule](custom-rule) | Compare a whitespace edit and a digit edit in a retry limit read by a workspace plugin. |
| [Typed capability header](typed-capabilities) | Inspect the compiler's `<filesystem>` probe and run the app that uses its generated constant. |
| [Package-defined probe](package-probe) | Edit a codec profile and trace how a plugin classifies shell output as a typed support state. |
| [Shader bundle](shader-bundle) | Build shaders from an inventory and trace private compile actions and runtime asset publication. |
| [Shader reflection](shader-reflection) | Build `runtime_only`, then `app`. Compare a shader body edit and a binding edit to see when a typed descriptor table rebuilds native code. |

Each project builds a target named `app`. The resident daemon can reuse
work from an earlier request in the same workspace.

```sh
ycxx build app
ycxx build app --trace-engine=interactive
ycxx show app --format=html > plan.html
```

The trace shows requests and reuse. Open `plan.html` to read the resolved plan
alongside its build definition. Use `ycxx build app --watch` to rerun on edits.

Read the [build design](../../docs/design/build.md) for the rule and artifact model.
