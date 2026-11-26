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
python3 run.py 1 --texture-count 2
ycxx show app
ycxx build app --trace-engine=json 2> unchanged.jsonl
```

The [shader rule package](rules/shaders.cc) defines four rules.

| Rule | Artifact or value |
| --- | --- |
| `shaders.module` | Runs `glslc` once per requested module. Each module declares a named `spirv` output. |
| `shaders.pack` | Stages `vertex:spirv` and `fragment:spirv`, and packs their bytes into the `bundle` artifact. |
| `shaders.reflect` | Stages the same `fragment:spirv` output for `spirv-cross --reflect` and classifies the JSON witness as a `shader.descriptors.v2` table. Each row has a descriptor kind, set, binding, and element count. |
| `shaders.bindings` | Consumes the `layout` value and writes `scene_bindings.hh` as the `header` artifact. |

The table accepts up to 16 descriptors across uniform buffers (`ubos` in the
witness) and fixed one-dimensional `sampler2D` arrays (`textures`). A descriptor
name starts with an ASCII letter and may contain ASCII letters, digits, and
single interior underscores. The fragment has one descriptor of each kind.

```glsl
layout(set = 0, binding = 1) uniform Material {
    vec4 tint;
} material;

layout(set = 1, binding = 7) uniform sampler2D scene_textures[2];
```

`runtime_only` needs only `glslc` and Python 3 for the shader actions. Its trace
shows the two module compiles and the pack action. Building `app` then adds the
reflection witness, its typed value and the generated header. `ycxx show app`
lists the artifact edges and the provenance of the descriptor table. The
unchanged request reports zero executed actions.

The app receives `scene.bundle` as a published runtime asset and
`scene_bindings.hh` as a compile input. The starter shader generates these
constants in the `scene` namespace.

```cpp
enum class descriptor_kind : unsigned { uniform_buffer = 1, combined_sampler = 2 };

inline constexpr unsigned descriptor_Material_set = 0u;
inline constexpr unsigned descriptor_Material_binding = 1u;
inline constexpr unsigned descriptor_Material_element_count = 1u;
inline constexpr descriptor_kind descriptor_Material_kind = descriptor_kind::uniform_buffer;

inline constexpr unsigned descriptor_scene_textures_set = 1u;
inline constexpr unsigned descriptor_scene_textures_binding = 7u;
inline constexpr unsigned descriptor_scene_textures_element_count = 2u;
inline constexpr descriptor_kind descriptor_scene_textures_kind = descriptor_kind::combined_sampler;
```

The launcher reads the published bundle under `.ycxx/assets/app/` and passes
its text, the expected Material binding, and the optional sampler count
to `.ycxx/out/app`. The C++ code checks the two variables' descriptor sets
and bindings in the bundled fragment against its generated `constexpr`
values. It follows the sampler's `UniformConstant` pointer to a
one-dimensional array of sampled images and checks the literal array length.
`python3 run.py 1` defaults to two textures. A binding or count mismatch exits
with status `4`.
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
3. Change `scene_textures[2]` to `scene_textures[3]`. Run
   `ycxx build app --trace-engine=json 2> count.jsonl` and
   `python3 run.py 3 --texture-count 3`. The element count changes the table
   and header, so the app rebuilds. With `--texture-count 2`, it exits with
   status `4`.
4. Introduce a GLSL syntax error and rebuild.
   `python3 run.py 3 --texture-count 3` still runs the last successful app and
   bundle. Restore the shader to recover the build.

Use `ycxx build app --watch --trace-engine=interactive` to follow the edits in
another terminal. After a successful build, save the plan with
`ycxx show app --format=html > plan.html` and open it in a browser.
