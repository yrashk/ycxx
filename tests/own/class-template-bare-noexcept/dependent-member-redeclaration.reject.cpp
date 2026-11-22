// [except.spec], [temp.inst]
struct Ready { static constexpr bool ready = true; };

template<class T> struct Endpoint {
    int read() const noexcept;
};

template<class T>
int Endpoint<T>::read() const noexcept(T::ready) { return 2; }  //! error

int use(const Endpoint<Ready>& endpoint) { return endpoint.read(); }
