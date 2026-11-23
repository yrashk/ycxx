#pragma once

struct Root {
  virtual int step() = 0;
};

struct Owner : Root {
  int payload;
  int step() override;
};

extern "C" int dispatch(Root* source);
extern "C" const Root* borrow();
extern "C" const void* complete_address();
extern "C" const Owner* identity(Owner* source);
extern "C" const Root* upcast(Owner* source);
