using Safe = int(double) noexcept;
Safe* strengthen(int (*ptr)(double)) { return ptr; } //! error
