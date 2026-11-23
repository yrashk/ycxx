#if defined(__PIE__) || defined(__pie__)
#error unexpected PIE for a PIC object
#endif

#if defined(__PIC__) || defined(__pic__)
# if __PIC__ != 2 || __pic__ != 2
#  error unexpected PIC level
# endif
# define PIC_LEVEL 2
#else
# define PIC_LEVEL 0
#endif

static int local_data = 17;

extern "C" {
extern int pic_external_data;
int pic_external_call(int value);
using Operation = decltype(pic_external_call);
[[gnu::visibility("hidden")]] int pic_hidden_data = 13;

int* pic_data_cell = &pic_external_data;
Operation* pic_function_cell = &pic_external_call;

int pic_level() { return PIC_LEVEL; }
int* pic_data_address() { return &pic_external_data; }
Operation* pic_function_address() { return &pic_external_call; }
int pic_call(int value) { return pic_external_call(value + 1); }
int* pic_hidden_address() { return &pic_hidden_data; }
int* pic_local_address() { return &local_data; }
}
