template<bool> struct Result {};
template<class T> constexpr bool Enabled = true;
template<class T> bool Runtime = true;
template<class T> using Checked = Result<Enabled<T> && Runtime<T>>; using Invalid = Checked<int>; //! error
