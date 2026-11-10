#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint64_t (*Function)(bool, bool, uint64_t, uint64_t *, uint64_t *);
#define DECLARE(name) extern uint64_t name(bool, bool, uint64_t, uint64_t *, uint64_t *)
DECLARE(select_freeze_phi); DECLARE(select_noundef_phi);
DECLARE(select_default_phi); DECLARE(select_same_phi); DECLARE(select_guard_phi);
DECLARE(select_freeze_load); DECLARE(select_noundef_load);
DECLARE(select_default_load); DECLARE(select_same_load); DECLARE(select_guard_load);
DECLARE(gap_compute); DECLARE(gap_load); DECLARE(gap_readonly);
DECLARE(call_then_store); DECLARE(store_then_call); DECLARE(select_loop);
#undef DECLARE

static uint32_t calls;
void select_unknown_writer(uint64_t *state, uint64_t *side, uint64_t x) {
  ++calls;
  state[0] = 3;
  state[1] ^= 0x55;
  side[0] ^= x;
}

// This body has no writes, matching the declaration visible during the pass.
uint64_t select_readonly_reader(const uint64_t *state, uint64_t x) {
  return state[0] ^ x ^ 0xa5;
}

struct Fixture { const char *name; Function function; };
#define FIXTURE(name) {#name, name}
static const struct Fixture fixtures[] = {
  FIXTURE(select_freeze_phi), FIXTURE(select_noundef_phi),
  FIXTURE(select_default_phi), FIXTURE(select_same_phi), FIXTURE(select_guard_phi),
  FIXTURE(select_freeze_load), FIXTURE(select_noundef_load),
  FIXTURE(select_default_load), FIXTURE(select_same_load), FIXTURE(select_guard_load),
  FIXTURE(gap_compute), FIXTURE(gap_load), FIXTURE(gap_readonly),
  FIXTURE(call_then_store), FIXTURE(store_then_call), FIXTURE(select_loop)
};
#undef FIXTURE

int main(int argc, char **argv) {
  const bool control = argc > 1 && !strcmp(argv[1], "control");
  const uint64_t inputs[] = {0, 1, UINT64_C(0x8000000000000000), UINT64_MAX};
  unsigned comparisons = 0, mismatches = 0;
  for (unsigned f = 0; f != sizeof(fixtures) / sizeof(fixtures[0]); ++f) {
    const char *name = fixtures[f].name;
    for (unsigned condition = 0; condition != 2; ++condition) {
      for (unsigned guard = 0; guard != 2; ++guard) {
        for (unsigned i = 0; i != sizeof(inputs) / sizeof(inputs[0]); ++i) {
          const uint64_t x = inputs[i];
          uint64_t state[] = {9, 7}, side[] = {x ^ 0x8080, 0x12345678};
          uint64_t wantedState[] = {9, 7}, wantedSide[] = {x ^ 0x8080, 0x12345678};
          uint64_t expected = x + (condition ? 11 : 22);
          uint32_t wantedCalls = 0;
          const uint64_t selected = condition ? 1 : 2;
          const bool memory = strstr(name, "_load") != NULL;
          if (memory) wantedState[0] = selected;
          if (strstr(name, "select_default")) {
            if (condition) {
              expected = x + 33;
              if (memory) wantedState[0] = 3;
            }
          } else if (strstr(name, "select_same")) expected = x + 11;
          else if (strstr(name, "select_guard") && !guard) expected = x + 44;
          else if (!strcmp(name, "gap_compute")) {
            wantedState[0] = 1;
            expected = x + 7;
          } else if (!strcmp(name, "gap_load")) {
            wantedState[0] = 1;
            expected = (guard ? 1 : wantedSide[0]) ^ x;
          } else if (!strcmp(name, "gap_readonly")) {
            wantedState[0] = 1;
            expected = 1 ^ x ^ 0xa5;
          } else if (!strcmp(name, "call_then_store") || !strcmp(name, "store_then_call")) {
            wantedCalls = 1;
            wantedState[0] = !strcmp(name, "call_then_store") ? selected : 3;
            wantedState[1] ^= 0x55;
            wantedSide[0] ^= x;
            expected = x + (wantedState[0] == 1 ? 11 : wantedState[0] == 2 ? 22 : 33);
          } else if (!strcmp(name, "select_loop")) {
            wantedState[0] = 2;
            wantedState[1] = x & 3;
            if (!wantedState[1]) wantedState[1] = 1;
            expected = wantedState[1];
          }
          uint64_t *sideArgument = !strcmp(name, "gap_load") && guard ? state : side;
          calls = 0;
          uint64_t actual = fixtures[f].function(condition, guard, x, state, sideArgument);
          if (control && !f && !condition && !guard && !i) actual ^= 1;
          bool match = actual == expected && calls == wantedCalls &&
              !memcmp(state, wantedState, sizeof state) && !memcmp(side, wantedSide, sizeof side);
          ++comparisons;
          mismatches += !match;
          printf("%s c=%u g=%u x=%" PRIu64 " result=%" PRIu64
                 " state0=%" PRIu64 " state1=%" PRIu64 " side0=%" PRIu64
                 " side1=%" PRIu64 " calls=%u expected=%" PRIu64 " match=%u\n",
                 name, condition, guard, x, actual, state[0], state[1], side[0],
                 side[1], calls, expected, match);
        }
      }
    }
  }
  printf("comparisons=%u mismatches=%u skips=0 control=%u\n", comparisons, mismatches, control);
  return mismatches ? 1 : 0;
}
