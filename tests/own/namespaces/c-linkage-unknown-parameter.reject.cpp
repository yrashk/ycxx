// A qualified definition needs a corresponding prior namespace member ([dcl.meaning.general], [dcl.link]).
namespace api { extern "C" int ycxx_lookup(int); }
int api::ycxx_lookup(double v) { return static_cast<int>(v); } //! error
