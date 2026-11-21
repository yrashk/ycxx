template<class T> concept Integral = __is_integral(T);
template<Integral T> struct Named;
Named<int>* accepted;
