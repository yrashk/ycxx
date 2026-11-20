// A qualified definition needs a corresponding prior namespace member ([dcl.meaning.general], [dcl.link]).
namespace n { extern "C" int ycxx_lookup(int); }
int n::ycxx_lookup(int v) { return v; }
int main() { return ::ycxx_lookup(0); } //! error
