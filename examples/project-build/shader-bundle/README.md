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
cat .ycxx/assets/app/release-note.txt
ycxx show app
ycxx build app --trace-engine=json 2> unchanged.jsonl
```

The `shader_inventory` generator filters comments and blank lines from
`shaders/inventory.txt`. The `scene` rule binds its named output `list` to
`inventory` through `plan_inputs`. The rule package reads those bytes by the
name `inventory` through the rule SDK. Each row gives a unique runtime name, a
`vertex` or `fragment` stage, and a workspace-relative GLSL path.
[Inventory](shaders/inventory.txt) · [Rule package](rules/shaders.cc)

The package emits a private compile action for each row and a public pack
action named `scene`. The packer receives the named `spirv` artifacts in
runtime-name order and writes one `name hex-bytes` line per shader. The first
build runs the external actions `shader_inventory`, `scene/compile_triangle-fragment`,
`scene/compile_triangle-vertex`, `scene`, and `release_note`.

The app requests two runtime assets. The `scene` action supplies `scene.bundle`
and the independent `release_note` generator supplies `release-note.txt`.
To demonstrate loading with a freestanding executable, `run.py` reads
`.ycxx/assets/app/scene.bundle` and passes its contents and a shader name to
`.ycxx/out/app`. The C++ app selects the named entry and checks its SPIR-V magic
and word alignment. It exits with `0` for a valid entry, `2` for a missing name,
or `3` for malformed entry data. The default name is `triangle-fragment`. Both
the build and the app run without a GPU.

## Edit the workspace

- The unchanged build launches zero actions. Append a comment with
  `printf '\n# Build note\n' >> shaders/inventory.txt`. The next build runs only
  `shader_inventory`. Its output bytes are equal, so it reuses the `scene`
  rule worker result and the rest of the build.
- Reorder the inventory rows. The next build runs `shader_inventory` and the
  `scene` rule worker, then reuses the compile and pack actions.
- Change `1.0` to `0.25` in the first component of `color` in
  `shaders/triangle.frag`. The next build runs `scene/compile_triangle-fragment`
  and `scene`. It reuses the inventory, vertex shader, and native executable.
- Introduce a syntax error in the shader and build again. The previous bundle
  remains available to `run.py`. Restore the shader, rebuild, then remove the
  published bundle and rebuild once more to see the cached result restored.

Run `ycxx show app --format=html > plan.html` to inspect the plan and its named
artifact edges.

## Add and remove a shader

Add the checked-in accent shader to the editable inventory and build again.

```sh
cp shaders/inventory.txt shaders/inventory.before
printf '%s\n' 'triangle-accent fragment shaders/accent.frag' >> shaders/inventory.txt
ycxx build app --trace-engine=json 2> add.jsonl
python3 run.py triangle-accent
```

`shader_inventory` produces a new list. The `scene` rule worker adds
`scene/compile_triangle-accent` and updates the `scene` pack action. The build
reuses both original shader compiles and the native executable.

Restore the previous inventory. The next published bundle contains the two
original shaders, whose compile results are reused.

```sh
mv shaders/inventory.before shaders/inventory.txt
ycxx build app --trace-engine=json 2> remove.jsonl
python3 run.py triangle-vertex
status=0
python3 run.py triangle-accent || status=$?
test "$status" -eq 2
```

## Recover from a failed plan under watch

After the first successful build, add a local file beside the two managed assets.
Start watch in another terminal in the same workspace and wait for the initial
build to finish.

```sh
assets=.ycxx/assets/app
printf 'Local note\n' > "$assets/local.txt"
```

```sh
ycxx build app --watch --trace-engine=interactive
```

In the original terminal, append an inventory row with an invalid stage.

```sh
cp shaders/inventory.txt shaders/inventory.before
printf '%s\n' 'broken compute shaders/accent.frag' >> shaders/inventory.txt
```

Watch runs `shader_inventory` and reports the `scene` rule worker's diagnostic.
The last successful bundle remains available to `run.py`. Restore the inventory
and wait for watch to report success.

```sh
python3 run.py
mv shaders/inventory.before shaders/inventory.txt
```

## Rename and remove a runtime asset under watch

Keep watch running from the previous section.

In `build.ycxx`, change `outputs = { note = "release-note.txt" }` to
`outputs = { note = "shader-release.txt" }`. Once watch reports success, the old
name is gone. Check the new name, the local file, and the runnable app from the
first terminal.

```sh
test ! -e "$assets/release-note.txt" &&
cat "$assets/shader-release.txt" "$assets/local.txt" &&
python3 run.py
```

Now set `runtime_assets = [artifact("scene", "bundle")]` in `build.ycxx`. Wait
for watch to report success again. The next published set drops
`shader-release.txt` and carries forward `scene.bundle` and the local file.

```sh
test ! -e "$assets/shader-release.txt" &&
cat "$assets/local.txt" &&
python3 run.py
```
