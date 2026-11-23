// [class.access.base]: access is through the naming class's base path.
struct Origin { using T = char; };
struct Hidden : private Origin {};
struct Combined : Hidden {
  using Published = T; //! error
};
