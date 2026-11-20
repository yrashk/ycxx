#ifndef YCXX_INLINE_DATA_432_SHARED_DATA_H
#define YCXX_INLINE_DATA_432_SHARED_DATA_H

namespace data {
inline int initialized = 17;
inline int zero_initialized;
inline const int immutable = 37;

extern inline int redeclared;
int redeclared = 23;

template<class T>
int specialized = 29;

template<>
inline int specialized<int> = 31;
}

int* main_initialized_address();
int* main_zero_address();
int* main_redeclared_address();
int* main_specialized_address();
const int* main_immutable_address();

int* peer_initialized_address();
int* peer_zero_address();
int* peer_redeclared_address();
int* peer_specialized_address();
const int* peer_immutable_address();

int peer_values_match(int initialized, int zero, int redeclared, int specialized);
void peer_write_values(int initialized, int zero, int redeclared, int specialized);

#endif
