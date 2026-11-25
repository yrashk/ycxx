# Project build examples

Each directory is a workspace with its own `build.ycxx`. Run the commands from
that directory with a current `ycxx` on `PATH`. You can also copy a workspace
out of the checkout and build it there.

| Project walkthrough | Build definition | What to try |
| --- | --- | --- |
| [Freestanding targets](freestanding-targets/README.md) | [build.ycxx](freestanding-targets/build.ycxx) | Build one app for Linux x86-64 and macOS arm64 and inspect each target's output. |
| [Native C++](native/README.md) | [build.ycxx](native/build.ycxx) | Watch a generator input change and inspect the library dependency and source glob. |
| [Custom rule](custom-rule/README.md) | [build.ycxx](custom-rule/build.ycxx) | Use a C++ workspace plugin to generate a retry-limit header and compare a whitespace edit with a digit edit. |
| [Typed capability header](typed-capabilities/README.md) | [build.ycxx](typed-capabilities/build.ycxx) | Inspect the compiler's `<filesystem>` probe and run the app that uses its generated constant. |
| [Package-defined probe](package-probe/README.md) | [build.ycxx](package-probe/build.ycxx) | Edit a codec profile and trace how a plugin classifies shell output as a typed support state. |
| [Shader bundle](shader-bundle/README.md) | [build.ycxx](shader-bundle/build.ycxx) | Generate an inventory, add and remove a shader, and update managed runtime assets under watch. |
| [Shader reflection](shader-reflection/README.md) | [build.ycxx](shader-reflection/build.ycxx) | Compare a shader body edit with a binding edit to see when the generated descriptor table rebuilds native code. |
| [Generated archive](generated-archive/README.md) | [build.ycxx](generated-archive/build.ycxx) | Follow a generated archive through a library dependency and watch an archive edit relink the app. |
| [Foreign Make library](foreign-make/README.md) | [build.ycxx](foreign-make/build.ycxx) | Link an archive built by Make, then watch a vendor header or declared helper edit. |

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
