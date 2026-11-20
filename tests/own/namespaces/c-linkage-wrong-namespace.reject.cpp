// A qualified definition needs a corresponding prior namespace member ([dcl.meaning.general], [dcl.link]).
namespace api { extern "C" int ycxx_lookup(int); }
namespace wrong {}
int wrong::ycxx_lookup(int v) { return v; } //! error
