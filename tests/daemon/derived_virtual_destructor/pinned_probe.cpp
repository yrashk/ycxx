namespace std { class type_info { public: virtual ~type_info(); const char* name_; }; }
namespace __cxxabiv1 {
class __fundamental_type_info : public std::type_info { public: ~__fundamental_type_info() override; };
__fundamental_type_info::~__fundamental_type_info() {}
}
