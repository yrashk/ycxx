#include "bundle.hh"
#include "scene_bindings.hh"

namespace {

bool parse_binding(const char* input, unsigned& binding) {
  if (*input == '\0') return false;
  for (const char* digit = input; *digit != '\0'; ++digit) {
    if (*digit < '0' || *digit > '9') return false;
    const unsigned value = static_cast<unsigned>(*digit - '0');
    if (binding > (0xffffffffu - value) / 10u) return false;
    binding = binding * 10u + value;
  }
  return true;
}

bool name_matches(const shaders::Module& module, unsigned start, unsigned words, const char* name) {
  for (unsigned index = 0; index < words; ++index) {
    unsigned chars = module.word(start + index);
    for (unsigned byte = 0; byte < 4; ++byte) {
      const auto c = static_cast<char>(chars & 0xffu);
      if (c != *name) return false;
      if (c == '\0') return true;
      ++name;
      chars >>= 8;
    }
  }
  return false;
}

bool has_descriptor(const shaders::Module& module, unsigned set, unsigned binding) {
  constexpr unsigned op_name = 5;
  constexpr unsigned op_variable = 59;
  constexpr unsigned op_decorate = 71;
  constexpr unsigned uniform_storage_class = 2;
  constexpr unsigned binding_decoration = 33;
  constexpr unsigned descriptor_set_decoration = 34;

  unsigned variable = 0;
  for (unsigned index = shaders::header_words; index < module.words;) {
    const unsigned instruction = module.word(index);
    const unsigned length = instruction >> 16;
    if ((instruction & 0xffffu) == op_name && length >= 3 &&
        name_matches(module, index + 2, length - 2, "material")) {
      const unsigned id = module.word(index + 1);
      if (variable != 0 || id == 0 || id >= module.word(3)) return false;
      variable = id;
    }
    index += length;
  }
  if (variable == 0) return false;

  bool uniform = false;
  bool set_matches = false;
  bool binding_matches = false;
  for (unsigned index = shaders::header_words; index < module.words;) {
    const unsigned instruction = module.word(index);
    const unsigned length = instruction >> 16;
    const unsigned opcode = instruction & 0xffffu;
    if (opcode == op_variable && length >= 4 && module.word(index + 2) == variable) {
      uniform = module.word(index + 3) == uniform_storage_class;
    } else if (opcode == op_decorate && length == 4 && module.word(index + 1) == variable) {
      const unsigned decoration = module.word(index + 2);
      const unsigned value = module.word(index + 3);
      if (decoration == descriptor_set_decoration) {
        if (set_matches || value != set) return false;
        set_matches = true;
      } else if (decoration == binding_decoration) {
        if (binding_matches || value != binding) return false;
        binding_matches = true;
      }
    }
    index += length;
  }
  return uniform && set_matches && binding_matches;
}

}  // namespace

int main(int argc, char** argv) {
  unsigned expected = 0;
  if (argc != 3 || !parse_binding(argv[1], expected)) return 1;
  const char* entry = shaders::find_module(argv[2], "fragment");
  if (!entry) return 2;
  shaders::Module fragment{entry};
  if (!fragment.valid()) return 3;

  constexpr unsigned set = scene::descriptor_Material_set;
  constexpr unsigned binding = scene::descriptor_Material_binding;
  return binding == expected && has_descriptor(fragment, set, binding) ? 0 : 4;
}
