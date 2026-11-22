// [conv.fpint], [conv.double], [conv.integral]: conversions compiled by ycxx for the host
// consumer, which compares them with its own conversions of the same values.
extern "C" {

double fp643_i32_to_f64(int x) { return x; }
double fp643_i64_to_f64(long x) { return x; }
double fp643_u32_to_f64(unsigned x) { return x; }
double fp643_u64_to_f64(unsigned long x) { return x; }
float fp643_i32_to_f32(int x) { return x; }
float fp643_i64_to_f32(long x) { return x; }
float fp643_u32_to_f32(unsigned x) { return x; }
float fp643_u64_to_f32(unsigned long x) { return x; }
double fp643_i16_to_f64(short x) { return x; }
float fp643_u8_to_f32(unsigned char x) { return x; }
double fp643_bool_to_f64(bool x) { return x; }

int fp643_f64_to_i32(double x) { return static_cast<int>(x); }
long fp643_f64_to_i64(double x) { return static_cast<long>(x); }
unsigned fp643_f64_to_u32(double x) { return static_cast<unsigned>(x); }
unsigned long fp643_f64_to_u64(double x) { return static_cast<unsigned long>(x); }
int fp643_f32_to_i32(float x) { return static_cast<int>(x); }
long fp643_f32_to_i64(float x) { return static_cast<long>(x); }
unsigned fp643_f32_to_u32(float x) { return static_cast<unsigned>(x); }
unsigned long fp643_f32_to_u64(float x) { return static_cast<unsigned long>(x); }
short fp643_f64_to_i16(double x) { return static_cast<short>(x); }
unsigned char fp643_f32_to_u8(float x) { return static_cast<unsigned char>(x); }

float fp643_f64_to_f32(double x) { return static_cast<float>(x); }
double fp643_round_trip(float x) { return static_cast<float>(static_cast<double>(x)); }

double fp643_fabs_f64(double x) { return __builtin_fabs(x); }
float fp643_fabs_f32(float x) { return __builtin_fabsf(x); }

}
