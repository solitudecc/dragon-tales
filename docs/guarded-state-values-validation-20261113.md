# State-value guards: implementation milestone

Historical pre-freeze milestone. The later undef-condition fix is recorded in
`guarded-state-definedness-validation-20261113.md`. Earlier native fixtures use
fully defined data and do not cover this LLVM deferred-UB boundary.

The explicit `guarded-state-values` builtin checks an existing SSA state after
all seed effects. A matched untrusted literal chooses a proved original ICmp
direction; mismatch retains the original conditional branch. The copied tail
keeps its actual target read and same-value switch fallback. It adds no counter
range, memory, calling-convention or table-content fact. Every committed rewrite
reports Incomplete. See `llvm-guarded-state-values.md` for the contract.

Frozen build: `f33d36ac4f75baeb039ad5a1a28ee471a7ba4059` plus patch
`228c0ebbb6f44f7314722fbecc5920f00273018d9238d2729ca158e63742d8a7`.
The state-values source is
`562a9ab89d301045522eb968f13ca558c04bd4d1ee2ec0a5bc7940fd8b6148bc`;
regression source is
`f3eeeabbe823319128440fd2cf0cdb048cbe760da8f636355a70ad4fe055e9d4`.
The L2 controller, guarded-transition core and original J test remain unchanged.
Independent source review receipt is
`cdc5c3b6806fd6b3075616bd68e6b6ba8d8fdb46873833b5fc9356289523aa2f`.
Source review is not semantic validation.

Final tests compile on both preceding J libraries. Each executes four cases /
nine assertions and fails all four cases / five assertions because the builtin
is absent. This is an API-absence regression, not old wrong execution. New
related focus passes 74 cases / 3213 assertions in both builds. Full CTest passes
1139/1139 with Z3 (118.36 s) and 1004/1004 without Z3 (107.33 s).

Both runtimes keep exact old J41/I245/H94 IR products and reports, plus K's
previously native-executed IR. No prior native observations are recounted.
The 41 specified J byte-corruption controls remain detected.

Frozen backend SHA-256:

- main: `a2f82bdd378c72e42b57b1e2fcb9e58317b106531d385492f93ab47230df5ef5`
- no-Z3: `b4fae0a37b04ab2e9a8bab3ec4841751076f3dbaa9d93a2cb1654b19da203a3d`

Source/old failures/build/full/focus/compatibility evidence is under
`~/vmc-runs/guarded-state-values-20261113/`. Metadata archive SHA-256 is
`b4c24aa1d6f0fb93076bb7346251bb444c1a042ce63f2908a8f9c8d14a0ef714`:
860 payloads independently checked on macOS for exact membership, length and
hash. Local archive:
`/Users/c/Desktop/deob/validation-artifacts/guarded-state-values-20261113/guarded-state-values-metadata-v1.tar.gz`.
Raw runtimes are retained separately.

Independent full-state native results are now recorded in
`guarded-state-values-native-validation-20261113.md`; real four-module structure
checks and the final composed replay remain pending. This milestone does not establish
complete loop de-flattening; final composed replay and observed-loop evidence
remain required before main merge. No new machine-instruction, Unicorn or
69-input corpus campaign is claimed.

Standalone validation drivers link the frozen backend with `-ldragon -pthread`
and the LLVM include directory. Do not additionally link `llvm-config --libs`:
the backend already contains LLVM; a second dynamic copy can abort before main
through duplicate command-line registration or allocator state. Preserve such
infrastructure failures separately and rerun without changing model or gates.
