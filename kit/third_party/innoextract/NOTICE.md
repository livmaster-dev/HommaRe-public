# innoextract 1.9 and the libraries linked into it

The launcher installs a game from the player's own GOG offline installer
(`setup_*.exe` + `*-N.bin`, Inno Setup) with innoextract: the macOS app runs
`Contents/Helpers/innoextract`; the iOS app, which cannot run another program,
has innoextract built in as a static library (`build-ios.sh`, `lib/`); the
Windows app runs `innoextract.exe` beside its executable, innoextract's own
1.9 Windows build, unchanged (the licenses its archive carries are in
`resources/innoextract-license/`). None of
this code is the kit's own except `lib/` (the library's build files and C entry
point, MIT like the kit), and none of the rest is covered by the kit's MIT
license. Each part keeps the license below.

| Part | Version | License | How it is in the app |
|---|---|---|---|
| innoextract | 1.9 + 3 upstream patches (+ 1 own patch on iOS) | zlib | macOS: the helper executable; iOS: compiled into the app |
| innoextract for Windows (with the Boost, liblzma, zlib, bzip2 and libstdc++ runtime it links statically) | 1.9, the upstream release build | zlib; its archive's `license/` | Windows: `innoextract.exe` beside the app |
| Boost (Iostreams, Filesystem, Date_Time, Program_Options and the Boost libraries they build on) | 1.89.0 release archive, built by `build.sh` | Boost Software License 1.0 | macOS: linked statically into the helper |
| Boost (Filesystem and Iostreams sources, header-only parts) | 1.89.0 release archive | Boost Software License 1.0 | iOS: compiled into the app |
| liblzma (XZ Utils) | 5.8.1 release archive | 0BSD | linked statically |
| zlib | the system's `libz` | zlib | system library, not shipped |
| bzip2 / libbz2 | the system's `libbz2` | bzip2 license (BSD-style) | system library, not shipped |
| libiconv | the system's `libiconv` | system library | system library, not shipped |

- innoextract project: https://constexpr.org/innoextract/
- Exact source archive: https://constexpr.org/innoextract/files/innoextract-1.9.tar.gz
- SHA-256: `6344a69fc1ed847d4ed3e272e0da5998948c6b828cb7af39c6321aba6cf88126`
- Patches (`patches/`, unmodified upstream commits from
  https://github.com/dscharrer/innoextract, the same ones Homebrew's formula applies):
  - `01-cmake4-83d0bf4.patch` — commit 83d0bf4, build with CMake 4;
  - `02-boost185-264c2fe.patch` — commit 264c2fe (PR 169), build with Boost 1.85;
  - `03-boost189-882796e.patch` — commit 882796e (PR 199), build with Boost 1.89.
  These touch only the build files and `src/stream/slice.cpp`; under the zlib
  license's clause 2 the patched source is an altered version of innoextract.
- Build: `build.sh <output-dir>` (CMake, Release, `Boost_USE_STATIC_LIBS=ON`,
  `LZMA_USE_STATIC_LIBS=ON`).
- iOS library: `build-ios.sh <output-dir> [iphoneos|iphonesimulator]` (the
  kit's iOS CMake runs it once per build tree into `build/innoextract-ios/`)
  fetches, by SHA-256:
  - https://archives.boost.io/release/1.89.0/source/boost_1_89_0.tar.bz2
    (`85a33fa22621b4f314f8e85e1a5e2a9363d22e4f4992925d4bb3bc631b5a0c7a`);
  - https://github.com/tukaani-project/xz/releases/download/v5.8.1/xz-5.8.1.tar.gz
    (`507825b599356c10dca1cd720c9d0d0c9d5400b9de300af00e4d1ea150795543`);

  applies the three patches above and `lib-patches/01-progress-hook.patch`
  (the kit's own change to `src/util/console.{hpp,cpp}`: a hook in the
  progress bar's update through which the app follows the extraction and
  cancels it; marked in the source as added), and builds innoextract without
  `src/cli/main.cpp`, `src/util/process.cpp` (replaced by `lib/process.cpp`,
  which runs no programs) and `src/util/windows.cpp`, with `lib/configure.hpp`
  and `lib/release.cpp` in place of the files its CMake generates. Under the
  zlib license's clause 2 this is an altered version of innoextract.

- Windows: https://constexpr.org/innoextract/files/innoextract-1.9/innoextract-1.9-windows.zip
  (SHA-256 `6989342c9b026a00a72a38f23b62a8e6a22cc5de69805cf47d68ac2fec993065`),
  fetched by the kit's Windows CMake (`host/CMakeLists.txt`); only
  `innoextract.exe` and `license/*.txt` go into the app.

The installer's contents are the player's own: the app ships no game files, and
the install manifests under the game repository's `launcher-manifests/` hold
only file names, sizes and SHA-256 digests.

## innoextract — zlib License

```
Copyright (C) 2011-2020 Daniel Scharrer <daniel@constexpr.org>

This software is provided 'as-is', without any express or implied
warranty.  In no event will the author(s) be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
```

## Boost — Boost Software License 1.0

```
Boost Software License - Version 1.0 - August 17th, 2003

Permission is hereby granted, free of charge, to any person or organization
obtaining a copy of the software and accompanying documentation covered by
this license (the "Software") to use, reproduce, display, distribute,
execute, and transmit the Software, and to prepare derivative works of the
Software, and to permit third-parties to whom the Software is furnished to
do so, all subject to the following:

The copyright notices in the Software and this entire statement, including
the above license grant, this restriction and the following disclaimer,
must be included in all copies of the Software, in whole or in part, and
all derivative works of the Software, unless such copies or derivative
works are solely in the form of machine-executable object code generated by
a source language processor.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.
```

## liblzma (XZ Utils) — BSD Zero Clause License

Copyright (C) The XZ Utils authors and contributors. This software includes
code from XZ Utils <https://tukaani.org/xz/>. (The xz command line tools and
scripts, which have other licenses, are not used.)

```
Permission to use, copy, modify, and/or distribute this
software for any purpose with or without fee is hereby granted.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL
THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR
CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
```

## zlib and bzip2

Boost.Iostreams' zlib and bzip2 filters call the system libraries `libz` and
`libbz2` (macOS and iOS); the app links them dynamically and does not ship them.
zlib is under the zlib License (Copyright (C) 1995-2024 Jean-loup Gailly and
Mark Adler); bzip2 under its BSD-style license (Copyright (C) 1996-2019 Julian
R Seward). A build that links either statically
must add their full license texts here.

## For distribution

Boost and liblzma are built from the pinned source archives with the app's
deployment target (macOS 14.0, iOS 17.0), so the helper and the library run
on every system the app supports.
