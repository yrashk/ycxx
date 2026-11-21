int select(int);
int select(long) noexcept;
int (*target)(int) noexcept = &select; //! error
