// Baseline toolchain questions. Each row describes a compiler invocation and, where it
// matters, includes a source witness. The probe runner owns staging and result validation.
export module ycxx.build.toolchain_probe;

import std;

export namespace ycxx::build {

enum class ToolchainProbePhase : std::uint8_t {
  driver_query,
  syntax,
  header,
  object,
  executable_link,
  shared_link,
};

struct ToolchainProbe {
  std::string_view id;
  std::string_view name;
  ToolchainProbePhase phase;
  std::string_view source;
  std::span<const std::string_view> args;
  std::string_view target;  // empty for the compiler's default target
  std::string_view output;  // empty when the invocation produces no artifact
  bool native_leaf_link = false;
};

std::span<const ToolchainProbe> baseline_toolchain_probes() noexcept;

}  // namespace ycxx::build

namespace ycxx::build {
namespace {

using namespace std::literals;
using enum ToolchainProbePhase;

// The dialect probes measure acceptance of the named mode. The separate feature
// probes exercise semantics, so an accepted command-line option is insufficient.
constexpr std::string_view target_query[] = {"-dumpmachine"sv};
constexpr std::string_view syntax20[] = {"-std=c++20"sv, "-fsyntax-only"sv};
constexpr std::string_view syntax23[] = {"-std=c++23"sv, "-fsyntax-only"sv};
constexpr std::string_view syntax26[] = {"-std=c++26"sv, "-fsyntax-only"sv};
constexpr std::string_view reflection[] = {"-std=c++26"sv, "-freflection"sv, "-fsyntax-only"sv};
constexpr std::string_view object_args[] = {"-std=c++20"sv, "-ffreestanding"sv, "-c"sv};
constexpr std::string_view hosted_args[] = {"-std=c++20"sv};
constexpr std::string_view freestanding_args[] = {"-std=c++20"sv, "-ffreestanding"sv, "-nostdlib"sv};
constexpr std::string_view shared_args[] = {"-std=c++20"sv, "-fPIC"sv, "-shared"sv};

constexpr auto declaration = "int ycxx_probe();\n"sv;
constexpr auto leaf_function = R"cpp(
extern "C" int ycxx_probe(int value) { return value + 1; }
)cpp"sv;
constexpr auto main_function = "int main() { return 0; }\n"sv;
// -ffreestanding selects the language environment and can still link a hosted runtime.
// This probe also asks for a link without the normal runtime. It supplies an ELF
// entry and a main for drivers that link through the platform's main convention.
constexpr auto freestanding_main = R"cpp(
int main() { return 0; }
extern "C" void _start() {}
)cpp"sv;
constexpr auto plugin_entry = R"cpp(
struct ycxx_rule_host_v1;
extern "C" int ycxx_rule_plan_v1(const ycxx_rule_host_v1*) { return 0; }
)cpp"sv;

constexpr ToolchainProbe probes[] = {
    {"driver.default-target", "Default target", driver_query, {}, target_query, {}, {}},
    {"dialect.cxx20", "C++20 mode", syntax, declaration, syntax20, {}, {}},
    {"dialect.cxx23", "C++23 mode", syntax, declaration, syntax23, {}, {}},
    {"dialect.cxx26", "C++26 mode", syntax, declaration, syntax26, {}, {}},

    {"header.array", "<array>", header, "#include <array>\n", syntax20, {}, {}},
    {"header.chrono", "<chrono>", header, "#include <chrono>\n", syntax20, {}, {}},
    {"header.concepts", "<concepts>", header, "#include <concepts>\n", syntax20, {}, {}},
    {"header.coroutine", "<coroutine>", header, "#include <coroutine>\n", syntax20, {}, {}},
    {"header.cstddef", "<cstddef>", header, "#include <cstddef>\n", syntax20, {}, {}},
    {"header.cstdint", "<cstdint>", header, "#include <cstdint>\n", syntax20, {}, {}},
    {"header.execution", "<execution>", header, "#include <execution>\n", syntax20, {}, {}},
    {"header.expected", "<expected>", header, "#include <expected>\n", syntax23, {}, {}},
    {"header.filesystem", "<filesystem>", header, "#include <filesystem>\n", syntax20, {}, {}},
    {"header.format", "<format>", header, "#include <format>\n", syntax20, {}, {}},
    {"header.iostream", "<iostream>", header, "#include <iostream>\n", syntax20, {}, {}},
    {"header.memory", "<memory>", header, "#include <memory>\n", syntax20, {}, {}},
    {"header.optional", "<optional>", header, "#include <optional>\n", syntax20, {}, {}},
    {"header.ranges", "<ranges>", header, "#include <ranges>\n", syntax20, {}, {}},
    {"header.span", "<span>", header, "#include <span>\n", syntax20, {}, {}},
    {"header.string", "<string>", header, "#include <string>\n", syntax20, {}, {}},
    {"header.string_view", "<string_view>", header, "#include <string_view>\n", syntax20, {}, {}},
    {"header.type_traits", "<type_traits>", header, "#include <type_traits>\n", syntax20, {}, {}},
    {"header.utility", "<utility>", header, "#include <utility>\n", syntax20, {}, {}},
    {"header.vector", "<vector>", header, "#include <vector>\n", syntax20, {}, {}},

    {"feature.concepts", "Concept constraints", syntax, R"cpp(
template<class T> concept OneByte = sizeof(T) == 1;
template<OneByte T> constexpr int witness(T) { return 5; }
static_assert(witness('a') == 5);
static_assert(!OneByte<char[2]>);
)cpp", syntax20, {}, {}},
    {"feature.if-consteval", "if consteval", syntax, R"cpp(
constexpr int witness() {
    if consteval { return 5; } else { return 8; }
}
static_assert(witness() == 5);
)cpp", syntax23, {}, {}},
    {"feature.explicit-object", "Explicit object parameters", syntax, R"cpp(
struct Value {
    int n;
    constexpr int get(this const Value& self) { return self.n; }
};
static_assert(Value{5}.get() == 5);
)cpp", syntax23, {}, {}},
    {"feature.pack-indexing", "Pack indexing", syntax, R"cpp(
template<class T> constexpr bool is_char = false;
template<> constexpr bool is_char<char> = true;
template<class... Ts> using second = Ts...[1];
static_assert(is_char<second<int, char>>);
)cpp", syntax26, {}, {}},
    {"feature.reflection", "Static reflection", syntax, "#include <meta>\n" R"cpp(
static_assert(std::meta::is_type(^^int));
static_assert(^^int != ^^long);
)cpp", reflection, {}, {}},
    {"feature.std-span", "std::span indexing", syntax, "#include <span>\n" R"cpp(
constexpr int values[] = {3, 5};
constexpr std::span<const int, 2> view(values);
static_assert(view.size() == 2 && view[1] == 5);
)cpp", syntax20, {}, {}},
    {"feature.std-expected", "std::expected value and error", syntax, "#include <expected>\n" R"cpp(
constexpr std::expected<int, int> success(5);
constexpr std::expected<int, int> failure(std::unexpected(7));
static_assert(success.has_value() && *success == 5);
static_assert(!failure.has_value() && failure.error() == 7);
)cpp", syntax23, {}, {}},
    {"feature.std-format", "std::format with a checked format string", syntax, "#include <format>\n" R"cpp(
auto witness() { return std::format("{:04}", 42); }
)cpp", syntax20, {}, {}},

    {"object.default", "Default target object", object, leaf_function, object_args, {}, "probe.o"},
    {"object.x86_64-linux", "x86-64 Linux object", object, leaf_function, object_args, "x86_64-pc-linux-gnu", "probe.o"},
    {"object.aarch64-linux", "AArch64 Linux object", object, leaf_function, object_args, "aarch64-unknown-linux-gnu", "probe.o"},
    {"object.arm64-darwin", "Arm64 Darwin object", object, leaf_function, object_args, "arm64-apple-darwin", "probe.o"},
    {"object.riscv64-linux", "RISC-V Linux object", object, leaf_function, object_args, "riscv64-unknown-linux-gnu", "probe.o"},
    {"link.hosted", "Hosted executable", executable_link, main_function, hosted_args, {}, "probe"},
    {"link.freestanding", "Freestanding executable without the default runtime", executable_link,
     freestanding_main, freestanding_args, {}, "probe", true},
    {"link.host-shared", "Host shared library with the rule entry point", shared_link,
     plugin_entry, shared_args, {}, "probe.so", true},
};

}  // namespace

std::span<const ToolchainProbe> baseline_toolchain_probes() noexcept { return probes; }

}  // namespace ycxx::build
