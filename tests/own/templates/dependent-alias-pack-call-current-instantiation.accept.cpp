template<class T> struct Self {
    using Arg = typename T::Arg;
    static auto run(int value) { return Self<T>::Arg(value); }
};

struct HasArg { using Arg = int; };

int instantiate() { return Self<HasArg>::run(7); }
