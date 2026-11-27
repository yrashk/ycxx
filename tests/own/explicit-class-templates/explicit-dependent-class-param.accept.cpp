// #715: explicit(sizeof(T) > 1) depends on the class template's parameter: W<char> has a converting constructor, W<int> only direct-initializes ([dcl.fct.spec], [temp.inst])
template <class T> struct W {
    T v;
    explicit(sizeof(T) > 1) W(T x) : v(x) {}
};

W<char> c_copy = 'a';
W<char> c_list = {'b'};
W<int> i_direct(1);
W<int> i_list{2};

int take(W<char> w) { return w.v; }
int use() { return take('c') + static_cast<W<int>>(3).v; }
