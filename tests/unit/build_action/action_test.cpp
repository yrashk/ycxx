#include <stdlib.h>
#include <unistd.h>

import std;
import ycxx.build.action;
import ycxx.hash;
import ycxx.unit;

namespace {

using namespace ycxx;
using namespace ycxx::build;
using namespace ycxx::unit;
namespace fs = std::filesystem;

Action shell(std::string script) {
  return {.tool = "/bin/sh",
          .args = {{.value = "-c"}, {.value = std::move(script)}, {.value = "action-test"}},
          .environment = {},
          .outputs = {}};
}

std::string text(const Artifact& artifact) {
  return std::string(artifact.bytes.begin(), artifact.bytes.end());
}

void stages_named_outputs(const fs::path& root) {
  Action action = shell("printf '%s' \"$VALUE\" > \"$2\"; /bin/cat \"$1\" > \"$3\"; printf stdout; printf stderr >&2");
  action.environment = {{"VALUE", "header"}};
  action.outputs = {{"header", "generated.h"}, {"source", "generated.cc"}};
  action.args.push_back({ActionArg::Kind::input, "schema"});
  action.args.push_back({ActionArg::Kind::output, "header"});
  action.args.push_back({ActionArg::Kind::output, "source"});
  std::array inputs{ActionInput{"schema", "pkg/schema.idl", {{'A', 0, 'B'}}}};

  auto first = execute(action, inputs, root);
  check(first.exit_status == 0, first.err);
  check(first.out == "stdout" && first.err == "stderr", "separate process stdout and stderr");
  check(first.outputs.size() == 2, "both named outputs returned");
  if (first.outputs.size() != 2) return;
  check(first.outputs[0].name == "header" && text(first.outputs[0].contents) == "header", "header output");
  check(first.outputs[1].name == "source" && first.outputs[1].contents.bytes == inputs[0].contents.bytes,
        "source contains staged binary input");

  inputs[0].contents.bytes[0] = 'C';
  auto changed = execute(action, inputs, root);
  check(changed.exit_status == 0, changed.err);
  check(changed.outputs.size() == 2, "both outputs returned after editing the input");
  if (changed.outputs.size() != 2) return;
  check(fingerprint(first.outputs[0].contents) == fingerprint(changed.outputs[0].contents),
        "unchanged sibling has the same artifact fingerprint");
  check(fingerprint(first.outputs[1].contents) != fingerprint(changed.outputs[1].contents),
        "edited source changes only its artifact fingerprint");

  action.args[1].value = "printf corrupted > \"$2\"; printf failed >&2; exit 7";
  auto broken = execute(action, inputs, root);
  check(broken.exit_status == 7 && broken.err == "failed", "preserves process failure and diagnostics");
  check(broken.outputs.empty(), "failure exposes no output values");
  check(text(first.outputs[0].contents) == "header", "previous successful artifact remains usable");

  action.args[1].value = "printf partial > \"$2\"";
  broken = execute(action, inputs, root);
  check(broken.exit_status != 0 && broken.err.contains("generated.cc"), "missing output names the missing file");
  check(broken.outputs.empty(), "a missing sibling prevents publication of all outputs");

  action.args[1].value = "printf executable > \"$2\"; /bin/chmod +x \"$2\"; printf '' > \"$3\"";
  auto executable = execute(action, inputs, root);
  check(executable.exit_status == 0 && executable.outputs.size() == 2 &&
        executable.outputs[0].contents.executable && !executable.outputs[1].contents.executable,
        "captures executability separately for each artifact");

  action.args[1].value = "/bin/ln -s /dev/null \"$2\"; printf '' > \"$3\"";
  broken = execute(action, inputs, root);
  check(broken.exit_status != 0 && broken.outputs.empty(), "symlink is not an artifact output");

  action.args[1].value = "printf same > \"$2\"; /bin/ln \"$2\" \"$3\"";
  broken = execute(action, inputs, root);
  check(broken.exit_status != 0 && broken.err.contains("aliases") && broken.outputs.empty(),
        "both declared paths must name distinct output files");
  check(fs::is_empty(root), "private working directories are removed after outcomes");
}

void rejects_bad_declarations(const fs::path& root) {
  Action action = shell("printf invoked");
  action.outputs = {{"bad", "../escape"}};
  auto bad_path = execute(action, {}, root);
  check(bad_path.exit_status != 0 && bad_path.out.empty(), "reject escaping output before starting tool");

  action.outputs.clear();
  action.args.push_back({ActionArg::Kind::input, "missing"});
  auto undeclared = execute(action, {}, root);
  check(undeclared.exit_status != 0 && undeclared.err.contains("missing"), "reject undeclared argument");

  action.args.pop_back();
  std::array inputs{ActionInput{"a", "nested/file", {}}, ActionInput{"b", "nested//file", {}}};
  auto duplicate = execute(action, inputs, root);
  check(duplicate.exit_status != 0 && duplicate.err.contains("duplicate"), "reject equivalent input paths");

  action.tool = "/nonexistent-ycxx-action-tool";
  auto missing_tool = execute(action, {}, root);
  check(missing_tool.exit_status != 0 && missing_tool.err.contains("cannot start action tool"), "report failed spawn");
  check(fs::is_empty(root), "invalid actions and failed spawn leave scratch clean");
}

void cancels_foreground_process(const fs::path& root) {
  const auto started = root / "started";
  Action action = shell("printf started > \"$1\"; while :; do :; done");
  action.args.push_back({.value = started.native_encoded_string()});
  std::inplace_stop_source stop;
  std::thread stopper([&] {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!fs::exists(started) && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    stop.request_stop();
  });
  auto cancelled = execute(action, {}, root, stop.get_token());
  stopper.join();
  check(fs::remove(started), "tool started before cancellation");
  check(cancelled.exit_status != 0 && cancelled.err == "action cancelled" && cancelled.outputs.empty(),
        "cancel the running tool and expose no result");
  check(fs::is_empty(root), "cancelled action leaves scratch clean");
}

}  // namespace

int main() {
  auto directory = (fs::temp_directory_path() / "ycxx-action-test-XXXXXX").native_encoded_string();
  if (!::mkdtemp(directory.data())) {
    check(false, "could not create test scratch root");
    return result("build action");
  }
  const fs::path root(directory);
  stages_named_outputs(root);
  rejects_bad_declarations(root);
  cancels_foreground_process(root);
  fs::remove_all(root);
  return result("build action");
}
