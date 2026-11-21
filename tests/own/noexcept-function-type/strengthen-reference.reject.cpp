using Safe = int(double) noexcept;
Safe& strengthen(int (&ref)(double)) { return ref; } //! error
