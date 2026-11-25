# Build and run a shader bundle

With a current `ycxx` on `PATH`, install `glslc` and Python 3. If either tool is
installed elsewhere, set its absolute path in `build.ycxx`. The defaults are
`/usr/bin/glslc` and `/usr/bin/python3`.

```sh
example=$(mktemp -d "${TMPDIR:-/tmp}/ycxx-shader-bundle.XXXXXX")
cp -R . "$example"
cd "$example"

ycxx build app --trace-engine=json 2> first.jsonl
python3 run.py
python3 run.py triangle-vertex
ycxx show app
ycxx build app --trace-engine=json 2> unchanged.jsonl
```

The [rule package](rules/shaders.cc) reads the declared
[inventory](shaders/inventory.txt) through the rule SDK. Each row gives a unique
runtime name, a `vertex` or `fragment` stage, and a workspace-relative GLSL path.
Blank lines and lines beginning with `#` are allowed. The package emits a private
compile action for each row and a public pack action named `scene`. The packer
receives the named `spirv` artifacts in runtime-name order and writes one
`name hex-bytes` line per shader.

The app requests the `bundle` artifact as a runtime asset. To demonstrate loading
with a freestanding executable, `run.py` reads `.ycxx/assets/app/scene.bundle` and
passes its contents and a shader name to `.ycxx/out/app`. The C++ app selects the
named entry and checks its SPIR-V magic and word alignment. It exits with `0` for
a valid entry, `2` for a missing name, or `3` for malformed entry data. The default
name is `triangle-fragment`.

## Edit the workspace

- The unchanged build launches zero actions. Reordering inventory rows reruns the
  rule worker and preserves the compile and pack recipes.
- Change `1.0` to `0.25` in the first component of `color` in
  `shaders/triangle.frag`. The next build recompiles that shader and repacks the
  bundle. It reuses the vertex shader and native executable.
- Introduce a syntax error in the shader and build again. The previous bundle
  remains available to `run.py`. Restore the shader, rebuild, then remove the
  published bundle and rebuild once more to see the cached result restored.

Run `ycxx show app --format=html > plan.html` to inspect the plan and its named
artifact edges. Use `ycxx build app --watch --trace-engine=interactive` to follow
the edits in another terminal.
