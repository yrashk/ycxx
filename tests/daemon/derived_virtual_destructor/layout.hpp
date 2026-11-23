#pragma once

// Itanium C++ ABI §2.4 and §5.2.3, [class.dtor].
struct B {
  virtual ~B();
  const char* name_;
};

struct D : B {
  ~D() override;
};
