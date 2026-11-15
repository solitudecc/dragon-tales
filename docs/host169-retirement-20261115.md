# Historical host 169 project migration (2026-11-15)

The owner asked whether `cc@192.168.10.169` could be deleted. Current framework
development and both build variants use `ubuntu@192.168.10.241`, checkout
`/home/ubuntu/dragon-tales-rock5b`. No framework build or source reset was
performed on 169 during this migration. Nothing was deleted from 169.

## Scope and status

The full project archive passed independent verification on Mac and ROCK. The
project catalog covers all 12 Git repositories, including dirty
and untracked files and `.git`, their builds and validation outputs, `bench`,
`vmc-runs`, Unicorn source/environments, and the selected project root files.
It is not a backup of unrelated personal directories or an operating-system
image. The manifest records the exact selected roots; no special entries were
excluded within them.

All three archives below, including the temporary supplement, passed on both hosts.
`DUAL-HOST-CERTIFICATE.json` records project retirement readiness, the exact
archive hashes and the hashes of the verification/source/generator receipts;
its SHA-256 is
`4915f7181ff632fbcd8daf0bca65f9e53321b5b8739bc4e2118688b042a31f66`.
The project no longer requires 169 to remain running. The owner may remove it
after separately accounting for any unrelated personal/system data. No machine
shutdown or deletion was performed by this migration.

The catalog contains 750,154 entries and 98,902,291,675 logical regular-file
bytes, or 96,832,243,724 bytes counting each original inode once. Content
deduplication yields 126,564 blobs / 39,106,632,547 bytes. Original hardlink
groups remain separate metadata; equal content alone does not imply a hardlink.
The catalog SHA-256 is
`e9fc362b8dfe0391fe281aaf2ccf64367d41622c433a661e1905378c39daff8e`.

## Locations

- Mac: `/Users/c/Desktop/deob/validation-artifacts/host169-retirement-20261115-v1/`
- ROCK: `/home/ubuntu/retired-host169-20261115-v1/`
- Full content archive: `full-project-cas.tar.zst`, 25,110,685,894 bytes,
  SHA-256 `d1b0871833ba5c83cd5a9e094808e712f790bff167a7ae69642db1d1eaf99730`.
  Both hosts verify all 126,565 archive members, all 126,564 blob hashes, all
  693,334 regular-file mappings, 56,722 directories, 98 symlinks and 1,997
  original hardlink groups. Zstd EOF and zero tar trailing padding pass.
  Source pre/post metadata remains equal across all 750,154 captured entries.
- Separate critical source/tool archive: `source-tools-critical.tar.zst`,
  585,741,529 bytes, SHA-256
  `93bbb71bec6fcf8eea166cc384f3ab2127726ffc126944c831ad7e71c6952d87`.
  It passed independent Mac/ROCK checks, but alone excludes historical builds
  and validation outputs and is insufficient for retirement.
  On ROCK this separate copy is under `critical/`.
- Temporary-project supplement: `tmp-supplement/source-tools-critical.tar.zst`,
  135,378,764 bytes, SHA-256
  `05943c26e9b02ce4ad12c7fc1e24a76b02dd5f704f7ac8fe20e950d983888e01`.
  It captures 1,389 regular-file/symlink entries (484,698,678 logical bytes)
  from nonhidden cc-owned `/tmp` project/test files and subdirectories. System
  IPC, root-owned/dot entries and the new migration catalog work directory are
  excluded explicitly. No additional top-level Git worktree was found there. Empty
  temporary directories are not included. Its contents are rooted at `/tmp`,
  separately from the full project's home-relative paths.

The interrupted first transport is retained in its logs and failure receipt.
The second transport verifies the exact compressed prefix before appending;
the extra local first-attempt tail is preserved separately. No failed attempt
is relabeled as a successful full backup.

## Tigress on ROCK

Use `/home/ubuntu/retired-host169-20261115-v1/run-tigress`. The original x86-64
tool is preserved and the restored launcher uses installed `qemu-x86_64` with
the x86-64 cross GCC. The generation smoke test uses the original large
combined sample recipe: entropy initialization, array opaque predicates,
EncodeArithmetic and split/randomized Flatten. It exits 0 and generates C
byte-identical to the original reference (SHA-256
`3fc6107fc623211ba2595484a650d75f976584840375b7cbe35811e4576090cf`).
This is generator migration validation, not a new deobfuscation result.

## Verify and restore

The backup directory contains the exact migration helper sources, receipts and
a synthetic helper self-test. That test accepts a valid archive, rejects a
changed blob and an extra member, and restores contents, symlink targets and
original hardlink identity while keeping separate equal-content files separate.

Run the following with that host's completed `VERIFIED`
receipt and an empty new destination:

```sh
python3 migration-helpers/verify_cas.py full-project-cas.tar.zst CAS-READY.json VERIFIED.json
python3 migration-helpers/restore_cas.py full-project-cas.tar.zst CAS-READY.json VERIFIED.json /new/empty/destination
```

The existing main receipts are `MAC-FULL-VERIFIED.json` and
`ROCK-FULL-VERIFIED.json`; `VERIFIED.json` above is a new check's output. The
temporary supplement uses the separate `migration-helpers/verify_supplement.py`
format and can be extracted as an ordinary tar/zstd archive into a new empty
temporary-project directory after verification.

Restoration needs about 97 GB for original unique-inode data plus directory
overhead. File contents, modes, modification times, symlink text and original
hardlinks are recorded. Ownership, ACLs, xattrs and original ctime/inode numbers
are not captured; absolute symlink targets are preserved, not rewritten.
Old binaries remain historical evidence and are not substituted for the current
ROCK builds across glibc environments. No new target/native/Unicorn/corpus run
is claimed by archive verification.
