#include "ycxx_rule_api.h"

namespace {

using Text = ycxx_rule_bytes_v1;

constexpr size_t max_shaders = 16;
constexpr size_t max_name = 63;

// The worker calls the planner once and copies the emitted definition.
char definition_bytes[64 * 1024];

Text text(const char* value) {
  size_t size = 0;
  while (value[size] != '\0') ++size;
  return {value, size};
}

int compare(Text left, Text right) {
  for (size_t i = 0; i < left.size && i < right.size; ++i) {
    if (left.data[i] != right.data[i]) return left.data[i] < right.data[i] ? -1 : 1;
  }
  if (left.size == right.size) return 0;
  return left.size < right.size ? -1 : 1;
}

bool valid_name(Text name) {
  if (name.size == 0 || name.size > max_name) return false;
  for (size_t i = 0; i < name.size; ++i) {
    const char c = name.data[i];
    if ((c < 'a' || c > 'z') && (c < 'A' || c > 'Z') && (c < '0' || c > '9') &&
        c != '_' && c != '-') return false;
  }
  return true;
}

bool space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

Text take_word(Text& line) {
  while (line.size && space(line.data[0])) {
    ++line.data;
    --line.size;
  }
  size_t length = 0;
  while (length < line.size && !space(line.data[length])) ++length;
  Text word{line.data, length};
  line.data += length;
  line.size -= length;
  return word;
}

struct Shader {
  Text name;
  Text stage;
  Text source;
};

struct Definition {
  size_t size;
  bool valid;
};

void append(Definition& definition, Text part) {
  if (part.size > sizeof(definition_bytes) - definition.size) {
    definition.valid = false;
    return;
  }
  for (size_t i = 0; i < part.size; ++i) definition_bytes[definition.size++] = part.data[i];
}

void append(Definition& definition, const char* part) { append(definition, text(part)); }

void quote(Definition& definition, Text part, const char* prefix = "", const char* suffix = "") {
  append(definition, "\"");
  append(definition, prefix);
  for (size_t i = 0; i < part.size; ++i) {
    const char c = part.data[i];
    if (static_cast<unsigned char>(c) < 0x20) definition.valid = false;
    if (c == '"' || c == '\\') append(definition, "\\");
    const Text character{part.data + i, 1};
    append(definition, character);
  }
  append(definition, suffix);
  append(definition, "\"");
}

void emit_compile(Definition& definition, const Shader& shader, Text compiler) {
  append(definition, "action.generate ");
  quote(definition, shader.name, "compile_");
  append(definition, " {\n    tool = ");
  quote(definition, compiler);
  append(definition, "\n    args = [");
  quote(definition, shader.stage, "-fshader-stage=");
  append(definition, R"ycxx(, input("source"), "-o", output("spirv")]
    inputs = { source = )ycxx");
  quote(definition, shader.source);
  append(definition, " }\n    outputs = { spirv = ");
  quote(definition, shader.name, "", ".spv");
  append(definition, " }\n}\n");
}

void emit_pack(Definition& definition, Text target, Text python, const Shader* shaders, size_t count) {
  append(definition, "action.generate ");
  quote(definition, target);
  append(definition, " {\n    tool = ");
  quote(definition, python);
  append(definition, R"ycxx(
    args = [input("packer"), output("bundle"))ycxx");
  for (size_t i = 0; i < count; ++i) {
    append(definition, ", ");
    quote(definition, shaders[i].name);
    append(definition, ", input(");
    quote(definition, shaders[i].name, "shader_");
    append(definition, ")");
  }
  append(definition, "]\n    inputs = { packer = \"rules/pack.py\"\n");
  for (size_t i = 0; i < count; ++i) {
    append(definition, "        ");
    quote(definition, shaders[i].name, "shader_");
    append(definition, " = artifact(");
    quote(definition, shaders[i].name, "compile_");
    append(definition, ", \"spirv\")\n");
  }
  append(definition, "    }\n    outputs = { bundle = ");
  quote(definition, target, "", ".bundle");
  append(definition, " }\n}\n");
}

int reject(const ycxx_rule_host_v1* host, const char* message) {
  if (host->diagnose) host->diagnose(host->context, message, text(message).size);
  return 1;
}

}  // namespace

extern "C" int ycxx_rule_plan_v1(const ycxx_rule_host_v1* host) {
  if (!host || host->abi_version != YCXX_RULE_ABI_VERSION || host->struct_size < sizeof(*host) ||
      !host->rule || !host->target || !host->attribute || !host->read || !host->emit) return 1;

  const Text target = text(host->target);
  if (compare(text(host->rule), text("bundle")) != 0 || !valid_name(target))
    return reject(host, "expected the bundle rule and a target name of 1 to 63 ASCII letters, digits, underscores or hyphens");

  Text inventory{}, compiler{}, python{};
  if (host->attribute(host->context, "inventory", &inventory) != 1 || inventory.size == 0 ||
      inventory.size > 255 || host->attribute(host->context, "compiler", &compiler) != 1 ||
      compiler.size == 0 || compiler.data[0] != '/' ||
      host->attribute(host->context, "python", &python) != 1 || python.size == 0 ||
      python.data[0] != '/')
    return reject(host, "expected an inventory path of at most 255 bytes and absolute compiler and python paths");

  char inventory_path[256];
  for (size_t i = 0; i < inventory.size; ++i) inventory_path[i] = inventory.data[i];
  inventory_path[inventory.size] = '\0';
  Text contents{};
  if (host->read(host->context, inventory_path, &contents) != 1)
    return reject(host, "the shader inventory must exist and be listed in reads");

  Shader shaders[max_shaders];
  size_t count = 0;
  size_t first = 0;
  while (first < contents.size) {
    size_t end = first;
    while (end < contents.size && contents.data[end] != '\n') ++end;
    Text line{contents.data + first, end - first};
    first = end < contents.size ? end + 1 : end;
    const Text name = take_word(line);
    if (name.size == 0 || name.data[0] == '#') continue;

    const Text stage = take_word(line);
    const Text source = take_word(line);
    if (!valid_name(name) || (compare(stage, text("vertex")) != 0 &&
                              compare(stage, text("fragment")) != 0) ||
        source.size == 0 || source.data[0] == '/' || take_word(line).size != 0)
      return reject(host, "expected a shader name, vertex or fragment stage, and relative source path per inventory row");
    if (count == max_shaders) return reject(host, "expected at most 16 shaders in the inventory");

    // Compile names and pack inputs use the runtime names. Row order is immaterial.
    size_t at = count;
    while (at > 0 && compare(name, shaders[at - 1].name) < 0) {
      shaders[at] = shaders[at - 1];
      --at;
    }
    if (at > 0 && compare(name, shaders[at - 1].name) == 0)
      return reject(host, "duplicate shader name in the inventory");
    const Shader shader{name, stage, source};
    shaders[at] = shader;
    ++count;
  }
  if (count == 0) return reject(host, "expected at least one shader in the inventory");

  Definition definition{0, true};
  for (size_t i = 0; i < count; ++i) emit_compile(definition, shaders[i], compiler);
  emit_pack(definition, target, python, shaders, count);
  if (!definition.valid) return reject(host, "invalid string or oversized shader definition");
  return host->emit(host->context, definition_bytes, definition.size);
}
