// Compile source supplied by the host and inspect the resulting object in memory.
import std;
import ycxx.ast;
import ycxx.obj;
import ycxx.pipeline;
import ycxx.pp;

namespace pipeline = ycxx::pipeline;

int main(int argc, char** argv) {
  if (argc != 2)
    return 1;
  pipeline::Session session;
  ycxx::pp::Config config;
  config.target = &ycxx::ast::x86_64_linux;
  config.angle_dirs.push_back((std::filesystem::path(argv[1]) / "linux/include").native());
  const pipeline::TuId unit{0};

  const std::string source = "#include <stddef.h>\nextern \"C\" size_t answer() { return 42; }";
  session.set_source(unit, "embedded.cpp", source,
                     "embedded.o", {}, config);
  const auto& result = session.get<pipeline::Tu>(pipeline::Tu::Key{unit});
  if (!result.object)
    return 1;
  const auto& symbols = result.object->symbols;
  return std::ranges::any_of(symbols, [](const ycxx::obj::symbol& symbol) {
    return symbol.defined() && symbol.name_view() == "answer";
  }) ? 0 : 2;
}
