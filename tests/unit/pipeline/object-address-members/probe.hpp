#pragma once
namespace config {
template<int* Object> struct Probe {
  static int read() noexcept { return *Object; }
};
using Read = int (*)() noexcept;
extern int shared;
template<> int Probe<&shared>::read() noexcept;
}
using config::Read;
Read peer_shared() noexcept;
Read peer_private() noexcept;
int* peer_shared_object() noexcept;
int* peer_private_object() noexcept;
