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
#define YCXX_RULE_FACT_U32_V1 1u
#define YCXX_RULE_FACT_TEXT_V1 2u

typedef struct ycxx_rule_bytes_v1 {
  const char* data;
  size_t size;
} ycxx_rule_bytes_v1;

typedef struct ycxx_rule_capability_v1 {
  const char* name;
  uint32_t support;
} ycxx_rule_capability_v1;

typedef struct ycxx_rule_fact_field_v1 {
  const char* name;
  uint32_t kind;
  /* Set the inactive member to zero. Text is UTF-8 and uses its byte count. */
  uint32_t number;
  ycxx_rule_bytes_v1 text;
} ycxx_rule_fact_field_v1;

typedef struct ycxx_rule_fact_row_v1 {
  const char* name;
  const ycxx_rule_fact_field_v1* fields;
  size_t field_count;
} ycxx_rule_fact_row_v1;

typedef struct ycxx_rule_fact_v1 {
  const char* name;
  const char* schema;
  const ycxx_rule_fact_row_v1* rows;
  size_t row_count;
} ycxx_rule_fact_v1;

typedef struct ycxx_rule_witness_v1 {
  const char* name;
  int exit_status;
  ycxx_rule_bytes_v1 out;
  ycxx_rule_bytes_v1 err;
  /* Check that the host includes export_fact before reading this member.
     Null identifies a support-state witness. */
  const char* schema;
} ycxx_rule_witness_v1;

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

  /* Check struct_size before accessing this extension. Only the classifier receives a
     witness. It describes a normal exit and borrows up to 64 KiB of each stream through
     the entry point's return. The streams may contain arbitrary bytes. */
  const ycxx_rule_witness_v1* witness;

  /* Only the classifier receives this callback. Supply one YCXX_RULE_*_V1 support state
     for witness->name. Return 0 on success or -1 on an invalid or repeated export. */
  int (*export_support)(void* context, const char* name, uint32_t support);

  /* Check struct_size before accessing the fact extension. Facts, rows and fields
     are sorted by name and borrowed through the entry point's return. The host
     accepts up to 64 fact bindings with a 256 KiB combined budget. */
  const ycxx_rule_fact_v1* facts;
  size_t fact_count;

  /* Only a classifier with a schema receives this callback. Supply one table for
     the witness's name and schema. The worker copies and sorts its rows and fields.
     Names and schema are nonempty UTF-8 strings of at most 255 bytes. A table can
     have up to 256 rows and 32 fields per row. Each text field can contain up to
     4096 bytes. The table has a 64 KiB budget for its schema, names and values.
     Return 0 on success or -1 on an invalid, repeated or oversized export. */
  int (*export_fact)(void* context, const ycxx_rule_fact_v1* fact);
} ycxx_rule_host_v1;

/* Call the host from this thread before returning. Return 0 after successfully emitting
   the definition, or a nonzero rule error. */
int ycxx_rule_plan_v1(const ycxx_rule_host_v1* host);

/* Optional. Call export_support or export_fact as appropriate for the witness from
   this thread and return 0 to classify it. Return a nonzero rule error on failure.
   Only the planner may call emit. */
int ycxx_rule_classify_v1(const ycxx_rule_host_v1* host);

#ifdef __cplusplus
}
#endif

#endif
