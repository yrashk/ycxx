// [except.handle] Named and abstract declarations preserve cv and references.
void work();
int verify_reference(const volatile int&);
int verify_pointer(const int* const&);
int invoke(int (*)(int));

int inspect() {
  try {
    work();
  } catch (const volatile int& value) {
    const volatile int* address = &value;
    return verify_reference(*address);
  } catch (const int* const& pointer) {
    const int* const* address = &pointer;
    return verify_pointer(*address);
  } catch (int (*)(int)) {
    return 1;
  } catch (const void* pointer) {
    return pointer != nullptr;
  } catch (...) {
    return 2;
  }
  return 0;
}

// [except.handle] Array and function handler types adjust to pointers.
int adjustments() {
  try {
    work();
  } catch (int values[3]) {
    int** address = &values;
    return **address;
  } catch (int function(int)) {
    int (**address)(int) = &function;
    return invoke(*address);
  }
  return 0;
}

void abstract_adjustments() {
  try { work(); }
  catch (int[]) {}
  catch (int(int)) {}
}
