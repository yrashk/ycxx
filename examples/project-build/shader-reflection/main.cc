#include "bundle.hh"
#include "scene_bindings.hh"

namespace {

bool parse_unsigned(const char* input, unsigned& result) {
  result = 0;
  if (*input == '\0') return false;
  for (const char* digit = input; *digit != '\0'; ++digit) {
    if (*digit < '0' || *digit > '9') return false;
    const unsigned value = static_cast<unsigned>(*digit - '0');
    if (result > (0xffffffffu - value) / 10u) return false;
    result = result * 10u + value;
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

bool valid_id(const shaders::Module& module, unsigned id) {
  return id != 0 && id < module.word(3);
}

unsigned descriptor_type(const shaders::Module& module, const char* name, unsigned storage,
                         unsigned set, unsigned binding) {
  constexpr unsigned op_name = 5;
  constexpr unsigned op_variable = 59;
  constexpr unsigned op_decorate = 71;
  constexpr unsigned binding_decoration = 33;
  constexpr unsigned descriptor_set_decoration = 34;

  unsigned variable = 0;
  for (unsigned index = shaders::header_words; index < module.words;) {
    const unsigned instruction = module.word(index);
    const unsigned length = instruction >> 16;
    if ((instruction & 0xffffu) == op_name && length >= 3 &&
        name_matches(module, index + 2, length - 2, name)) {
      const unsigned id = module.word(index + 1);
      if (variable != 0 || !valid_id(module, id)) return 0;
      variable = id;
    }
    index += length;
  }
  if (variable == 0) return 0;

  unsigned type = 0;
  bool set_matches = false;
  bool binding_matches = false;
  for (unsigned index = shaders::header_words; index < module.words;) {
    const unsigned instruction = module.word(index);
    const unsigned length = instruction >> 16;
    const unsigned opcode = instruction & 0xffffu;
    if (opcode == op_variable && length >= 4 && module.word(index + 2) == variable) {
      if (type != 0 || module.word(index + 3) != storage) return 0;
      type = module.word(index + 1);
      if (!valid_id(module, type)) return 0;
    } else if (opcode == op_decorate && length == 4 && module.word(index + 1) == variable) {
      const unsigned decoration = module.word(index + 2);
      const unsigned value = module.word(index + 3);
      if (decoration == descriptor_set_decoration) {
        if (set_matches || value != set) return 0;
        set_matches = true;
      } else if (decoration == binding_decoration) {
        if (binding_matches || value != binding) return 0;
        binding_matches = true;
      }
    }
    index += length;
  }
  return set_matches && binding_matches ? type : 0;
}

unsigned find_instruction(const shaders::Module& module, unsigned opcode, unsigned words,
                          unsigned id, unsigned id_operand) {
  if (!valid_id(module, id)) return 0;
  unsigned match = 0;
  for (unsigned index = shaders::header_words; index < module.words;) {
    const unsigned instruction = module.word(index);
    const unsigned length = instruction >> 16;
    if ((instruction & 0xffffu) == opcode && length > id_operand &&
        module.word(index + id_operand) == id) {
      if (match != 0 || length != words) return 0;
      match = index;
    }
    index += length;
  }
  return match;
}

constexpr unsigned uniform_constant_storage = 0;

bool has_sampled_image_array(const shaders::Module& module, unsigned type, unsigned count) {
  constexpr unsigned op_type_int = 21;
  constexpr unsigned op_type_sampled_image = 27;
  constexpr unsigned op_type_array = 28;
  constexpr unsigned op_type_pointer = 32;
  constexpr unsigned op_constant = 43;

  const unsigned pointer = find_instruction(module, op_type_pointer, 4, type, 1);
  if (pointer == 0 || module.word(pointer + 2) != uniform_constant_storage) return false;
  const unsigned array = find_instruction(module, op_type_array, 4, module.word(pointer + 3), 1);
  if (array == 0 || find_instruction(module, op_type_sampled_image, 3, module.word(array + 2), 1) == 0)
    return false;
  const unsigned length = find_instruction(module, op_constant, 4, module.word(array + 3), 2);
  if (length == 0 || module.word(length + 3) != count) return false;
  const unsigned integer = find_instruction(module, op_type_int, 4, module.word(length + 1), 1);
  if (integer == 0 || module.word(integer + 2) != 32) return false;
  const unsigned signedness = module.word(integer + 3);
  return signedness == 0 || (signedness == 1 && count <= 0x7fffffffu);
}

}  // namespace

int main(int argc, char** argv) {
  unsigned expected_binding = 0;
  unsigned expected_count = 2;
  if ((argc != 3 && argc != 4) || !parse_unsigned(argv[1], expected_binding) ||
      (argc == 4 && (!parse_unsigned(argv[3], expected_count) || expected_count == 0))) return 1;
  const char* entry = shaders::find_module(argv[2], "fragment");
  if (!entry) return 2;
  shaders::Module fragment{entry};
  if (!fragment.valid()) return 3;

  static_assert(scene::descriptor_Material_kind == scene::descriptor_kind::uniform_buffer);
  static_assert(scene::descriptor_Material_element_count == 1);
  static_assert(scene::descriptor_scene_textures_kind == scene::descriptor_kind::combined_sampler);
  if (expected_binding != scene::descriptor_Material_binding ||
      expected_count != scene::descriptor_scene_textures_element_count) return 4;

  constexpr unsigned uniform_storage = 2;
  const unsigned material = descriptor_type(fragment, "material", uniform_storage,
                                            scene::descriptor_Material_set, scene::descriptor_Material_binding);
  const unsigned textures = descriptor_type(fragment, "scene_textures", uniform_constant_storage,
                                            scene::descriptor_scene_textures_set,
                                            scene::descriptor_scene_textures_binding);
  return material != 0 && has_sampled_image_array(fragment, textures, expected_count) ? 0 : 4;
}
