namespace { struct LocalTag { int key; }; }
int missing_definition(const LocalTag& tag) { return tag.key; }

extern "C" int retain_other() {
  LocalTag tag{3};
  return missing_definition(tag);
}
