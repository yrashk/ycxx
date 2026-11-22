namespace { struct LocalTag { int key; }; }
int missing_definition(const LocalTag&);

int main() {
  LocalTag tag{4};
  return missing_definition(tag);
}
