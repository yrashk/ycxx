#include "generated_config.hh"
#include "schema.hh"

int main() {
  return schema_score() == generated_score ? generated_score : 1;
}
