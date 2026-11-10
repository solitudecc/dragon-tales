#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern int32_t phi_payload(bool);
extern int32_t load_phi_payload(bool);
extern uint64_t mixed_state(bool, uint64_t);
extern uint64_t load_mixed_state(bool, uint64_t);

int main(int argc, char **argv) {
  const bool control = argc > 1 && strcmp(argv[1], "control") == 0;
  size_t comparisons = 0, mismatches = 0;
  for (unsigned left = 0; left != 2; ++left) {
    const int32_t expected = left ? 10 : 20;
    const int32_t actual[] = {phi_payload(left), load_phi_payload(left)};
    const char *names[] = {"phi_payload", "load_phi_payload"};
    for (unsigned which = 0; which != 2; ++which) {
      const int32_t checked = actual[which] ^ (control && !left && !which);
      ++comparisons;
      const bool match = checked == expected;
      mismatches += !match;
      printf("%s left=%u actual=%" PRId32 " expected=%" PRId32 " match=%u\n",
             names[which], left, checked, expected, match);
    }
  }
  const uint64_t values[] = {
      0, 1, 2, 3, 10, 20, UINT64_C(0x7fffffffffffffff),
      UINT64_C(0x8000000000000000), UINT64_MAX};
  for (unsigned known = 0; known != 2; ++known) {
    for (unsigned index = 0; index != sizeof(values) / sizeof(values[0]); ++index) {
      const uint64_t value = known ? 1 : values[index];
      const uint64_t expected = value == 1 ? 11 : (value == 2 ? 22 : 33);
      const uint64_t actual[] = {
          mixed_state(known, values[index]), load_mixed_state(known, values[index])};
      const char *names[] = {"mixed_state", "load_mixed_state"};
      for (unsigned which = 0; which != 2; ++which) {
        ++comparisons;
        const bool match = actual[which] == expected;
        mismatches += !match;
        printf("%s known=%u unknown=%" PRIu64 " actual=%" PRIu64
               " expected=%" PRIu64 " match=%u\n",
               names[which], known, values[index], actual[which], expected, match);
      }
    }
  }
  printf("comparisons=%zu mismatches=%zu skips=0 control=%u\n",
         comparisons, mismatches, control);
  return mismatches ? 1 : 0;
}
