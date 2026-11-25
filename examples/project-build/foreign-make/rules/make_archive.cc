#include "ycxx_rule_api.h"

namespace {

using Text = ycxx_rule_bytes_v1;

Text text(const char* value) {
  size_t size = 0;
  while (value[size] != '\0') ++size;
  return {value, size};
}

bool equal(Text left, Text right) {
  if (left.size != right.size) return false;
  for (size_t i = 0; i < left.size; ++i)
    if (left.data[i] != right.data[i]) return false;
  return true;
}

bool relative_file(Text path) {
  size_t start = 0;
  for (size_t i = 0; i <= path.size; ++i) {
    if (i < path.size && path.data[i] != '/') {
      if (static_cast<unsigned char>(path.data[i]) < 0x20) return false;
      continue;
    }
    if (i == start) return false;
    const Text component{path.data + start, i - start};
    if (equal(component, text(".")) || equal(component, text(".."))) return false;
    start = i + 1;
  }
  return true;
}

bool tool_path(Text path) {
  if (path.size < 2 || path.data[0] != '/') return false;
  for (size_t i = 0; i < path.size; ++i)
    if (static_cast<unsigned char>(path.data[i]) < 0x20) return false;
  return true;
}

Text directory_prefix(Text path) {
  size_t end = path.size;
  while (end && path.data[end - 1] != '/') --end;
  return {path.data, end};
}

Text take_line(Text& contents) {
  size_t length = 0;
  while (length < contents.size && contents.data[length] != '\n') ++length;
  const auto consumed = length + (length < contents.size);
  const Text line{contents.data, length && contents.data[length - 1] == '\r' ? length - 1 : length};
  contents = {contents.data + consumed, contents.size - consumed};
  return line;
}

// The rule worker copies the emitted declaration before this process exits.
char definition_bytes[1024 * 1024];

struct Definition {
  size_t size = 0;
  bool valid = true;
};

void append(Definition& definition, Text value) {
  if (value.size > sizeof(definition_bytes) - definition.size) {
    definition.valid = false;
    return;
  }
  for (size_t i = 0; i < value.size; ++i) definition_bytes[definition.size++] = value.data[i];
}

void append(Definition& definition, const char* value) { append(definition, text(value)); }

void quoted_bytes(Definition& definition, Text value) {
  for (size_t i = 0; i < value.size; ++i) {
    const char c = value.data[i];
    switch (c) {
      case '"': append(definition, "\\\""); break;
      case '\\': append(definition, "\\\\"); break;
      case '\n': append(definition, "\\n"); break;
      case '\r': append(definition, "\\r"); break;
      case '\t': append(definition, "\\t"); break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) definition.valid = false;
        else append(definition, Text{value.data + i, 1});
        break;
    }
  }
}

void quote(Definition& definition, Text value, Text prefix = Text{}) {
  append(definition, "\"");
  quoted_bytes(definition, prefix);
  quoted_bytes(definition, value);
  append(definition, "\"");
}

int reject(const ycxx_rule_host_v1* host, const char* message) {
  if (host->diagnose) host->diagnose(host->context, message, text(message).size);
  return 1;
}

}  // namespace

extern "C" int ycxx_rule_plan_v1(const ycxx_rule_host_v1* host) {
  if (!host || host->abi_version != YCXX_RULE_ABI_VERSION || host->struct_size < sizeof(*host) ||
      !host->rule || !host->target || !host->attribute || !host->read || !host->emit) return 1;
  if (!equal(text(host->rule), text("archive"))) return reject(host, "expected the foreign archive rule");

  Text make{}, cc{}, ar{}, makefile{}, inventory{};
  if (host->attribute(host->context, "make", &make) != 1 || !tool_path(make) ||
      host->attribute(host->context, "cc", &cc) != 1 || !tool_path(cc) ||
      host->attribute(host->context, "ar", &ar) != 1 || !tool_path(ar))
    return reject(host, "make, cc and ar must name absolute executable paths");
  if (host->attribute(host->context, "makefile", &makefile) != 1 || !relative_file(makefile) ||
      host->attribute(host->context, "inventory", &inventory) != 1 || !relative_file(inventory))
    return reject(host, "makefile and inventory must name normalized workspace-relative files");

  // The read callback takes a NUL-terminated name. Reuse the buffer before emitting.
  if (inventory.size >= sizeof(definition_bytes)) return reject(host, "inventory path is too long");
  for (size_t i = 0; i < inventory.size; ++i) definition_bytes[i] = inventory.data[i];
  definition_bytes[inventory.size] = '\0';
  Text contents{};
  if (host->read(host->context, definition_bytes, &contents) != 1)
    return reject(host, "the inventory must be available through reads");

  const auto directory = directory_prefix(makefile);
  const Text basename{makefile.data + directory.size, makefile.size - directory.size};
  Definition definition;
  append(definition, "action.generate ");
  quote(definition, text(host->target));
  append(definition, " {\n    tool = ");
  quote(definition, make);
  append(definition, R"ycxx(
    args = ["-f", input("makefile"), "archive"]
    inputs = {
        makefile = )ycxx");
  quote(definition, makefile);
  while (contents.size) {
    const auto path = take_line(contents);
    if (!path.size) continue;
    if (!relative_file(path))
      return reject(host, "inventory entries must be normalized paths relative to the Makefile directory");
    if (equal(path, basename)) return reject(host, "the Makefile is staged separately from the inventory");
    append(definition, "\n        ");
    quote(definition, path, text("file:"));
    append(definition, " = ");
    quote(definition, path, directory);
  }
  append(definition, "\n    }\n    environment = {\n        SOURCE_DIR = ");
  if (directory.size) quote(definition, Text{directory.data, directory.size - 1}, text("inputs/"));
  else quote(definition, text("inputs"));
  append(definition, "\n        CC = ");
  quote(definition, cc);
  append(definition, "\n        AR = ");
  quote(definition, ar);
  append(definition, R"ycxx(
        ARCHIVE = "outputs/libforeign.a"
        PATH = "/usr/bin:/bin"
        LC_ALL = "C"
        ZERO_AR_DATE = "1"
    }
    outputs = { archive = "libforeign.a" }
}
)ycxx");
  if (!definition.valid) return reject(host, "invalid string or oversized foreign archive declaration");
  return host->emit(host->context, definition_bytes, definition.size);
}
