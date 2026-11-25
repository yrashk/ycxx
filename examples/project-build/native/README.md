# Native project build

From this directory, with `ycxx` on `PATH`, build and run the app.

```sh
ycxx build app
status=0
.ycxx/out/app || status=$?
printf '%s\n' "$status"
```

The app exits with status `42`, twice the value in `input.txt`. The `arithmetic`
library exports `include/` to the app. The source glob selects `src/main.cc`
and excludes `src/twice_test.cc`, which has its own `main`. The `input_header`
generator runs `/bin/sh` with the declared script and value inputs and produces
the header consumed by `src/main.cc`.

After building, run `ycxx show app` to see the source selection, library
dependency, and the generator's tool, inputs, and output.

To see reuse, run `ycxx build app --trace-engine=json` twice from a fresh copy
of this directory. The `actions_executed` count on the final line includes the
generator on the first run and is `0` on the unchanged run.

## Watch a generator input

Start a watch in this directory.

```sh
ycxx build app --watch --trace-engine=stream
```

In a second terminal in the same directory, change the input to `22`.

```sh
printf '%s\n' 22 > input.txt
```

When the watch reports a successful rebuild, rerun the app as above to see
status `44`. Press Ctrl-C in the watch terminal to stop it, then restore the
input with `printf '%s\n' 21 > input.txt`.
