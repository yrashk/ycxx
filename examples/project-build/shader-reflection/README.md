# Share a shader module between reflection and runtime packaging

Install `glslc`, `spirv-cross`, and Python 3, and put a current `ycxx` on `PATH`.
The [build definition](build.ycxx) uses `/usr/bin/glslc`, `/usr/bin/spirv-cross`,
and `/usr/bin/python3`. If your tools are elsewhere, locate them with `command -v`
and set their absolute paths in `build.ycxx`. The package uses `cxx()` to build
its C++ rule library. See the [custom rule example](../custom-rule) for compiler
selection.

Run these commands from this directory. Copying the project gives you a
workspace for the edits.

```sh
example=$(mktemp -d "${TMPDIR:-/tmp}/ycxx-shader-reflection.XXXXXX")
cp -R . "$example"
cd "$example"

ycxx build runtime_only --trace-engine=json 2> runtime.jsonl
python3 run.py --runtime-only
python3 run.py --runtime-only --module vertex

ycxx build app --trace-engine=json 2> first.jsonl
python3 run.py 1
ycxx show app
ycxx build app --trace-engine=json 2> unchanged.jsonl
```

The [shader rule package](rules/shaders.cc) defines four rules.

| Rule | Artifact or value |
| --- | --- |
| `shaders.module` | Runs `glslc` once per requested module. Each module declares a named `spirv` output. |
| `shaders.pack` | Stages `vertex:spirv` and `fragment:spirv`, and packs their bytes into the `bundle` artifact. |
| `shaders.reflect` | Stages the same `fragment:spirv` output for `spirv-cross --reflect` and classifies the JSON witness as a `shader.descriptors.v1` table. Each uniform block is a row with unsigned `set` and `binding` fields. |
| `shaders.bindings` | Consumes the `layout` value and writes `scene_bindings.hh` as the `header` artifact. |

This package's table covers uniform buffers (`ubos` in the witness). It accepts
up to 16 blocks named with ASCII letters, digits, and single interior
underscores. Each name begins with a letter.

`runtime_only` needs only `glslc` and Python 3 for the shader actions. Its trace
shows the two module compiles and the pack action. Building `app` then adds the
reflection witness, its typed value and the generated header. `ycxx show app`
lists the artifact edges and the provenance of the descriptor table. The
unchanged request reports zero executed actions.

The app receives `scene.bundle` as a published runtime asset and
`scene_bindings.hh` as a compile input. The starter shader generates these
constants in the `scene` namespace.

```cpp
inline constexpr unsigned descriptor_Material_set = 0u;
inline constexpr unsigned descriptor_Material_binding = 1u;
```

The launcher reads the published bundle under `.ycxx/assets/app/` and passes
its text and the expected binding to
`.ycxx/out/app`. The C++ code checks the set and binding on the `material`
uniform variable in the bundled fragment against its generated `constexpr`
values. `run.py 1` exits with status `0` when both the supplied binding and the
published module agree with the header. Wrong bindings exit with status `4`.
Both executables use status `2` for a missing entry and `3` for malformed
SPIR-V framing. The runtime-only executable checks the selected module's framing.

## Edit the fragment shader

1. Change `0.25` to `0.5` in `shaders/triangle.frag`. Run
   `ycxx build app --trace-engine=json 2> body.jsonl` and `python3 run.py 1`.
   The new SPIR-V runs through packing and reflection. Equal descriptor tables
   let the build reuse the header generation and native app.
2. Change `binding = 1` to `binding = 3` in the same file. Run
   `ycxx build app --trace-engine=json 2> binding.jsonl` and `python3 run.py 3`.
   The changed table regenerates the header, recompiles the app, and publishes
   the bundle with the new binding. `python3 run.py 1` now exits with status `4`.
3. Introduce a GLSL syntax error and rebuild. `python3 run.py 3` still runs the
   last successful app and bundle. Restore the shader to recover the build.

Use `ycxx build app --watch --trace-engine=interactive` to follow the edits in
another terminal. After a successful build, save the plan with
`ycxx show app --format=html > plan.html` and open it in a browser.
