extern "C" const char* marker_address();
extern "C" int observe_marker();

int main(int argc, char**) {
  const char* address = marker_address();
  int observed = observe_marker();
  if (argc == 1) return address == nullptr && observed == 0 ? 0 : 99;
  if (address == nullptr || *address != 41) return 98;
  return observed;
}
