# Compatibility and static inspection

Supported executable: Steam/Windows x64 collection build identified by SHA-256
`E6FA885F560C8A47640FF8DF4DE3CE4A938C8053EB11A90A7B65F5EBF1AB6B64`.
Portrait position provider requires dra02.dll SHA-256
`FFEFA1109A43FA7A019A80EE16C6F287884302C8CBB921D361F595E730C38744`.
These fingerprints are identification data, not game assets or a redistribution
of the binaries. A changed game build requires new investigation and validation.

`profiles/portrait-20281557.json` belongs to the standalone static PE inspector.
Its runtime_hooks_enabled=false/status fields deliberately do not authorize
runtime integration. The runtime performs independent fingerprint/ABI checks.

```powershell
python -B tools/inspect_build.py $localExecutable
```

The inspector reads only the explicit local file, uses the Python standard
library and emits JSON. It does not execute or load that file, attach to a process,
or install hooks. Exit0 means the static profile matched,2 means mismatch,1 means
an inspection error. Three static identification signatures were consulted from
MGSM2Fix under MIT; the exact revision and attribution are in the profile and
THIRD-PARTY-NOTICES.md. No MGSM2Fix hook implementation is bundled.
