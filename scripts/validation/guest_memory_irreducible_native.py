#!/usr/bin/env python3
"""Compare native before/after IR with an independent finite-memory model.

Build guest_memory_irreducible_native.cpp against the library under test, run
it in OUT, then lower before.ll and after.ll using llc-21 -filetype=obj
-relocation-model=pic. Link each object with the helper below into before.so /
after.so. Run this script with OUT. The .so compilation must not run further
LLVM optimizations: this oracle tests the actual GuestMemorySSA output.

Helper C source for linking both objects:
  #include <stdint.h>
  #include <string.h>
  void unknown_writer(unsigned char *p) {
    uint64_t v; memcpy(&v, p + 8, 8); v ^= UINT64_C(0xff00ff00ff00ff00);
    memcpy(p + 8, &v, 8);
  }
"""

import ctypes
import hashlib
import json
from pathlib import Path
import struct
import sys

MODES = (
    "exact", "narrow", "unknown_call", "unknown_alias", "fence", "atomic_write",
    "volatile_load", "volatile_store", "partial", "other_guest_base", "cross_block",
)
COUNTS = (0, 1, 2, 7, 8, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF)
PATTERNS = ("zero", "ones", "affine")
MEMORY_BYTES = 65536
MASK64 = (1 << 64) - 1


def initial_memory(pattern):
    if pattern == "zero":
        return bytearray(MEMORY_BYTES)
    if pattern == "ones":
        return bytearray([255]) * MEMORY_BYTES
    return bytearray((index * 131 + 17) & 255 for index in range(MEMORY_BYTES))


def expected(mode, choice, count, memory):
    """Execute source-level steps; do not derive a value from rewritten IR."""
    side = 0 if choice else 1
    remaining = count & 7
    while True:
        value = (0x1122334455667788, 0x8877665544332211)[side]
        if mode == "partial":
            memory[8] = value & 255
        elif mode != "cross_block" or side == 0:
            struct.pack_into("<Q", memory, 8, value)
        if mode == "unknown_call":
            current = struct.unpack_from("<Q", memory, 8)[0]
            struct.pack_into("<Q", memory, 8, current ^ 0xFF00FF00FF00FF00)
        elif mode == "unknown_alias":
            struct.pack_into("<Q", memory, 8, 3)
        elif mode == "atomic_write":
            current = struct.unpack_from("<Q", memory, 8)[0]
            struct.pack_into("<Q", memory, 8, (current + 1) & MASK64)
        elif mode == "other_guest_base":
            # Both separately loaded Guest bases alias the same runtime bytes.
            struct.pack_into("<Q", memory, 8, 0)
        if mode in ("narrow", "other_guest_base"):
            result = struct.unpack_from("<H", memory, 11)[0]
        elif mode == "cross_block" and side == 0:
            result = value
        else:
            result = struct.unpack_from("<Q", memory, 8)[0]
        if remaining == 0:
            return result, bytes(memory)
        remaining -= 1
        side ^= 1


def compare(actual, wanted):
    return actual[0] == wanted[0] and actual[1] == wanted[1]


def run(function, memory, choice, count):
    array = (ctypes.c_ubyte * MEMORY_BYTES).from_buffer_copy(memory)
    pointer = ctypes.addressof(array)
    descriptor = ctypes.c_void_p(pointer)
    result = int(function(pointer + 8, ctypes.byref(descriptor), choice, count))
    if descriptor.value != pointer:
        raise AssertionError("native function changed the Guest descriptor")
    return result, bytes(array)


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: guest_memory_irreducible_native.py OUT")
    out = Path(sys.argv[1]).resolve()
    libraries = {name: ctypes.CDLL(str(out / (name + ".so")))
                 for name in ("before", "after")}
    rows = []
    counts = {"inputs": 0, "native_matches": 0, "differences": 0,
              "result_controls_detected": 0, "memory_controls_detected": 0,
              "skipped": 0, "excluded": 0, "normalized": 0}
    for mode in MODES:
        functions = {name: getattr(library, mode) for name, library in libraries.items()}
        for function in functions.values():
            function.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_bool,
                                 ctypes.c_uint32]
            function.restype = ctypes.c_uint64
        for pattern in PATTERNS:
            for choice in (False, True):
                for count in COUNTS:
                    memory = initial_memory(pattern)
                    wanted = expected(mode, choice, count, memory.copy())
                    snapshots = {name: run(function, memory, choice, count)
                                 for name, function in functions.items()}
                    counts["inputs"] += 1
                    for name, snapshot in snapshots.items():
                        matched = compare(snapshot, wanted)
                        counts["native_matches" if matched else "differences"] += 1
                        # Mutate exactly one output field and prove the full
                        # comparator rejects it; do not change normal inputs.
                        result_control = (snapshot[0] ^ 1, snapshot[1])
                        memory_control = bytearray(snapshot[1])
                        memory_control[MEMORY_BYTES - 1] ^= 1
                        if not compare(result_control, wanted) and matched:
                            counts["result_controls_detected"] += 1
                        if not compare((snapshot[0], bytes(memory_control)), wanted) and matched:
                            counts["memory_controls_detected"] += 1
                        rows.append({"mode": mode, "pattern": pattern,
                                     "choice": choice, "count": count, "side": name,
                                     "matched": matched, "result": snapshot[0],
                                     "expected_result": wanted[0],
                                     "memory_sha256": hashlib.sha256(snapshot[1]).hexdigest(),
                                     "expected_memory_sha256": hashlib.sha256(wanted[1]).hexdigest()})
    (out / "native-records.json").write_text(json.dumps(rows, indent=2) + "\n")
    (out / "native-summary.json").write_text(json.dumps(counts, indent=2) + "\n")
    print(json.dumps(counts, sort_keys=True))
    wanted_matches = counts["inputs"] * 2
    return int(counts["differences"] != 0 or
               counts["native_matches"] != wanted_matches or
               counts["result_controls_detected"] != wanted_matches or
               counts["memory_controls_detected"] != wanted_matches)


if __name__ == "__main__":
    raise SystemExit(main())
