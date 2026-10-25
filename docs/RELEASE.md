# Releases

## v0.1.1

The v0.1.1 annotated tag marks the merge of the title-isolation and
gameplay-only map changes, validated in game as internal rc7 (VALIDATION.md).
The GitHub release carries notes only: no precompiled binaries are attached or
committed. End-user packages are built locally with `scripts/Package-Release.ps1`
from the validated build and distributed separately; their DLL SHA-256 is
recorded in VALIDATION.md.

## v0.1.0

The v0.1.0 annotated tag identifies the initial source commit based on the
validated internal rc5 implementation, with no GitHub release or binaries.
Internal rc3/rc4/rc5/rc6/rc7 archives remain local references.

## Before publishing

Review the exact index/tree, verify licenses, scan all blobs for private content
and proprietary artifacts, run the source-only checks, and ensure the history
contains only the intended commits. Do not rewrite history, force-push, or upload
private diagnostics. Changes reach `main` through a pull request.

For continued development, work in this checkout and a separate out/ folder.
Nothing changes an installed mod until a distinct, explicitly authorized install
step. Keep validated builds and progress backups outside Git and the Steam folder.
See BUILD.md, INSTALL.md, VALIDATION.md and PUBLICATION.md.
