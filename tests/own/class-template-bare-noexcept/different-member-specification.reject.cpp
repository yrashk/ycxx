// [except.spec]
template<class T> struct Endpoint {
    int read() const noexcept;
};

template<class T>
int Endpoint<T>::read() const noexcept(false) { return 2; }  //! error

int use(const Endpoint<int>& endpoint) { return endpoint.read(); }
