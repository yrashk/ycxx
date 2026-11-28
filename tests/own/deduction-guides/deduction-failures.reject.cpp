// #316: a deduced class type needs an initializer, is the declared type alone, and needs a viable guide ([dcl.type.class.deduct]/1, [over.match.class.deduct])
template <class T> struct W {
    W(T*) {}
};

int i;
W ok(&i);
W missing; //! error
W* pointer = &ok; //! error
W none(1); //! error
