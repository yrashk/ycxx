// #715: an argument copy-initializing a W<int> parameter cannot use its explicit constructor template ([class.conv.ctor], [over.match.copy])
template <class T> struct W {
    T v;
    template <class U> explicit W(U u) : v(u) {}
};

int take(W<int> w) { return w.v; }

int ok() { return take(W<int>('a')); }
int bad() { return take('a'); } //! error
