template<int N>
struct Tag {};

template<int First, int Second>
struct Pair {};

extern Tag<4> shifted;
extern Tag<auto(8 >> 1)> shifted;

extern Tag<8> added;
extern Tag<auto(7) + 1> added;

extern Pair<7, 9> comma;
extern Pair<auto(7), auto{9}> comma;

extern Tag<1> compared;
extern Tag<auto{8 > 1}> compared;
