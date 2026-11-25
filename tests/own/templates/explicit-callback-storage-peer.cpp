extern "C" {
    extern long (*volatile explicit_callback)(int);

    int verify_explicit_callback() {
        auto callback = explicit_callback;
        if (callback == nullptr)
            return 1;
        return callback(6) == 28L ? 0 : 2;
    }
}
