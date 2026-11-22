template<class T>
int count(T*, int value = sizeof(T)); //! error
template<class T>
int count(const volatile T*, int value = 0);

int use(void* p) { return count(p); }
