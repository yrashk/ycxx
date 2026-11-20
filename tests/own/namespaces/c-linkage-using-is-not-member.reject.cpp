// A qualified definition needs a corresponding prior namespace member ([dcl.meaning.general], [dcl.link]).
namespace original { extern "C" int ycxx_lookup(int); }
namespace n { using original::ycxx_lookup; }
int n::ycxx_lookup(int v) { return v; } //! error
