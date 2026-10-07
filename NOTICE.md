# Notices

Original jam-vm code is licensed under **BSD-2-Clause OR Apache-2.0**;
see [LICENSE.md](LICENSE.md). Imported files keep their own license notices.
The prepared `upstream/` trees are fetched separately and retain their upstream
licenses. Changes to OpenJDK in `patches/hotspot-jam.patch` and
`patches/labsjdk-compat.patch` retain the notices and terms of the corresponding
OpenJDK files.

Changes to Jam in `patches/jam-host-windows.patch` retain Jam's license notices
and terms. The extension supplies Windows mappings for the hosted heap.

Changes to Graal in `patches/graal-jam.patch` retain the notices and terms of
the corresponding Graal files. Packaged runtimes include the upstream JDK and
Graal notices, Jam and native licenses, and the LLVM runtime license under
`legal/`. LLVM's libc++, libc++abi and libunwind use Apache-2.0 with LLVM
exceptions; see the bundled license for their complete terms.
Windows packages include the LLVM compiler-runtime license and the Microsoft
redistribution documents supplied with their MSVC runtime DLLs.

## Documentation presentation

`docs/site/site.css`, `docs/site/site.js` and `docs/site/theme.js` come from
[ekmett/thc](https://github.com/ekmett/thc/tree/a94dd433054c098b82c52d606f55d279913a634f/docs/site)
at revision `a94dd433054c098b82c52d606f55d279913a634f`.
The documentation shell in `tools/build_docs.py` follows that revision's
[`src/tools/docs/Main.hs`](https://github.com/ekmett/thc/blob/a94dd433054c098b82c52d606f55d279913a634f/src/tools/docs/Main.hs).

Copyright 2026 Edward Kmett. These files and the derived shell retain
**UPL-1.0 AND BSD-3-Clause**. The complete upstream terms are included in
[docs/site/LICENSE-thc.txt](docs/site/LICENSE-thc.txt).

The stylesheet is unchanged. The scripts use jam-vm's page titles and a separate
appearance preference key. The shell has jam-vm's navigation, source links and
status text. The docs build copies the theme's license into the site.
