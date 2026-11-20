extern "C" int external_step(int value);
extern "C" int requested_alias(int value) asm("step_alias");
using Step = decltype(&external_step);

extern "C" Step original_address() { return &external_step; }
extern "C" Step alias_address() { return &requested_alias; }
