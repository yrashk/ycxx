// #339: conversion functions with distinct target types can coexist
// with the same object qualifiers ([class.conv.fct]).
template<class T>
struct Converter {
  using value_type = T;
  operator value_type() const;
  operator int() const;
};

static_assert(sizeof(Converter<long>) > 0);
