# Link a generated archive

Run this example on Linux x86-64 or macOS arm64 with `ycxx` on `PATH` and the
system C++ tools installed. If `c++` or `ar` is installed elsewhere, set its
absolute path in `build.ycxx`. Run these commands from this directory.

```sh
example=$(mktemp -d "${TMPDIR:-/tmp}/ycxx-generated-archive.XXXXXX")
cp -R . "$example"
cd "$example"

ycxx build app --trace-engine=json 2> first.jsonl
status=0
.ycxx/out/app || status=$?
printf '%s\n' "$status"
test "$status" -eq 42

ycxx show app --format=html > plan.html
ycxx build app --trace-engine=json 2> unchanged.jsonl
```

The `bonus_object` action compiles `archive_bonus.cc`. The `bonus_archive`
action stages that object and runs `ar rcs` to create an indexed `libbonus.a`
in the host's archive format. The `bonus` library exports the archive with
`public_link_inputs`. The executable depends on `score`, which depends on
`bonus`, so the archive supplies `archived_bonus` to the native link. The app
returns the score, initially `35 + 7`.

Open `plan.html` to follow the object artifact into `bonus_archive` and the
archive artifact into the executable. The first trace includes both external
actions and the native link. The unchanged trace reports zero actions.

## Watch an archive edit

Start watch in a second terminal in the copied workspace.

```sh
ycxx build app --watch --trace-engine=interactive
```

After its initial build, change `return 7;` to `return 8;` in `archive_bonus.cc`.
Watch runs `bonus_object`, `bonus_archive`, and the native link. The native
translation units reuse their results. After watch reports success, run the
app again in the first terminal.

```sh
status=0
.ycxx/out/app || status=$?
printf '%s\n' "$status"
test "$status" -eq 43
```

Press `q` in the trace view to stop watch.
