# Typed capability header

`build.ycxx` asks whether the compiler that built the bundled C++ rule package
can include `<filesystem>`. The build exports that result as a named header
artifact. The application uses its `constexpr` value as a template argument.

```sh
ycxx build app --trace-engine=json
ycxx build app --trace-engine=json
status=0
.ycxx/out/app || status=$?
printf 'status=%s\n' "$status"
ycxx show app --format=html > plan.html
```

The second build reports `"actions_executed":0`. The exit status depends on the
selected compiler. The HTML report includes its identity and the probe result.

| Exit status | Probe result |
| --- | --- |
| 0 | The compiler accepted the header witness |
| 10 | The compiler rejected the witness |
| 20 | The probe could not determine support |
