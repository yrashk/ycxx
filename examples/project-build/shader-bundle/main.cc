namespace {

bool valid_spirv_entry(const char* hex) {
  // SPIR-V starts with a five-word header whose first word is the magic number.
  const char* magic = "03022307";
  while (*magic != '\0') {
    if (*hex != *magic) return false;
    ++hex;
    ++magic;
  }
  unsigned digits = 8;
  while (*hex != '\0' && *hex != '\n') {
    const char digit = *hex++;
    if ((digit < '0' || digit > '9') && (digit < 'a' || digit > 'f')) return false;
    ++digits;
  }
  return digits >= 40 && digits % 8 == 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) return 1;
  const char* entry = argv[2];
  while (*entry != '\0') {
    const char* name = argv[1];
    while (*name != '\0' && *entry != '\n' && *entry == *name) {
      ++entry;
      ++name;
    }
    if (*name == '\0' && *entry == ' ') return valid_spirv_entry(entry + 1) ? 0 : 3;
    while (*entry != '\0' && *entry != '\n') ++entry;
    if (*entry == '\n') ++entry;
  }
  return 2;
}
