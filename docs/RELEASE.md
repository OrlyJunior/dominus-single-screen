# v0.1.0 source publication

The v0.1.0 annotated Git tag identifies the initial source commit based on the
validated internal rc5 implementation. It is not a GitHub Release and contains
no precompiled binaries. Internal rc3/rc4/rc5 archives remain local references.

Before committing/pushing: review the exact index/tree, verify licenses, scan
all blobs for private content/proprietary artifacts, run available source-only
checks, and ensure origin is the intended private repository with no conflicting
history. Do not rewrite history, force-push, or upload private diagnostics.

For continued development, work in this checkout and a separate out/build folder.
Nothing changes an installed mod until a distinct, explicitly authorized install
step. Keep validated builds and progress backups outside Git and the Steam folder.
See BUILD.md, INSTALL.md, VALIDATION.md and PUBLICATION.md.
