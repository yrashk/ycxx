#pragma once

namespace shaders {

inline const char* find_module(const char* bundle, const char* name) {
  while (*bundle != '\0') {
    const char* entry = bundle;
    const char* part = name;
    while (*part != '\0' && *entry == *part) {
      ++entry;
      ++part;
    }
    if (*part == '\0' && *entry == ' ') return entry + 1;
    while (*bundle != '\0' && *bundle != '\n') ++bundle;
    if (*bundle == '\n') ++bundle;
  }
  return nullptr;
}

inline unsigned nibble(char digit) {
  if (digit >= '0' && digit <= '9') return static_cast<unsigned>(digit - '0');
  if (digit >= 'a' && digit <= 'f') return static_cast<unsigned>(digit - 'a') + 10;
  return 16;
}

constexpr unsigned header_words = 5;

struct Module {
  const char* hex;
  unsigned words = 0;

  unsigned word(unsigned index) const {
    unsigned result = 0;
    for (unsigned byte = 0; byte < 4; ++byte) {
      const unsigned offset = index * 8 + byte * 2;
      const unsigned value = nibble(hex[offset]) * 16 + nibble(hex[offset + 1]);
      result |= value << (byte * 8);
    }
    return result;
  }

  bool valid() {
    // The packer records each module's little-endian bytes in a hex line.
    unsigned digits = 0;
    while (hex[digits] != '\n' && hex[digits] != '\0') {
      if (nibble(hex[digits]) >= 16) return false;
      ++digits;
    }
    words = digits / 8;
    if (digits % 8 != 0 || words < header_words || word(0) != 0x07230203u ||
        word(3) == 0 || word(4) != 0) return false;

    unsigned instruction = header_words;
    while (instruction < words) {
      const unsigned length = word(instruction) >> 16;
      if (length == 0 || length > words - instruction) return false;
      instruction += length;
    }
    return true;
  }
};

}  // namespace shaders
