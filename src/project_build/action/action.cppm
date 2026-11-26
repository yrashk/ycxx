// External build tools run against staged input snapshots. The caller owns reuse and installation.
module;
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

export module ycxx.build.action;

import std;
import ycxx.execution;
import :spawn;

export namespace ycxx::build {

struct Artifact {
  std::vector<std::uint8_t> bytes;
  bool executable = false;
};

struct ActionInput {
  std::string name;
  std::string path;  // relative to inputs/, preserving the source's directory layout
  Artifact contents;
};

struct ActionOutput {
  std::string name;
  std::string filename;  // a filename inside outputs/
};

struct ActionArg {
  enum class Kind : std::uint8_t { literal, input, output, target_triple };
  Kind kind = Kind::literal;
  std::string value;  // an argument word or the name of a declared input or output
};

struct Action {
  std::string tool;  // resolved absolute host executable
  std::vector<ActionArg> args;
  std::vector<std::pair<std::string, std::string>> environment;
  std::vector<ActionOutput> outputs;
};

struct NamedArtifact {
  std::string name;
  Artifact contents;
};

enum class ActionCompletion { exited, signalled, cancelled, timed_out, setup_failed, output_failed };

struct ActionResult {
  int exit_status = 0;
  std::string out;
  std::string err;
  std::vector<NamedArtifact> outputs;
  ActionCompletion completion = ActionCompletion::exited;
};

// Runs a tool in a private working directory within the caller's existing scratch root. The
// tool waits for all output writers before it exits. Artifact arguments are relative to the
// working directory. All named outputs become available together on success. The tool can use
// its ambient filesystem, so the caller needs an input contract before reusing its result.
ActionResult execute(const Action& action, std::span<const ActionInput> inputs,
                     const std::filesystem::path& scratch_root, ycxx::exec::inplace_stop_token stop = {},
                     std::optional<std::size_t> log_limit = std::nullopt);

}  // namespace ycxx::build

namespace ycxx::build {
namespace {

namespace fs = std::filesystem;

std::string os_error(std::string_view operation, int error) {
  return std::format("{}: {}", operation, ::strerror(error));
}

ActionResult failed(std::string message, ActionCompletion completion = ActionCompletion::setup_failed) {
  return {.exit_status = 1, .out = {}, .err = std::move(message), .outputs = {}, .completion = completion};
}

bool valid_path(std::string_view text) {
  if (text.empty() || text.contains('\0')) return false;
  const fs::path path(text);
  return path.is_relative() && !path.filename().empty() &&
         std::ranges::none_of(path, [](const fs::path& part) { return part == "." || part == ".."; });
}

struct Command {
  std::vector<std::string> args;
  std::vector<std::string> environment;
};

std::expected<Command, std::string> prepare(const Action& action, std::span<const ActionInput> inputs) {
  if (action.tool.empty() || action.tool.contains('\0') || !fs::path(action.tool).is_absolute())
    return std::unexpected("action tool must be an absolute executable path");

  std::map<std::string_view, std::string> input_paths;
  std::set<std::string> paths;
  for (const auto& input : inputs) {
    if (!valid_path(input.path)) return std::unexpected("invalid action input path: " + input.path);
    auto path = (fs::path("inputs") / fs::path(input.path).lexically_normal()).native();
    if (input.name.empty() || !input_paths.emplace(input.name, path).second)
      return std::unexpected("empty or duplicate action input name: " + input.name);
    if (!paths.insert(std::move(path)).second)
      return std::unexpected("duplicate action input path: " + input.path);
  }

  std::map<std::string_view, std::string> output_paths;
  for (const auto& output : action.outputs) {
    if (!valid_path(output.filename) || output.filename.contains('/'))
      return std::unexpected("invalid action output filename: " + output.filename);
    auto path = "outputs/" + output.filename;
    if (output.name.empty() || !output_paths.emplace(output.name, path).second)
      return std::unexpected("empty or duplicate action output name: " + output.name);
    if (!paths.insert(std::move(path)).second)
      return std::unexpected("duplicate action output path: " + output.filename);
  }

  Command cmd{{action.tool}, {}};
  for (const auto& arg : action.args) {
    if (arg.kind == ActionArg::Kind::literal || arg.kind == ActionArg::Kind::target_triple) {
      if (arg.value.contains('\0')) return std::unexpected("action argument contains NUL");
      cmd.args.push_back(arg.value);
      continue;
    }
    const auto& bindings = arg.kind == ActionArg::Kind::input ? input_paths : output_paths;
    auto match = bindings.find(arg.value);
    if (match == bindings.end()) return std::unexpected("undeclared action argument: " + arg.value);
    cmd.args.push_back(match->second);
  }

  std::set<std::string_view> names;
  for (const auto& [name, value] : action.environment) {
    if (name.empty() || name.contains('=') || name.contains('\0') || value.contains('\0') ||
        !names.insert(name).second)
      return std::unexpected("invalid or duplicate action environment entry: " + name);
    cmd.environment.push_back(name + "=" + value);
  }
  std::ranges::sort(cmd.environment);
  return cmd;
}

struct Scratch {
  const fs::path path;
  ~Scratch() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

std::optional<std::string> stage(const fs::path& root, std::span<const ActionInput> inputs) {
  std::error_code ec;
  fs::create_directory(root / "outputs", ec);
  if (ec) return "cannot create action output directory: " + ec.message();
  for (const auto& input : inputs) {
    const auto path = root / "inputs" / input.path;
    fs::create_directories(path.parent_path(), ec);
    if (ec) return "cannot stage action input " + input.path + ": " + ec.message();
    std::ofstream file(path, std::ios::binary | std::ios::noreplace);
    if (!input.contents.bytes.empty())
      file.write(reinterpret_cast<const char*>(input.contents.bytes.data()),
                 static_cast<std::streamsize>(input.contents.bytes.size()));
    file.close();
    if (!file) return "cannot stage action input " + input.path;
    using enum fs::perms;
    auto perms = owner_read | owner_write;
    if (input.contents.executable) perms |= owner_exec;
    fs::permissions(path, perms, fs::perm_options::replace, ec);
    if (ec) return "cannot set action input permissions for " + input.path + ": " + ec.message();
  }
  return std::nullopt;
}

struct SpawnFiles {
  posix_spawn_file_actions_t state;
  int error = ::posix_spawn_file_actions_init(&state);
  ~SpawnFiles() { if (error == 0) ::posix_spawn_file_actions_destroy(&state); }
};

struct SpawnFlags {
  posix_spawnattr_t state;
  int error = ::posix_spawnattr_init(&state);
  ~SpawnFlags() { if (error == 0) ::posix_spawnattr_destroy(&state); }
};

std::vector<char*> argv(std::vector<std::string>& entries) {
  std::vector<char*> args;
  for (auto& entry : entries) args.push_back(entry.data());
  args.push_back(nullptr);
  return args;
}

std::expected<pid_t, std::string> spawn(const fs::path& cwd, Command& command) {
  SpawnFiles files;
  SpawnFlags flags;
  if (files.error != 0) return std::unexpected(os_error("cannot initialize action files", files.error));
  if (flags.error != 0) return std::unexpected(os_error("cannot initialize action flags", flags.error));
  if (int error = ::posix_spawn_file_actions_addchdir_np(&files.state, cwd.c_str()))
    return std::unexpected(os_error("cannot set action working directory", error));
  if (int error = ::posix_spawn_file_actions_addopen(&files.state, STDIN_FILENO, "/dev/null", O_RDONLY, 0))
    return std::unexpected(os_error("cannot redirect action input", error));
  if (int error = ::posix_spawn_file_actions_addopen(&files.state, STDOUT_FILENO, ".stdout", O_WRONLY | O_CREAT | O_EXCL, 0600))
    return std::unexpected(os_error("cannot redirect action stdout", error));
  if (int error = ::posix_spawn_file_actions_addopen(&files.state, STDERR_FILENO, ".stderr", O_WRONLY | O_CREAT | O_EXCL, 0600))
    return std::unexpected(os_error("cannot redirect action stderr", error));
  short attributes = POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF;
  if (int error = isolate_spawn(files.state, attributes))
    return std::unexpected(os_error("cannot close inherited action descriptors", error));
  sigset_t signals;
  sigemptyset(&signals);
  sigaddset(&signals, SIGPIPE);
  if (int error = ::posix_spawnattr_setsigdefault(&flags.state, &signals))
    return std::unexpected(os_error("cannot set action signal defaults", error));
  if (int error = ::posix_spawnattr_setflags(&flags.state, attributes))
    return std::unexpected(os_error("cannot set action spawn flags", error));
  if (int error = ::posix_spawnattr_setpgroup(&flags.state, 0))
    return std::unexpected(os_error("cannot set action process group", error));

  auto args = argv(command.args);
  auto env = argv(command.environment);
  pid_t pid;
  if (int error = ::posix_spawn(&pid, args.front(), &files.state, &flags.state, args.data(), env.data()))
    return std::unexpected(os_error("cannot start action tool " + command.args.front(), error));
  return pid;
}

std::expected<int, std::string> wait_for(pid_t pid, ycxx::exec::inplace_stop_token stop) {
  for (;;) {
    if (stop.stop_requested()) {
      ::kill(-pid, SIGKILL);
      while (::waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
      return std::unexpected("action cancelled");
    }
    int status;
    const auto reaped = ::waitpid(pid, &status, stop.stop_possible() ? WNOHANG : 0);
    if (reaped > 0) return status;
    if (reaped < 0) {
      if (errno == EINTR) continue;
      return std::unexpected(os_error("cannot wait for action tool", errno));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

struct Fd {
  const int value;
  ~Fd() { if (value >= 0) ::close(value); }
};

using FileIdentity = std::pair<dev_t, ino_t>;

struct File {
  Artifact contents;
  FileIdentity identity;
};

std::expected<File, std::string> read_file(int dir, const std::string& name,
                                          std::optional<std::size_t> limit = std::nullopt) {
  const Fd fd{::openat(dir, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
  if (fd.value < 0) return std::unexpected(os_error("cannot read action output " + name, errno));
  struct stat entry{};
  if (::fstat(fd.value, &entry) < 0) return std::unexpected(os_error("cannot stat action output " + name, errno));
  if (!S_ISREG(entry.st_mode)) return std::unexpected("action output is not a regular file: " + name);
  File result{{{}, (entry.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0}, {entry.st_dev, entry.st_ino}};
  std::array<std::uint8_t, 16384> buffer;
  for (;;) {
    auto n = ::read(fd.value, buffer.data(), buffer.size());
    if (n == 0) return result;
    if (n < 0) {
      if (errno == EINTR) continue;
      return std::unexpected(os_error("cannot read action output " + name, errno));
    }
    if (limit && static_cast<std::size_t>(n) > *limit - result.contents.bytes.size())
      return std::unexpected(std::format("action log {} exceeds {} bytes", name, *limit));
    result.contents.bytes.insert(result.contents.bytes.end(), buffer.begin(), buffer.begin() + n);
  }
}

std::expected<std::string, std::string> read_log(int dir, const std::string& file,
                                               std::optional<std::size_t> limit) {
  auto captured = read_file(dir, file, limit);
  if (!captured) return std::unexpected(std::move(captured.error()));
  const auto& bytes = captured->contents.bytes;
  return std::string(bytes.begin(), bytes.end());
}

}  // namespace

ActionResult execute(const Action& action, std::span<const ActionInput> inputs,
                     const fs::path& scratch_root, ycxx::exec::inplace_stop_token stop,
                     std::optional<std::size_t> log_limit) {
  auto cmd = prepare(action, inputs);
  if (!cmd) return failed(std::move(cmd.error()));
  if (stop.stop_requested()) return failed("action cancelled", ActionCompletion::cancelled);

  std::error_code ec;
  auto root = fs::absolute(scratch_root, ec);
  if (ec) return failed("cannot locate action scratch directory: " + ec.message());
  auto path = (root / "action-XXXXXX").native();
  if (!::mkdtemp(path.data())) return failed(os_error("cannot create action scratch directory", errno));
  const Scratch scratch{fs::path(path)};
  if (auto error = stage(scratch.path, inputs)) return failed(std::move(*error));
  const Fd cwd{::open(scratch.path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
  if (cwd.value < 0) return failed(os_error("cannot open action scratch directory", errno));
  const Fd outputs{::openat(cwd.value, "outputs", O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
  if (outputs.value < 0) return failed(os_error("cannot open action output directory", errno));

  if (stop.stop_requested()) return failed("action cancelled", ActionCompletion::cancelled);
  auto child = spawn(scratch.path, *cmd);
  if (!child) return failed(std::move(child.error()));
  auto status = wait_for(*child, stop);
  if (stop.stop_requested()) return failed("action cancelled", ActionCompletion::cancelled);
  if (!status) return failed(std::move(status.error()));

  auto out = read_log(cwd.value, ".stdout", log_limit);
  if (!out) return failed(std::move(out.error()), ActionCompletion::output_failed);
  auto err = read_log(cwd.value, ".stderr", log_limit);
  if (!err) return failed(std::move(err.error()), ActionCompletion::output_failed);
  const bool exited = WIFEXITED(*status);
  const int code = exited ? WEXITSTATUS(*status) : 128 + WTERMSIG(*status);
  ActionResult result{code, std::move(*out), std::move(*err), {},
                      exited ? ActionCompletion::exited : ActionCompletion::signalled};
  if (code != 0) return result;

  std::vector<NamedArtifact> artifacts;
  std::set<FileIdentity> seen;
  for (const auto& output : action.outputs) {
    auto file = read_file(outputs.value, output.filename);
    if (file && !seen.insert(file->identity).second)
      file = std::unexpected("action output aliases another declared output: " + output.filename);
    if (!file) {
      result.exit_status = 1;
      result.completion = ActionCompletion::output_failed;
      if (!result.err.empty() && result.err.back() != '\n') result.err.push_back('\n');
      result.err += file.error();
      return result;
    }
    artifacts.push_back({output.name, std::move(file->contents)});
  }
  if (stop.stop_requested()) return failed("action cancelled", ActionCompletion::cancelled);
  result.outputs = std::move(artifacts);
  return result;
}

}  // namespace ycxx::build
