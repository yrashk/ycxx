# Build a freestanding project for two targets

From this directory, with `ycxx` on `PATH`, copy the workspace and build both
target configurations. The source uses core C++ without headers or library
calls. The Linux build links a freestanding ELF executable. The macOS build
links an arm64 Mach-O executable.

```sh
example=$(mktemp -d "${TMPDIR:-/tmp}/ycxx-project-targets.XXXXXX")
cp build.ycxx main.cc "$example"
cd "$example"

ycxx build app --target=x86_64-pc-linux-gnu
ycxx build app --target=arm64-apple-darwin

linux_app=.ycxx/targets/x86_64-pc-linux-gnu/out/app
darwin_app=.ycxx/targets/arm64-apple-darwin/out/app
file "$linux_app" "$darwin_app"

ycxx show app --target=x86_64-pc-linux-gnu --format=html > linux-plan.html
ycxx show app --target=arm64-apple-darwin --format=html > darwin-plan.html
```

Open the two HTML plans to compare the selected target triples and their provenance.
Each explicit target has its own output directory. `ycxx build app` uses the
host default and publishes to `.ycxx/out/app`.

Run the executable that matches your host. It exits with status `42`.

```sh
case "$(uname -s):$(uname -m)" in
    Linux:x86_64) app=$linux_app ;;
    Darwin:arm64) app=$darwin_app ;;
esac
status=0
"$app" || status=$?
printf '%s\n' "$status"
test "$status" -eq 42
```

To watch an edit, append `--watch --trace-engine=interactive` to either build
command. Edit the return value in `main.cc` to see the selected build update.
