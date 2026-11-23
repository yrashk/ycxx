extern "C" {
#ifdef PIC_PROVIDER
int pic_external_data = 7;
int pic_external_call(int value) { return value + 9; }
#else
extern int pic_external_data;
int pic_external_call(int value);
using Operation = decltype(pic_external_call);
# ifdef PREEMPT
int pic_external_data = 41;
int pic_external_call(int value) { return value + 53; }
int pic_hidden_data = 99;
# endif

extern int* pic_data_cell;
extern Operation* pic_function_cell;
int pic_level();
int* pic_data_address();
Operation* pic_function_address();
int pic_call(int value);
int* pic_hidden_address();
int* pic_local_address();
#endif
}

#ifndef PIC_PROVIDER
int main() {
# ifdef PREEMPT
  constexpr int expected_data = 41;
  constexpr int expected_call = 57;
# else
  constexpr int expected_data = 7;
  constexpr int expected_call = 13;
# endif
  if (pic_level() != 2) return 1;
  int* address = pic_data_address();
  if (address != &pic_external_data || pic_data_cell != address || *address != expected_data) return 2;
  *address = expected_data + 1;
  if (pic_external_data != expected_data + 1) return 3;
  if (pic_function_address() != &pic_external_call || pic_function_cell != &pic_external_call) return 4;
  if (pic_call(3) != expected_call || pic_function_cell(4) != expected_call) return 5;
  if (*pic_hidden_address() != 13 || *pic_local_address() != 17) return 6;
# ifdef PREEMPT
  if (pic_hidden_address() == &pic_hidden_data || pic_hidden_data != 99) return 7;
# endif
  return 0;
}
#endif
