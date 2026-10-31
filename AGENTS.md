# Contributor instructions

This repository publishes the validated Portrait of Ruin implementation and an
experimental Dawn of Sorrow profile (v0.2.0). Do not assume another
title, game fingerprint, display layout, or ABI is supported.
Read README.md and docs/ARCHITECTURE.md before changing runtime behavior.

Never commit game binaries/data, saves, snapshots, runtime logs, captures, crash
dumps, compiled artifacts, credentials, machine-specific paths, or account IDs.
Keep source-only audits independent of the ignore rules. Preserve third-party
license files and attribution; LICENSE applies only to project-owned material.

Builds and synthetic fixtures belong under out/. Normal build/test commands must
not install, launch, attach to, or modify a game. Gameplay and installation require
explicit user authorization and separate integrity checks. Never restore user
progress automatically. Keep a validated installed version separate from source
work and experimental builds. See docs/BUILD.md and docs/VALIDATION.md.
