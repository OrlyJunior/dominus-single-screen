# Third-party components

The root MIT license covers project-owned material only. Vendored components,
upstream signature data and toolchain runtime notices retain their own terms;
none are automatically relicensed by this publication.

The optional developer capture adapter uses RenderDoc's MIT-licensed API header,
copied from the existing RenderDoc 1.46 distribution. Its complete notice is in
`third_party/renderdoc/renderdoc_app.h`; see the adjacent `NOTICE.md`. No RenderDoc
DLL is included in the mod payload.

The source and binary development package includes MinHook **v1.3.4**, obtained
as source from the [official repository](https://github.com/TsudaKageyu/minhook/tree/v1.3.4).
MinHook and the included Hacker Disassembler Engine have BSD two-clause terms.
The complete required notices are retained in
`third_party/minhook/LICENSE.txt`; keep that file with any binary redistribution.
No precompiled MinHook DLL was downloaded or bundled.

Three static identification signatures were consulted in MGSM2Fix at commit
`8739522145bdf790b5e1029ada57d629c5be1a71`, under the MIT license. No hook
implementation or dependency from MGSM2Fix was copied. Its notice is retained
below for the signature data. See `docs/COMPATIBILITY.md` for exact references.

> MIT License
>
> Copyright (c) 2023-2026 nuggslet
>
> Permission is hereby granted, free of charge, to any person obtaining a copy
> of this software and associated documentation files (the "Software"), to deal
> in the Software without restriction, including without limitation the rights
> to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
> copies of the Software, and to permit persons to whom the Software is
> furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in all
> copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
> IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
> FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
> AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
> LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
> OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
> SOFTWARE.

geo-11/Helixmod material was research only. No archive, shader fix or proprietary
game asset is included. The development compiler is separate from the mod:
official Zig 0.13.0 Windows x86_64, archive SHA-256
`D859994725EF9402381E557C60BB57497215682E355204D754EE3DF75EE3C158`.
The locally retained portable toolchain includes its upstream licenses; it is
not included in the mod package. The statically linked C/C++ runtime notices
(libc++, libc++abi, libunwind, MinGW and Zig) are retained separately in
`third_party/runtime-licenses/` and must accompany binary redistribution.
System Windows D3D11, DXGI and shader compiler
DLLs are loaded from System32 at runtime and are not redistributed.
