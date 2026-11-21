// The substituted base must be complete at the first demand ([class.derived], [temp.inst]).
struct Incomplete;
template<class Base> struct Derived : Base {}; static_assert(sizeof(Derived<Incomplete>) > 0); //! error
