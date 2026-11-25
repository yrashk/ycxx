#ifndef YCXX_RULE_API_H
#define YCXX_RULE_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define YCXX_RULE_ABI_VERSION 1u
#define YCXX_RULE_SUPPORTED_V1 0u
#define YCXX_RULE_UNSUPPORTED_V1 1u
#define YCXX_RULE_INDETERMINATE_V1 2u

typedef struct ycxx_rule_bytes_v1 {
  const char* data;
  size_t size;
} ycxx_rule_bytes_v1;

typedef struct ycxx_rule_capability_v1 {
  const char* name;
  uint32_t support;
} ycxx_rule_capability_v1;

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

  /* Check struct_size before accessing this extension. The array is sorted by name
     and borrowed through the entry point's return. It is null when the count is zero. */
  const ycxx_rule_capability_v1* capabilities;
  size_t capability_count;

  /* Check struct_size before accessing this callback. Supply one nonempty diagnostic of
     at most 4096 bytes. The worker copies it and reports it when the entry point fails.
     Return 0 on success or -1 on an invalid, repeated or oversized diagnostic. */
  int (*diagnose)(void* context, const char* text, size_t size);
} ycxx_rule_host_v1;

/* Call the host from this thread before returning. Return 0 after successfully emitting
   the definition, or a nonzero rule error. */
int ycxx_rule_plan_v1(const ycxx_rule_host_v1* host);

#ifdef __cplusplus
}
#endif

#endif
