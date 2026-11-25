#include "bundle.hh"

int main(int argc, char** argv) {
  if (argc != 3) return 1;
  const char* entry = shaders::find_module(argv[2], argv[1]);
  if (!entry) return 2;
  shaders::Module module{entry};
  return module.valid() ? 0 : 3;
}
