// The rule library and its callbacks live in a short-lived process (#1257).
module;
#include <dlfcn.h>
export module ycxx.build.rule_worker;

import std;
import ycxx.build.rule_protocol;
import ycxx.lsp;

export namespace ycxx::build {

// The SDK staged as an input when compiling a rule package.
std::string_view rule_sdk_header();

// Arguments name the staged library, JSON request and output definition, respectively.
int run_rule_worker(std::span<const std::string_view> args);

}  // namespace ycxx::build

namespace ycxx::build {
namespace {

constexpr char sdk_header[] = {
#embed "../../resources/rules/rule_api.h"
    , 0};
constexpr std::size_t max_request_size = 8 * 1024 * 1024;
constexpr std::size_t max_definition_size = 1024 * 1024;
constexpr std::uint32_t abi_version = 1;

// The C ABI in the staged SDK header. No C++ objects or ownership cross this boundary.
struct Bytes {
  const char* data;
  std::size_t size;
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

  std::set<std::string_view> keys;
  for (const auto& attribute : request->attributes)
    if (!valid_key(attribute.name) || !keys.insert(attribute.name).second)
      return std::unexpected("invalid or duplicate rule attribute");
  keys.clear();
  for (const auto& read : request->reads)
    if (!valid_key(read.path) || !keys.insert(read.path).second)
      return std::unexpected("invalid or duplicate rule read");
  return request;
}

Bytes borrow(std::string_view bytes) { return {bytes.data(), bytes.size()}; }

struct Invocation {
  const RuleWorkerRequest& request;
  std::string emitted;
  bool invalid_read = false;
  bool invalid_emit = false;
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
    const auto& reads = invocation.request.reads;
    const auto found = std::ranges::find_if(reads, [&](const auto& item) { return item.path == path; });
    if (found != reads.end()) {
      if (!found->staged) return 0;
      *result = borrow(*found->staged);
      return 1;
    }
  }
  invocation.invalid_read = true;
  return -1;
}

int emit(void* context, const char* definition, std::size_t size) noexcept {
  if (!context) return -1;
  auto& invocation = *static_cast<Invocation*>(context);
  if (invocation.invalid_emit || !invocation.emitted.empty() || !definition || size == 0 ||
      size > max_definition_size) {
    invocation.invalid_emit = true;
    return -1;
  }
  try {
    invocation.emitted.assign(definition, size);
  } catch (...) {
    invocation.invalid_emit = true;
    return -1;
  }
  return 0;
}

}  // namespace

std::string_view rule_sdk_header() { return {sdk_header, sizeof sdk_header - 1}; }

int run_rule_worker(std::span<const std::string_view> args) {
  if (args.size() != 3 || !std::ranges::all_of(args, valid_key))
    return fail("expected a library path, a request path and an output path");

  auto request = load_request(std::string(args[1]));
  if (!request) return fail(request.error());

  std::string library_path(args[0]);
  if (!library_path.contains('/')) library_path.insert(0, "./");
  const Library library{::dlopen(library_path.c_str(), RTLD_NOW | RTLD_LOCAL)};
  if (!library.handle) {
    const char* detail = ::dlerror();
    return fail(detail ? detail : "cannot load rule library");
  }
  const auto plan = reinterpret_cast<int (*)(const Host*)>(::dlsym(library.handle, "ycxx_rule_plan_v1"));
  if (!plan) return fail("rule library has no ycxx_rule_plan_v1 entry point");

  Invocation invocation{*request, {}};
  const Host host{abi_version, sizeof(Host), request->rule.c_str(), request->target.c_str(),
                  borrow(request->definition), &invocation, attribute, read, emit};
  const int status = plan(&host);
  if (invocation.invalid_read) return fail("rule attempted an invalid or undeclared read");
  if (invocation.invalid_emit) return fail("rule attempted an invalid, repeated or oversized definition");
  if (status != 0) return fail(std::format("rule '{}' failed with status {}", request->rule, status));
  if (invocation.emitted.empty()) return fail("rule did not emit a definition");

  std::ofstream output(std::string(args[2]), std::ios::binary | std::ios::noreplace);
  if (!output) return fail("cannot open rule output");
  output.write(invocation.emitted.data(), static_cast<std::streamsize>(invocation.emitted.size()));
  output.close();
  if (!output) return fail("cannot write rule output");
  return 0;
}

}  // namespace ycxx::build
