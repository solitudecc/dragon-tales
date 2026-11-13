# New bounded business function: ordinary-lift recipe A

This is a first practical batch for the owner-approved reusable-framework /
sample-pass boundary. It introduces a new bounded unsigned business function,
not another seed of the old Tigress kernel. Its plain and hand-obfuscated C
are 61 and 77 lines. Eight words, four rounds, at most sixteen inner updates,
real input-dependent branches, rotations and byte writes produce a return
value and sixteen output bytes. An independent Python word/bit-string model
specifies those public results. The manual variant adds MBA identities, two
algebraic opaque conditions and an encoded ordinary switch state machine.
Its normal path needs at most 62 of its fixed 64 dispatcher iterations. No
heap, VM, unknown external implementation or source-specific core rule is used.

The normal input freeze contains plain, manual, Tigress 2.1 arithmetic and
Tigress switch-flattened variants at C O0/O2: eight ELF images and sixteen
fixed boundary inputs. Its manifest SHA256 is
`4ae488b0b0c0a236f077ce2bbb7d4ec7cdba40fca719ac2b0713bee3ca0cb0f7`.
All source/ELF bytes are frozen before the first LLVM replay. An additional
wrong-return C freeze is separate; it never changes the normal ELF bytes.

The unchanged complete function oracle compares each ELF with its own Unicorn
execution, including all 76 modelled register fields, all 65536 Guest bytes,
call observations and seven Exit fields. Cross-ELF comparison uses only the
independent public C specification. Four LLVM plans use FunctionStart static
views: raw/static O0/O2 and product O0/O2. The product recipe is explicitly
`mem2reg, instcombine, gvn, guest-memory, mba, predicates, dispatch-specialize,
dispatch-deflatten, control-flow, dce`, one iteration and sixteen maximum
invocations. Solver budgets, one-million-Guest-instruction limit and
180-second child limits are declared before execution. Native execution must
actually reach the root RET. No comparison field or old gate is relaxed.

## First measured outcomes

| family | independent C checks | LLVM attempts | complete matches | Incomplete |
| --- | ---: | ---: | ---: | ---: |
| plain | 32/32 | 128 | 128 | 0 |
| manual combination | 32/32 | 128 | 0 | 128 |
| Tigress arithmetic | 32/32 | 128 | 128 | 0 |
| Tigress switch flatten | 32/32 | 128 | 0 | 128 |

All eight images have actual entry checks; none reports an unlifted instruction.
The 256 incomplete rows exit with actual Branch Transfer before the first
uncovered switch target. Their complete final register/memory states are not
compared and are not counted as semantic passes. The initial IR has no
available local case switch for these four machine jump sites. Ordinary
lifting did not publish usable targets belonging to the function. This does
not demonstrate that every optional framework component fails on the images.

Three independent controls passed: wrong C return has exactly the specified
one-bit return difference on 32 inputs with all output bytes equal; its 128
LLVM-own-Unicorn comparisons still match. Register and memory corruptions
each produce 128 complete observations with exactly one specified field/byte
difference, and each keeps 32 matching C checks. The register XOR is one;
the sole memory byte is Guest `0x8000`, also XOR one. No retries or exclusions.
Normal original-C calibration is 128 actual checks / 32 distinct ELF-input
pairs; the three controls add 96 / 64. Across both, the exact union is 64
ELF-input pairs over the same sixteen business inputs, not 224 distinct inputs.

## Precision and cost are separate from correctness

There are measured MBA rewrites, including the manual add/xor helpers and
Tigress arithmetic (27 and 8 reported simplifications at C O0/O2). The
independent report retains exact before/after helper IR and source hashes.
Only the entire pipeline is credited: no per-MBA intermediate frame was saved.
Predicate fold counters include other generated conditions; they do not prove
that the source opaque predicates disappeared. Guest-memory forwarding and
dispatcher rewriting are zero in this recipe. Every raw switch is in an
exact `entry` block; deleting those entry switches is not CFF removal.

With equal original unoptimized bytes, final stock LLVM O2 after the product
recipe has **more** textual IR operations than stock LLVM O2 alone in all
eight image pairs:

| family | C O0: static / product | C O2: static / product |
| --- | ---: | ---: |
| plain | 11647 / 11680 | 7065 / 7128 |
| manual | 6126 / 6169 | 2714 / 2769 |
| arithmetic | 21485 / 21790 | 8497 / 8546 |
| flatten | 1670 / 1719 | 1138 / 1178 |

These counts do not measure complete semantic cones or analyst time. No
general readability, speedup, complete CFF or superiority-to-LLVM claim follows.
Decode/lift/pass, host optimization, compilation, Unicorn and native timings
are recorded separately. Unknown queries being zero does not establish query
coverage. Eighty original IR products were independently recounted with 108
input pins unchanged; the report and checker are retained under `A-independent`.

## Scope and next comparison

Recipe A uses ordinary function lifting and only the declared ten-step LLVM
recipe. A separate B configuration is being prepared using the already
implemented explicit decoded-region lift, unchanged actual Guest loads and
unknown exits, identical input ELF bytes and the same oracle/pipeline. It
requires unique executable symbol extents for this test. B is not a new core
repair, a new unseen sample, an image-immutability premise or an assertion of
full de-flattening. A's original failures remain. No B result is claimed here.

The final A evidence seal is separate from this implementation checkpoint.
No new production library is built; existing CTest/native campaigns are not
rerun or re-counted. There is no main merge or new instruction/corpus campaign.
