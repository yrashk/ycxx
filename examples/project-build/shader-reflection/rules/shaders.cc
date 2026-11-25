#include "reflect_json.hh"

namespace {

constexpr char schema[] = "shader.descriptors.v1";
constexpr size_t max_rows = 16;
constexpr size_t max_name = 63;

template<size_t Size>
int reject(const ycxx_rule_host_v1* host, const char (&message)[Size]) {
  if (host->diagnose) host->diagnose(host->context, message, Size - 1);
  return 1;
}

bool valid_host(const ycxx_rule_host_v1* host) {
  return host && host->abi_version == YCXX_RULE_ABI_VERSION && host->struct_size >= sizeof(*host);
}

bool letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

bool identifier(Text name) {
  if (name.size == 0 || name.size > max_name || !letter(name.data[0]) ||
      name.data[name.size - 1] == '_') return false;
  // The generated C++ name uses an underscore before and after the block name.
  for (size_t i = 1; i < name.size; ++i) {
    const char c = name.data[i];
    if (!letter(c) && !digit(c) && (c != '_' || name.data[i - 1] == '_')) return false;
  }
  return true;
}

struct Descriptor {
  Text name;
  uint32_t set;
  uint32_t binding;
};

bool descriptor(Json& json, Descriptor& result) {
  if (!take(json, '{')) return false;
  bool has_name = false;
  bool has_set = false;
  bool has_binding = false;
  do {
    Text key{};
    if (!string(json, key) || !take(json, ':')) return false;
    if (equal(key, "name")) {
      if (has_name || !string(json, result.name) || !identifier(result.name)) return false;
      has_name = true;
    } else if (equal(key, "set")) {
      if (has_set || !u32(json, result.set)) return false;
      has_set = true;
    } else if (equal(key, "binding")) {
      if (has_binding || !u32(json, result.binding)) return false;
      has_binding = true;
    } else if (!skip(json)) {
      return false;
    }
  } while (take(json, ','));
  return has_name && has_set && has_binding && take(json, '}');
}

bool descriptors(Json& json, Descriptor (&uniforms)[max_rows], size_t& count) {
  if (!take(json, '{')) return false;
  if (take(json, '}')) return done(json);
  bool has_ubos = false;
  do {
    Text key{};
    if (!string(json, key) || !take(json, ':')) return false;
    if (!equal(key, "ubos")) {
      if (!skip(json)) return false;
      continue;
    }
    if (has_ubos || !take(json, '[')) return false;
    has_ubos = true;
    if (take(json, ']')) continue;
    do {
      if (count == max_rows || !descriptor(json, uniforms[count])) return false;
      ++count;
    } while (take(json, ','));
    if (!take(json, ']')) return false;
  } while (take(json, ','));
  return take(json, '}') && done(json);
}

struct Writer {
  char* data;
  size_t capacity;
  size_t size = 0;
  bool valid = true;
};

void append(Writer& writer, char c) {
  if (writer.size == writer.capacity) {
    writer.valid = false;
    return;
  }
  writer.data[writer.size++] = c;
}

void append(Writer& writer, Text part) {
  for (size_t i = 0; i < part.size; ++i) append(writer, part.data[i]);
}

void append(Writer& writer, const char* part) { append(writer, text(part)); }

void quote(Writer& writer, Text part) {
  append(writer, '"');
  for (size_t i = 0; i < part.size; ++i) {
    switch (part.data[i]) {
      case '\n': append(writer, "\\n"); break;
      case '\r': append(writer, "\\r"); break;
      case '\t': append(writer, "\\t"); break;
      case '\\': append(writer, "\\\\"); break;
      case '"': append(writer, "\\\""); break;
      default:
        if (static_cast<unsigned char>(part.data[i]) < 0x20) writer.valid = false;
        append(writer, part.data[i]);
    }
  }
  append(writer, '"');
}

void quote(Writer& writer, const char* part) { quote(writer, text(part)); }

void number(Writer& writer, uint32_t value) {
  char digits[10];
  size_t count = 0;
  do {
    digits[count++] = static_cast<char>('0' + value % 10u);
    value /= 10u;
  } while (value != 0);
  while (count != 0) append(writer, digits[--count]);
}

bool attribute(const ycxx_rule_host_v1* host, const char* name, Text& result) {
  return host->attribute(host->context, name, &result) == 1 && result.size != 0;
}

bool tool(const ycxx_rule_host_v1* host, const char* name, Text& result) {
  return attribute(host, name, result) && result.data[0] == '/';
}

void action(Writer& definition, const char* kind, const char* target) {
  append(definition, kind);
  append(definition, ' ');
  quote(definition, target);
  append(definition, " {\n");
}

void spirv(Writer& definition, Text module) {
  append(definition, "artifact(");
  quote(definition, module);
  append(definition, ", \"spirv\")");
}

bool module(const ycxx_rule_host_v1* host, Writer& definition) {
  Text source{}, stage{}, compiler{};
  if (!attribute(host, "source", source) || source.data[0] == '/' ||
      !attribute(host, "stage", stage) || (!equal(stage, "vertex") && !equal(stage, "fragment")) ||
      !tool(host, "compiler", compiler)) return false;

  action(definition, "action.generate", host->target);
  append(definition, "    tool = ");
  quote(definition, compiler);
  append(definition, "\n    args = [\"-fshader-stage=");
  append(definition, stage);
  append(definition, R"ycxx(", input("source"), "-o", output("spirv")]
    inputs = { source = )ycxx");
  quote(definition, source);
  append(definition, " }\n    outputs = { spirv = \"");
  append(definition, stage);
  append(definition, ".spv\" }\n}\n");
  return true;
}

bool pack(const ycxx_rule_host_v1* host, Writer& definition) {
  Text vertex{}, fragment{}, python{};
  if (!attribute(host, "vertex", vertex) || !attribute(host, "fragment", fragment) ||
      !tool(host, "python", python)) return false;

  action(definition, "action.generate", host->target);
  append(definition, "    tool = ");
  quote(definition, python);
  append(definition, R"ycxx(
    args = [input("packer"), output("bundle"), input("vertex"), input("fragment")]
    inputs = {
        packer = "rules/pack.py"
        vertex = )ycxx");
  spirv(definition, vertex);
  append(definition, "\n        fragment = ");
  spirv(definition, fragment);
  append(definition, "\n    }\n    outputs = { bundle = \"scene.bundle\" }\n}\n");
  return true;
}

bool reflect(const ycxx_rule_host_v1* host, Writer& definition) {
  Text target{}, reflector{};
  if (!attribute(host, "module", target) || !tool(host, "reflector", reflector)) return false;

  action(definition, "action.observe", host->target);
  append(definition, "    export = \"layout\"\n    schema = ");
  quote(definition, schema);
  append(definition, "\n    tool = ");
  quote(definition, reflector);
  append(definition, R"ycxx(
    args = [input("module"), "--reflect"]
    inputs = { module = )ycxx");
  spirv(definition, target);
  append(definition, " }\n}\n");
  return true;
}

void constant(Writer& header, Text name, const ycxx_rule_fact_field_v1& field) {
  append(header, "inline constexpr unsigned descriptor_");
  append(header, name);
  append(header, '_');
  append(header, field.name);
  append(header, " = ");
  number(header, field.number);
  append(header, "u;\n");
}

bool bindings(const ycxx_rule_host_v1* host, Writer& definition) {
  if (host->fact_count != 1 || !host->facts) return false;
  const auto& layout = host->facts[0];
  if (!equal(text(layout.name), "layout") || !equal(text(layout.schema), schema) ||
      layout.row_count > max_rows) return false;

  char header_bytes[4096];
  Writer header{header_bytes, sizeof(header_bytes)};
  append(header, "#pragma once\n\nnamespace scene {\n");
  for (size_t i = 0; i < layout.row_count; ++i) {
    const auto& row = layout.rows[i];
    const auto name = text(row.name);
    if (!identifier(name) || row.field_count != 2) return false;
    const auto& binding = row.fields[0];
    const auto& set = row.fields[1];
    if (!equal(text(binding.name), "binding") || binding.kind != YCXX_RULE_FACT_U32_V1 ||
        !equal(text(set.name), "set") || set.kind != YCXX_RULE_FACT_U32_V1) return false;
    constant(header, name, set);
    constant(header, name, binding);
  }
  append(header, "}  // namespace scene\n");
  if (!header.valid) return false;

  action(definition, "action.generate", host->target);
  append(definition, R"ycxx(    tool = "/bin/sh"
    args = ["-c", "printf '%s' \"$1\" > \"$2\"", "shader-bindings", )ycxx");
  quote(definition, Text{header.data, header.size});
  append(definition, R"ycxx(, output("header")]
    outputs = { header = "scene_bindings.hh" }
}
)ycxx");
  return true;
}

}  // namespace

extern "C" int ycxx_rule_plan_v1(const ycxx_rule_host_v1* host) {
  if (!valid_host(host) || !host->rule || !host->target || !host->attribute || !host->emit) return 1;

  char definition_bytes[8192];
  Writer definition{definition_bytes, sizeof(definition_bytes)};
  const auto rule = text(host->rule);
  if (equal(rule, "module")) {
    if (!module(host, definition))
      return reject(host, "expected a relative source, vertex or fragment stage, and absolute compiler path");
  } else if (equal(rule, "pack")) {
    if (!pack(host, definition))
      return reject(host, "expected vertex and fragment module targets and an absolute python path");
  } else if (equal(rule, "reflect")) {
    if (!reflect(host, definition))
      return reject(host, "expected a module target and an absolute reflector path");
  } else if (equal(rule, "bindings")) {
    if (!bindings(host, definition))
      return reject(host, "expected the layout fact with shader.descriptors.v1 set and binding fields");
  } else {
    return reject(host, "expected module, pack, reflect, or bindings rule");
  }

  if (!definition.valid) return reject(host, "invalid string or oversized shader definition");
  return host->emit(host->context, definition.data, definition.size);
}

extern "C" int ycxx_rule_classify_v1(const ycxx_rule_host_v1* host) {
  if (!valid_host(host) || !host->witness || !host->export_fact) return 1;
  const auto& witness = *host->witness;
  if (!witness.name || !witness.schema || !equal(text(witness.name), "layout") ||
      !equal(text(witness.schema), schema)) return reject(host, "expected the shader layout witness");
  if (witness.exit_status != 0) return reject(host, "spirv-cross failed to reflect the module");

  Json json{witness.out};
  Descriptor uniforms[max_rows];
  size_t count = 0;
  if (!descriptors(json, uniforms, count))
    return reject(host, "expected spirv-cross JSON with up to 16 uniform blocks, portable names, and u32 set and binding fields");

  char names[max_rows][max_name + 1];
  ycxx_rule_fact_field_v1 fields[max_rows][2];
  ycxx_rule_fact_row_v1 rows[max_rows];
  for (size_t i = 0; i < count; ++i) {
    const auto& uniform = uniforms[i];
    for (size_t j = 0; j < uniform.name.size; ++j) names[i][j] = uniform.name.data[j];
    names[i][uniform.name.size] = '\0';
    fields[i][0] = {"set", YCXX_RULE_FACT_U32_V1, uniform.set, {}};
    fields[i][1] = {"binding", YCXX_RULE_FACT_U32_V1, uniform.binding, {}};
    rows[i] = {names[i], fields[i], 2};
  }
  const ycxx_rule_fact_v1 layout{witness.name, schema, rows, count};
  return host->export_fact(host->context, &layout);
}
