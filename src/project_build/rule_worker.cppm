// The rule library and its callbacks live in a short-lived process (#1257).
module;
#include <dlfcn.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
export module ycxx.build.rule_worker;

import std;
import ycxx.build.rule_protocol;
import ycxx.lsp;

export namespace ycxx::build {

// The SDK staged as an input when compiling a rule package.
std::string_view rule_sdk_header();

// Arguments name the staged library, JSON request and output, respectively.
int run_rule_worker(std::span<const std::string_view> args);
// The staged library exports the same symbol with a probe-specific witness result.
int run_shared_probe(std::span<const std::string_view> args);
int run_executable_probe(std::span<const std::string_view> args);

}  // namespace ycxx::build

namespace ycxx::build {
namespace {

constexpr char sdk_header[] = {
#embed "../../resources/rules/rule_api.h"
    , 0};
constexpr std::size_t max_request_size = 8 * 1024 * 1024;
constexpr std::size_t max_definition_size = 1024 * 1024;
constexpr std::size_t max_diagnostic_size = 4096;
constexpr std::uint32_t abi_version = 1;
constexpr std::uint32_t max_capability_support = 2;  // YCXX_RULE_INDETERMINATE_V1

// The C ABI in the staged SDK header. No C++ objects or ownership cross this boundary.
struct Bytes {
  const char* data;
  std::size_t size;
};

struct Capability {
  const char* name;
  std::uint32_t support;
};

struct Witness {
  const char* name;
  int exit_status;
  Bytes out;
  Bytes err;
};

struct Host {
  std::uint32_t abi_version;
  std::size_t struct_size;
  const char* rule;
  const char* target;
  Bytes definition;
  void* context;
  int (*attribute)(void*, const char*, Bytes*);
  int (*read)(void*, const char*, Bytes*);
  int (*emit)(void*, const char*, std::size_t);
  const Capability* capabilities;
  std::size_t capability_count;
  int (*diagnose)(void*, const char*, std::size_t);
  const Witness* witness;
  int (*export_support)(void*, const char*, std::uint32_t);
};

struct Library {
  void* handle;
  ~Library() { if (handle) ::dlclose(handle); }
};

int fail(std::string_view detail) {
  std::println(std::cerr, "ycxx: rule worker: {}", detail);
  return 1;
}

bool valid_key(std::string_view value) { return !value.empty() && !value.contains('\0'); }

std::expected<RuleWorkerRequest, std::string> load_request(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return std::unexpected("cannot open rule request");

  std::string text;
  std::array<char, 16384> buffer;
  while (file.read(buffer.data(), buffer.size()) || file.gcount() != 0) {
    const auto n = static_cast<std::size_t>(file.gcount());
    if (n > max_request_size - text.size()) return std::unexpected("rule request exceeds 8 MiB");
    text.append(buffer.data(), n);
  }
  if (!file.eof() || file.bad()) return std::unexpected("cannot read rule request");
  auto json = lsp::parse(text);
  if (!json) return std::unexpected(std::move(json.error()));
  auto request = lsp::from_json<RuleWorkerRequest>(*json);
  if (!request) return request;
  if (request->version != abi_version) return std::unexpected("unsupported rule request version");
  if (!valid_key(request->rule) || !valid_key(request->target))
    return std::unexpected("invalid rule or target name");
  if (const auto& witness = request->witness) {
    if (!valid_key(witness->name) || witness->exit_status < 0 || witness->exit_status > 255)
      return std::unexpected("invalid rule witness name or exit status");
    if (witness->out.size() > rule_witness_stream_limit || witness->err.size() > rule_witness_stream_limit)
      return std::unexpected("rule witness stream exceeds 64 KiB");
  }

  std::set<std::string_view> keys;
  for (const auto& attribute : request->attributes)
    if (!valid_key(attribute.name) || !keys.insert(attribute.name).second)
      return std::unexpected("invalid or duplicate rule attribute");
  keys.clear();
  for (const auto& read : request->reads)
    if (!valid_key(read.path) || !keys.insert(read.path).second)
      return std::unexpected("invalid or duplicate rule read");
  if (request->capabilities) {
    auto& capabilities = *request->capabilities;
    std::ranges::sort(capabilities, {}, &RuleCapabilityValue::name);
    std::string_view previous;
    for (const auto& capability : capabilities) {
      if (!valid_key(capability.name) || capability.name == previous)
        return std::unexpected("invalid or duplicate rule capability name");
      if (capability.support > max_capability_support)
        return std::unexpected("invalid rule capability support");
      previous = capability.name;
    }
  }
  return request;
}

using RuleInputs = std::map<std::string, std::optional<std::string>, std::less<>>;

std::expected<RuleInputs, std::string> load_reads(const RuleWorkerRequest& request) {
  RuleInputs inputs;
  for (const auto& read : request.reads) {
    if (!read.staged) {
      inputs.emplace(read.path, std::nullopt);
      continue;
    }
    const auto& spelling = *read.staged;
    const std::filesystem::path path(spelling);
    if (!valid_key(spelling) || !spelling.starts_with("inputs/") ||
        std::ranges::any_of(path, [](const auto& part) { return part == "." || part == ".."; }))
      return std::unexpected("invalid staged rule input path");
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::unexpected(std::format("cannot open staged rule input '{}'", read.path));
    std::string bytes{std::istreambuf_iterator<char>(file), {}};
    if (file.bad()) return std::unexpected(std::format("cannot read staged rule input '{}'", read.path));
    inputs.emplace(read.path, std::move(bytes));
  }
  return inputs;
}

Bytes borrow(std::string_view bytes) { return {bytes.data(), bytes.size()}; }

struct Invocation {
  const RuleWorkerRequest& request;
  const RuleInputs& inputs;
  std::string emitted;
  std::string diagnostic;
  std::optional<std::uint32_t> support = std::nullopt;
  bool invalid_read = false;
  bool invalid_emit = false;
  bool invalid_diagnose = false;
  bool invalid_support = false;
};

int attribute(void* context, const char* name, Bytes* result) noexcept {
  if (result) *result = {};
  if (!context || !name || !*name || !result) return -1;
  const auto& attrs = static_cast<const Invocation*>(context)->request.attributes;
  const auto found = std::ranges::find_if(attrs, [&](const auto& attr) { return attr.name == name; });
  if (found == attrs.end()) return 0;
  *result = borrow(found->value);
  return 1;
}

int read(void* context, const char* path, Bytes* result) noexcept {
  if (result) *result = {};
  if (!context) return -1;
  auto& invocation = *static_cast<Invocation*>(context);
  if (path && *path && result) {
    const auto found = invocation.inputs.find(path);
    if (found != invocation.inputs.end()) {
      if (!found->second) return 0;
      *result = borrow(*found->second);
      return 1;
    }
  }
  invocation.invalid_read = true;
  return -1;
}

int copy_once(std::string& output, bool& invalid, const char* text, std::size_t size,
              std::size_t limit) noexcept {
  if (invalid || !output.empty() || !text || size == 0 || size > limit) {
    invalid = true;
    return -1;
  }
  try {
    output.assign(text, size);
  } catch (...) {
    invalid = true;
    return -1;
  }
  return 0;
}

int emit(void* context, const char* definition, std::size_t size) noexcept {
  if (!context) return -1;
  auto& invocation = *static_cast<Invocation*>(context);
  if (invocation.request.witness) {
    invocation.invalid_emit = true;
    return -1;
  }
  return copy_once(invocation.emitted, invocation.invalid_emit, definition, size, max_definition_size);
}

int export_support(void* context, const char* name, std::uint32_t support) noexcept {
  if (!context) return -1;
  auto& invocation = *static_cast<Invocation*>(context);
  const auto& witness = invocation.request.witness;
  if (invocation.invalid_support || invocation.support || !witness ||
      !name || witness->name != name || support > max_capability_support) {
    invocation.invalid_support = true;
    return -1;
  }
  invocation.support = support;
  return 0;
}

int diagnose(void* context, const char* text, std::size_t size) noexcept {
  if (!context) return -1;
  auto& invocation = *static_cast<Invocation*>(context);
  return copy_once(invocation.diagnostic, invocation.invalid_diagnose, text, size, max_diagnostic_size);
}

}  // namespace

std::string_view rule_sdk_header() { return {sdk_header, sizeof sdk_header - 1}; }

int run_rule_worker(std::span<const std::string_view> args) {
  if (args.size() != 3 || !std::ranges::all_of(args, valid_key))
    return fail("expected a library path, a request path and an output path");

  auto request = load_request(std::string(args[1]));
  if (!request) return fail(request.error());
  auto inputs = load_reads(*request);
  if (!inputs) return fail(inputs.error());

  std::string library_path(args[0]);
  if (!library_path.contains('/')) library_path.insert(0, "./");
  const Library library{::dlopen(library_path.c_str(), RTLD_NOW | RTLD_LOCAL)};
  if (!library.handle) {
    const char* detail = ::dlerror();
    return fail(detail ? detail : "cannot load rule library");
  }
  const auto& report = request->witness;
  const char* entry = report ? "ycxx_rule_classify_v1" : "ycxx_rule_plan_v1";
  const auto invoke = reinterpret_cast<int (*)(const Host*)>(::dlsym(library.handle, entry));
  if (!invoke) return fail(std::format("rule library has no {} entry point", entry));

  std::vector<Capability> capabilities;
  if (request->capabilities) {
    capabilities.reserve(request->capabilities->size());
    for (const auto& capability : *request->capabilities)
      capabilities.push_back({capability.name.c_str(), capability.support});
  }
  Witness witness{};
  if (report)
    witness = {report->name.c_str(), report->exit_status, borrow(report->out), borrow(report->err)};
  Invocation invocation{*request, *inputs, {}, {}};
  const Host host{abi_version, sizeof(Host), request->rule.c_str(), request->target.c_str(),
                  borrow(request->definition), &invocation, attribute, read, emit,
                  capabilities.empty() ? nullptr : capabilities.data(), capabilities.size(), diagnose,
                  report ? &witness : nullptr, report ? export_support : nullptr};
  const int status = invoke(&host);
  if (invocation.invalid_read) return fail("rule attempted an invalid or undeclared read");
  if (report && invocation.invalid_emit) return fail("rule classifier attempted to emit a definition");
  if (invocation.invalid_emit) return fail("rule attempted an invalid, repeated or oversized definition");
  if (invocation.invalid_diagnose) return fail("rule supplied an invalid, repeated or oversized diagnostic");
  if (invocation.invalid_support) return fail("rule classifier attempted an invalid or repeated support export");
  if (status != 0) {
    auto message = std::format("rule '{}' failed with status {}", request->rule, status);
    if (!invocation.diagnostic.empty()) message.append(": ").append(invocation.diagnostic);
    return fail(message);
  }
  if (report && !invocation.support) return fail("rule classifier did not export a support state");
  if (!report && invocation.emitted.empty()) return fail("rule did not emit a definition");

  const std::string result = report ? lsp::dump(lsp::to_json(RuleCapabilityValue{report->name, *invocation.support}))
                                    : std::move(invocation.emitted);
  std::ofstream output(std::string(args[2]), std::ios::binary | std::ios::noreplace);
  if (!output) return fail("cannot open rule output");
  output.write(result.data(), static_cast<std::streamsize>(result.size()));
  output.close();
  if (!output) return fail("cannot write rule output");
  return 0;
}

int run_shared_probe(std::span<const std::string_view> args) {
  if (args.size() != 1 || !valid_key(args.front())) return fail("expected a staged probe library path");
  const std::string path(args.front());
  const Library library{::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL)};
  if (!library.handle) {
    const char* detail = ::dlerror();
    std::println(std::cerr, "ycxx: cannot load probe library: {}", detail ? detail : "unknown loader error");
    return 126;
  }
  const auto plan = reinterpret_cast<int (*)(const Host*)>(::dlsym(library.handle, "ycxx_rule_plan_v1"));
  if (!plan) return fail("probe library has no ycxx_rule_plan_v1 entry point");
  if (plan(nullptr) != 42) return fail("probe library returned the wrong witness");
  return 0;
}

int run_executable_probe(std::span<const std::string_view> args) {
  if (args.size() != 1 || !valid_key(args.front())) return fail("expected a staged probe executable path");
  std::string path(args.front());
  char* argv[]{path.data(), nullptr};
  ::execv(path.c_str(), argv);
  std::println(std::cerr, "ycxx: cannot run probe executable: {}", ::strerror(errno));
  return 126;
}

}  // namespace ycxx::build
