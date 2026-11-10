#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

extern uint64_t postcondition_probe(uint64_t *);
extern uint64_t distinct_bases_probe(void *);
extern uint64_t nonentry_probe(uint64_t *);

int main(void) {
  unsigned comparisons = 0, mismatches = 0;
  const uint64_t values[] = {1, 42};
  for (unsigned i = 0; i != 2; ++i) {
    uint64_t slot = values[i];
    uint64_t actual = postcondition_probe(&slot);
    uint64_t expected = values[i] == 42 ? 99 : 17;
    unsigned match = actual == expected && slot == 42;
    ++comparisons;
    mismatches += !match;
    printf("postcondition input=%" PRIu64 " actual=%" PRIu64
           " expected=%" PRIu64 " slot=%" PRIu64 " expected_slot=42 match=%u\n",
           values[i], actual, expected, slot, match);
  }
  for (uint64_t input = 1; input != 3; ++input) {
    uint64_t a[] = {0, input}, b[] = {0, 9};
    struct { uint64_t *a; uint64_t *b; } outer = {a, b};
    uint64_t actual = distinct_bases_probe(&outer);
    uint64_t expected = input == 1 ? 111 : 222;
    unsigned match = actual == expected && a[1] == input && b[1] == 1;
    ++comparisons;
    mismatches += !match;
    printf("distinct_bases input=%" PRIu64 " actual=%" PRIu64
           " expected=%" PRIu64 " a=%" PRIu64 " b=%" PRIu64 " match=%u\n",
           input, actual, expected, a[1], b[1], match);
  }
  for (unsigned input = 0; input != 2; ++input) {
    uint64_t slot = input ? UINT64_MAX : 0;
    uint64_t actual = nonentry_probe(&slot);
    unsigned match = actual == 222 && slot == 2;
    ++comparisons;
    mismatches += !match;
    printf("nonentry input=%u actual=%" PRIu64
           " expected=222 slot=%" PRIu64 " expected_slot=2 match=%u\n",
           input, actual, slot, match);
  }
  printf("comparisons=%u mismatches=%u skips=0\n", comparisons, mismatches);
  return mismatches ? 1 : 0;
}
