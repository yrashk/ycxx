namespace Flags {
template<class T> bool mutable_flag = true;
}
template<bool B> struct Result {};
template<class T> using Alias = Result<Flags::mutable_flag<T>>;

Alias<int> invalid; //! error: the template arguments do not match
