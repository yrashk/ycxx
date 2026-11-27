# Project build examples

Each directory is a workspace with its own `build.ycxx`. Run the commands from
that directory with a current `ycxx` on `PATH`. You can also copy a workspace
out of the checkout and build it there.

| Project walkthrough | Build definition | What to try |
| --- | --- | --- |
| [Freestanding targets](freestanding-targets/README.md) | [build.ycxx](freestanding-targets/build.ycxx) | Build one app for Linux x86-64 and macOS arm64 and inspect each target's output. |
| [Target-aware action](target-aware-action/README.md) | [build.ycxx](target-aware-action/build.ycxx) | Pass host and target triples through arguments or environment variables and inspect receipt reuse. |
| [Staged tool](staged-tool/README.md) | [build.ycxx](staged-tool/build.ycxx) | Run a generated executable to produce a C++ header, then edit the executable under watch. |
| [Native host tool](native-host-tool/README.md) | [build.ycxx](native-host-tool/build.ycxx) | Compile a host scanner to generate a C++ header and reuse it across native targets. |
| [Host C++ libraries](host-cxx-libraries/README.md) | [build.ycxx](host-cxx-libraries/build.ycxx) | Build a host scanner and cross-target app with shared library dependencies, then watch a transitive header edit. |
| [Host generated headers](host-generated-headers/README.md) | [build.ycxx](host-generated-headers/build.ycxx) | Give a host scanner a generated header and compare edits that preserve or change its bytes. |
| [Host generated sources](host-generated-sources/README.md) | [build.ycxx](host-generated-sources/build.ycxx) | Generate a host scanner's C++ source and sibling header, then trace equal and changed outputs. |
| [Host library generated headers](host-library-generated-headers/README.md) | [build.ycxx](host-library-generated-headers/build.ycxx) | Give a library shared by the host scanner and foreign app a generated header, then watch its score change. |
| [Host library generated sources](host-library-generated-sources/README.md) | [build.ycxx](host-library-generated-sources/build.ycxx) | Mix disk and generated sources in a shared library and watch a private generated sibling change. |
| [Host static archive inputs](host-static-archive-inputs/README.md) | [build.ycxx](host-static-archive-inputs/build.ycxx) | Link a generated archive into a native scanner and watch an archive edit change the app score. |
| [Native host witness](native-host-witness/README.md) | [build.ycxx](native-host-witness/build.ycxx) | Classify a host scanner's exit status and watch an included header change the typed score. |
| [Host producer witness](host-producer-witness/README.md) | [build.ycxx](host-producer-witness/build.ycxx) | Build a host scanner, generate a witness executable through two actions, and trace its score into a cross-target app. |
| [Host planning input](host-planning-inputs/README.md) | [build.ycxx](host-planning-inputs/build.ycxx) | Plan a custom rule from a host-generated inventory and compare edits with equal and changed bytes. |
| [Staged witness](staged-witness/README.md) | [build.ycxx](staged-witness/build.ycxx) | Run a generated analyzer and watch helper edits that preserve or change its typed result. |
| [Module discovery](module-discovery/README.md) | [build.ycxx](module-discovery/build.ycxx) | Inspect named module providers and compare the imports selected for Linux and macOS. |
| [Module providers](module-providers/README.md) | [build.ycxx](module-providers/build.ycxx) | Resolve a shared module through two import paths and compare its provider on Linux and macOS. |
| [Native C++](native/README.md) | [build.ycxx](native/build.ycxx) | Watch a generator input change and inspect the library dependency and source glob. |
| [Custom rule](custom-rule/README.md) | [build.ycxx](custom-rule/build.ycxx) | Use a C++ workspace plugin to generate a retry-limit header and compare a whitespace edit with a digit edit. |
| [Typed capability header](typed-capabilities/README.md) | [build.ycxx](typed-capabilities/build.ycxx) | Inspect the compiler's `<filesystem>` probe and run the app that uses its generated constant. |
| [Package-defined probe](package-probe/README.md) | [build.ycxx](package-probe/build.ycxx) | Edit a codec profile and trace how a plugin classifies shell output as a typed support state. |
| [Shader bundle](shader-bundle/README.md) | [build.ycxx](shader-bundle/build.ycxx) | Generate an inventory, add and remove a shader, and update managed runtime assets under watch. |
| [Shader reflection](shader-reflection/README.md) | [build.ycxx](shader-reflection/build.ycxx) | Compare a shader body edit with a binding edit to see when the generated descriptor table rebuilds native code. |
| [Generated archive](generated-archive/README.md) | [build.ycxx](generated-archive/build.ycxx) | Follow a generated archive through a library dependency and watch an archive edit relink the app. |
| [Foreign Make library](foreign-make/README.md) | [build.ycxx](foreign-make/build.ycxx) | Link an archive built by Make, then watch a vendor header or declared helper edit. |

## Find targets before building

Run these commands from the `custom-rule` example directory.

```sh
ycxx show --targets
ycxx show --targets --format=html > targets.html
```

The catalog lists `app` as `cpp.executable` and `retry_policy` as
`policy.header`. Open `targets.html` and follow either entry's location link
to its declaration. Run the same catalog command from any of the example
directories to see that workspace's targets. [Custom rule manifest](custom-rule/build.ycxx)

Each project declares a target named `app`. The resident daemon can reuse
work from an earlier request in the same workspace.

```sh
ycxx build app
ycxx build app --trace-engine=interactive
ycxx show app --format=html > plan.html
```

The trace shows requests and reuse. Open `plan.html` to read the resolved plan
alongside its build definition. In the generated archive example, follow
`app` through `score` and `bonus` to the `bonus_archive` output in
**Dependencies**. Its location link opens the archive reference in the
displayed manifest. [Generated archive walkthrough](generated-archive/README.md)

Use `ycxx build app --watch` to rerun on edits.

Read the [build design](../../docs/design/build.md) for the rule and artifact model.
