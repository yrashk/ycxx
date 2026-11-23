#pragma once

struct CastBase { int base; };
struct CastDerived : CastBase { int value; };

extern "C" const CastDerived* identity(CastDerived* source);
extern "C" const CastBase* upcast(CastDerived* source);
