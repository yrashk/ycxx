// Run from a terminal for the live view, or use --json to retain the paired samples.
import std;
import ycxx.bench;

namespace {

using namespace ycxx::bench;

std::uint32_t mix(std::uint32_t state) {
  return state * 1'664'525u + 1'013'904'223u;
}

struct DemoCase {
  std::string_view name;
  Properties properties;
  Metadata metadata;
};

struct Workloads {
  std::span<const DemoCase> cases;
  std::span<const std::size_t> selected;
  std::array<std::uint32_t, 3> state{1, 1, 1};

  const Properties& benchmark_properties(std::size_t index) const { return cases[selected[index]].properties; }
  const Metadata& benchmark_metadata(std::size_t index) const { return cases[selected[index]].metadata; }

  std::uint32_t operator()(std::size_t index, Iterations n) {
    const auto which = selected[index];
    auto& value = state[which];
    // The dependency chain is intentional. Every iteration's result is observable.
    switch (which) {
      case 0:
        for (Iterations i = 0; i != n; ++i) do_not_optimize(value = mix(value));
        break;
      case 1:
        for (Iterations i = 0; i != n; ++i) {
          do_not_optimize(value = mix(value));
          do_not_optimize(value = mix(value));
        }
        break;
      default:
        for (Iterations i = 0; i != n; ++i) do_not_optimize(value = std::rotl(mix(value), 7));
        break;
    }
    return value;
  }
};

}  // namespace

int main(int argc, char** argv) {
  using namespace ycxx::bench;
  enum class Format { console, ndjson, live, csv };
  auto format = Format::console;
  const bool terminal = stdout_is_terminal();
  const char* term = std::getenv("TERM");
  auto style = terminal && std::getenv("NO_COLOR") == nullptr
    && (term == nullptr || std::string_view(term) != "dumb") ? Style::ansi : Style::plain;
  bool shootout = false;
  bool list = false;
  bool details = false;
  Selection selection;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--json") format = Format::ndjson;
    else if (arg == "--json-live") format = Format::live;
    else if (arg == "--details") details = true;
    else if (arg == "--csv") format = Format::csv;
    else if (arg == "--color") style = Style::ansi;
    else if (arg == "--no-color") style = Style::plain;
    else if (arg == "--shootout") shootout = true;
    else if (arg == "--list") list = true;
    else if (arg == "--group" && i + 1 < argc) selection.group = argv[++i];
    else if (arg == "--tag" && i + 1 < argc) selection.tags.push_back(argv[++i]);
    else if (arg == "--band" && i + 1 < argc) {
      const std::string_view value = argv[++i];
      for (const auto band : {Band::smoke, Band::standard, Band::deep}) {
        if (value == band_name(band)) selection.band = band;
      }
      if (!selection.band || value != band_name(*selection.band)) {
        std::println(std::cerr, "unknown sampling band {}", value);
        return 2;
      }
    } else if (arg == "--help") {
      std::println("usage: ycxx-bench-demo [--color | --no-color] [--json | --json-live | --csv] [--shootout] [--details] "
                   "[--list] [--group GROUP] [--tag TAG] [--band smoke|standard|deep]");
      return 0;
    } else {
      std::println(std::cerr, "unknown or incomplete argument {}", arg);
      return 2;
    }
  }

  const DemoCase cases[] = {
    {"one mix", {"integer", {"baseline", "lcg"}, Band::standard},
      {{"dependency", std::string("u32")}, {"mixes", std::uint64_t{1}}}},
    {"two chained mixes", {"integer", {"lcg"}, Band::standard},
      {{"dependency", std::string("u32")}, {"mixes", std::uint64_t{2}}}},
    {"mix with bit rotation", {"bitwise", {"rotate", "lcg"}, Band::smoke},
      {{"dependency", std::string("u32")}, {"rotate_bits", std::uint64_t{7}}}},
  };
  const bool filtering = !selection.group.empty() || !selection.tags.empty() || selection.band.has_value();
  const auto count = shootout || list || filtering ? std::size(cases) : std::size_t{2};
  std::vector<std::size_t> selected;
  for (std::size_t i = 0; i != count; ++i) {
    if (matches(cases[i].properties, selection)) selected.push_back(i);
  }
  if (selected.empty()) {
    std::println(std::cerr, "no benchmark cases match the selection");
    return 2;
  }
  if (list) {
    std::println("{:<24} {:<11} {:<9} {}", "case", "group", "band", "tags");
    for (const auto index : selected) {
      const auto& [name, properties, metadata] = cases[index];
      std::print("{:<24} {:<11} {:<9}", name, properties.group, band_name(properties.band));
      for (const auto& tag : properties.tags) std::print(" {}", tag);
      std::println();
    }
    return 0;
  }
  if (format == Format::csv && selected.size() != 1) {
    if (filtering || shootout) {
      std::println(std::cerr, "CSV needs one case. Use --json to retain a comparison.");
      return 2;
    }
    selected.resize(1);
  }

  auto band = Band::inherit;
  std::vector<std::string_view> names;
  for (const auto index : selected) {
    band = std::max(band, cases[index].properties.band);
    names.push_back(cases[index].name);
  }
  auto config = configuration_for(band);
  config.process_telemetry = true;
  TerminalProgress progress(std::cout, selected.size(), config.profile);
  if (terminal && style == Style::ansi && format == Format::console)
    config.progress = std::ref(progress);

  const bool json = format == Format::ndjson || format == Format::live;
  if (format == Format::live) {
    write_live_start(std::cout, names);
    config.progress = [previous = std::optional<Progress>{}](Progress state) mutable {
      if (previous && previous->phase == state.phase && previous->completed == state.completed) return;
      write_live_progress(std::cout, state);
      previous = state;
    };
    config.sample_observer = [&](std::size_t which, std::size_t round, const Sample& sample) {
      write_live_sample(std::cout, names[which], round, sample);
    };
  }
  const auto print_details = [&](std::span<const Measurement> cases) {
    if (!details) return;
    for (std::size_t i = 0; i != cases.size(); ++i) {
      std::println();
      write_console(std::cout, names[i], cases[i], style);
    }
  };

  Workloads work{cases, selected};
  auto one_case = [&](std::size_t position) {
    return with_properties([&, position](Iterations n) { return work(position, n); },
      work.benchmark_properties(position), work.benchmark_metadata(position));
  };
  if (selected.size() == 1) {
    const auto result = measure_batched(one_case(0), config);
    progress.clear();
    if (json) write_ndjson(std::cout, names.front(), result);
    else if (format == Format::csv) write_csv(std::cout, names.front(), result);
    else write_console(std::cout, names.front(), result, style);
  } else if (selected.size() == 2 && !shootout) {
    const auto result = compare_batched(one_case(0), one_case(1), config);
    progress.clear();
    if (json) write_ndjson(std::cout, names[0], names[1], result);
    else {
      write_console(std::cout, names[0], names[1], result, style);
      if (details) {
        std::println();
        write_console(std::cout, names[0], result.reference, style);
        std::println();
        write_console(std::cout, names[1], result.contender, style);
      }
    }
  } else {
    const auto result = compare_many_batched(selected.size(), work, config);
    progress.clear();
    if (json) write_ndjson(std::cout, names, result);
    else {
      write_console(std::cout, names, result, style);
      print_details(result.cases);
    }
  }
  if (json) std::cout.flush();
}
