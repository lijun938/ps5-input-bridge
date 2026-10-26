# Third-party components

PS5 Input Bridge uses the following upstream components. Native libraries are statically linked; source pins and download URLs are recorded in `native/scripts/bootstrap.py`. The chiaki-ng feedback sender and its header are modified for synchronous input, complete control history, short state/history repeats and transport-error recovery; complete replacements are in `native/patches/chiaki/`. Checked edits to `lib/src/session.c` and `lib/src/takion.c` propagate send errors and are recorded in `native/patches/chiaki-return-errors.json`. The bootstrap applies both patch forms to the pinned source. Distribute corresponding PS5 Input Bridge source and build instructions with binaries under AGPL-3.0; keep all upstream notices.

| Component | Version / source | License copy |
| --- | --- | --- |
| chiaki-ng / libchiaki | v1.10.0, `0c4a45df0cae2af2ba2daef84e881850b07038a3` | `licenses/chiaki-ng-COPYING` (AGPL-3.0 and OpenSSL exception) |
| mbedTLS | 2.28.0 pinned commit | `licenses/mbedtls-LICENSE` |
| json-c | 0.17 | `licenses/json-c-COPYING` |
| miniupnpc | 2.2.8 | `licenses/miniupnpc-LICENSE` |
| libevent | 2.1.12-stable | `licenses/libevent-LICENSE` |
| curl | pinned chiaki-ng submodule | `licenses/curl-COPYING` |
| nanopb | pinned chiaki-ng submodule | `licenses/nanopb-LICENSE` |
| Jerasure | pinned chiaki-ng submodule | `licenses/jerasure-COPYING` |
| GF-Complete | pinned chiaki-ng submodule | `licenses/gf-complete-COPYING` |
| MinGW-w64 runtime support | LLVM-MinGW toolchain static runtime support | `licenses/mingw-runtime-COPYING` |

The published Windows application also includes the Microsoft .NET 9 / Windows Desktop runtime. Its supplied licenses and third-party notices are copied into `licenses/microsoft.netcore.app.runtime.win-x64-LICENSE.txt`, `licenses/microsoft.netcore.app.runtime.win-x64-NOTICES.txt`, and `licenses/microsoft.windowsdesktop.app.runtime.win-x64-LICENSE.txt`. Build-only LLVM-MinGW, CMake, Ninja, protoc and Python tools are not part of the runtime distribution.
