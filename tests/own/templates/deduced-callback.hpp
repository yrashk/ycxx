template<class Key>
struct CallbackSource {
    static auto evaluate() { return 61L; }
};

template<class Key>
struct DeferredCallback {
    inline static long (*volatile callback)() = &CallbackSource<Key>::evaluate;

    static long invoke() { return DeferredCallback<Key>::callback(); }
    static auto inspect() { return invoke(); }
};

// [dcl.spec.auto.general] requires return-type deduction in the unevaluated sizeof operand.
static_assert(sizeof(DeferredCallback<int>::inspect()) == sizeof(long));
