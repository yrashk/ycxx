#include "probe.hpp"

using config::Probe;
using config::shared;
static int private_value = 47;
Read peer_shared() noexcept { return &Probe<&shared>::read; }
Read peer_private() noexcept { return &Probe<&private_value>::read; }
int* peer_shared_object() noexcept { return &shared; }
int* peer_private_object() noexcept { return &private_value; }
