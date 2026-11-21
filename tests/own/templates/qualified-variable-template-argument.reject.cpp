namespace Flags {
template<class T> bool mutable_flag = true;
}
template<bool B> struct Result {};
// Keep the alias definition and use on one line for both diagnostic locations.
template<class T> using Alias = Result<Flags::mutable_flag<T>>; Alias<int> invalid; //! error
