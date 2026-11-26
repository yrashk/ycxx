import std;
import ycxx.bench;
import ycxx.cm;

namespace {

ycxx::cm::Model covering_model(std::size_t size) {
  using namespace ycxx::cm;
  Model model;
  std::vector<Term> objective;
  BoolVar previous_fused;
  for (std::size_t i = 0; i < size; ++i) {
    const auto direct = model.bool_var();
    const auto fused = model.bool_var();
    const Term cover[]{{1, direct.integer}, {1, fused.integer}};
    model.linear(cover, Rel::eq, 1);
    if (previous_fused.integer != IntVar::none) {
      const Term overlap[]{{1, previous_fused.integer}, {1, fused.integer}};
      model.linear(overlap, Rel::le, 1);
    }
    previous_fused = fused;
    objective.push_back({2, direct.integer});
    objective.push_back({3, fused.integer});
  }
  model.minimize(objective);
  return model;
}

}  // namespace

int main(int argc, char** argv) {
  const bool ndjson = argc == 2 && std::string_view(argv[1]) == "--ndjson";
  if (argc != 1 && !ndjson) {
    std::println(std::cerr, "usage: ycxx-cm-export-bench [--ndjson]");
    return 2;
  }

  for (const auto size : {64uz, 256uz, 1024uz}) {
    const auto model = covering_model(size);
    const auto name = std::format("FlatZinc export / {} covering decisions", size);
    const auto expected = ycxx::cm::write_flatzinc(model);
    if (!expected) return 1;

    using namespace ycxx::bench;
    const auto sample = measure(with_properties([&] {
      const auto exported = ycxx::cm::write_flatzinc(model);
      // Exports are also checked by unit.cm and the Chuffed integration test.
      return exported->size();
    }, Properties{"optimizer", {"flatzinc", "serialization"}, Band::standard},
       Metadata{{"decisions", std::uint64_t(size)}, {"variables", std::uint64_t(model.domains().size())},
                {"linear_rows", std::uint64_t(model.rows().size())},
                {"output_bytes", std::uint64_t(expected->size())}}));
    if (ndjson) ycxx::bench::write_ndjson(std::cout, name, sample);
    else ycxx::bench::write_console(std::cout, name, sample);
  }
}
