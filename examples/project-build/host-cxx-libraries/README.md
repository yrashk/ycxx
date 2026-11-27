# Share C++ libraries between a host tool and a target app

Both the host `scanner` and the selected-target `app` depend on `shared`.
The `left` and `right` libraries below it each depend on `base`. Each
library has a C++ source file and exports a public include directory.

```text
cpp.library "shared" {
    sources = ["shared.cc"]
    public_include_dirs = ["include/shared"]
    deps = ["left", "right"]
}

cpp.host_executable "scanner" {
    sources = ["scanner.cc"]
    deps = ["shared"]
}
```

The scanner and app both include `shared_score.hh`, which includes
`base_score.hh` through the transitive public include directories.
The `config` generator runs the host scanner through
`artifact("scanner", "executable")` and writes its score into a C++ header.
The app checks that constant against its own headers at compile time and
against its linked libraries at runtime. [Build definition](build.ycxx)

## Build for the other target first

Run from this directory on x86-64 Linux or arm64 macOS with `ycxx` on
`PATH`.

```sh
example=$(mktemp -d "${TMPDIR:-/tmp}/ycxx-host-cxx-libraries.XXXXXX")
cp -R build.ycxx generate-score.sh *.cc include "$example"
cd "$example"

case "$(uname -s):$(uname -m)" in
    Linux:x86_64) alternate=arm64-apple-darwin ;;
    Darwin:arm64) alternate=x86_64-pc-linux-gnu ;;
esac

ycxx build app --target="$alternate" --trace-engine=json 2> foreign.jsonl
foreign_app=".ycxx/targets/$alternate/out/app"
case "$alternate" in
    arm64-apple-darwin) file "$foreign_app" | grep 'Mach-O.*arm64' ;;
    x86_64-pc-linux-gnu) file "$foreign_app" | grep 'ELF.*x86-64' ;;
esac
ycxx show app --target="$alternate" --format=html > foreign-plan.html

ycxx build app
status=0
.ycxx/out/app || status=$?
printf '%s\n' "$status"
test "$status" -eq 17
```

The foreign build runs the scanner on the host and links the app for
`$alternate`. In the HTML report's **Target dependencies**, open `shared`
from either `app` or `scanner`. Follow `left` and `right` to the same
`base` card. Follow `app` through `config` to see the staged scanner
input and the links to its declarations.

## Watch a transitive library header

Run watch for the default app.

```sh
ycxx build app --watch --trace-engine=interactive
```

In another terminal, change `base_points = 7` to `base_points = 10` in
`include/base/base_score.hh`. The edit reaches both builds through their
shared library dependencies. After watch reports success, check the new
score.

```sh
status=0
.ycxx/out/app || status=$?
printf '%s\n' "$status"
test "$status" -eq 23
```

Press `q` in the trace view to stop watch.
