#ifndef YCXX_RULE_API_H
#define YCXX_RULE_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define YCXX_RULE_ABI_VERSION 1u

typedef struct ycxx_rule_bytes_v1 {
  const char* data;
  size_t size;
} ycxx_rule_bytes_v1;

typedef struct ycxx_rule_host_v1 {
  uint32_t abi_version;
  size_t struct_size;
  const char* rule;
  const char* target;
  ycxx_rule_bytes_v1 definition;
  void* context;

  /* Return 1 for present bytes, 0 for missing, or -1 for invalid calls or undeclared reads.
     The bytes are borrowed and stay valid through the entry point's return. */
  int (*attribute)(void* context, const char* name, ycxx_rule_bytes_v1* result);
  int (*read)(void* context, const char* path, ycxx_rule_bytes_v1* result);

  /* Supply one nonempty definition. The worker copies it. Return 0 on success. */
  int (*emit)(void* context, const char* definition, size_t size);
} ycxx_rule_host_v1;

/* Call the host from this thread before returning. Return 0 after successfully emitting
   the definition, or a nonzero rule error. */
int ycxx_rule_plan_v1(const ycxx_rule_host_v1* host);

#ifdef __cplusplus
}
#endif

#endif
