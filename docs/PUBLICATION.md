# Source publication audit — v0.1.0

The source publication was prepared in a separate checkout from the development
workspace. Runtime/header/test files were copied byte-for-byte from the rc5
working tree, whose documented changes, timestamps and reference build match the
validated implementation. SRC-SHA256.json records these source-file identities.
No claim is made that a rebuild in another path has an identical binary hash.

Original development trees, installed rc5, snapshots and prior-version backups
are not edited by publication. Full local inventories and copy/change records
are kept outside this repository. The remote must remain private. Only main and
an annotated v0.1.0 tag are pushed; no binary, GitHub Release or Nexus publication.

## Included

Project C++/headers, synthetic tests, a static PE inspector/profile, build and
non-destructive installer/package scripts, example settings, reviewed maintenance
documentation, MinHook/HDE, the optional RenderDoc API header, and license notices.

## Excluded

All compiled artifacts, game binaries/assets, saves, snapshots, runtime logs,
image/capture dumps, crash reports, toolchain downloads, Python caches, account
identifiers, personal machine paths, credentials, local handoffs and old source
backups. Files are selected explicitly and audited, not merely ignored by Git.

## Publication-only changes

README, INSTALL, BUILD, VALIDATION, COMPATIBILITY and RELEASE documentation were
rewritten to remove local environment/history details and stale development claims.
ARCHITECTURE summarizes current implementation without private runtime evidence.
End-user README/CHANGELOG and THIRD-PARTY-NOTICES were aligned with v0.1.0.
Build defaults/caches and installer-fixture paths are checkout-relative. CMake now
links the existing marker/provider sources and Steam-pad test. Runtime behavior is
unchanged. Project LICENSE uses the requested author notice; third-party license
files are retained verbatim and are not relicensed.

The commit/index and every reachable blob must be scanned before push, including
binary magic, denied extensions, absolute personal paths, user identifiers and
credential patterns. Runtime fingerprints, native RVAs, public Steam AppID and
upstream license authors are expected technical/provenance data, not personal
account data. Recheck remote privacy and absence of prior history immediately
before a non-forced push. Keep local-only audit reports outside Git.

Third-party files are retained byte-for-byte, including upstream whitespace.
Whitespace lint is applied to project-owned changes; an existing trailing space
in the MinGW license is preserved rather than rewriting the license text.
