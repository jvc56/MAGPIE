# The Windows build environment: the pinned llvm-mingw toolchain (clang for
# x86_64 and aarch64 Windows, UCRT) plus make, python3 and Wine, which `make
# windows`, `make windows_test` and `make windows_wine_test` run in. CI uses
# the same release, unpacked from its tarball (.github/workflows/run-tests.yml).
#
# The base image is pinned by the digest of its multi-arch index, not only by
# its tag, so a re-pushed tag cannot change the compiler. Its distro is Ubuntu
# 24.04 (noble); make and python3 are pinned to the versions the image already
# has, and wine64 to the one in noble's release pocket, which never changes.
# To update: `docker buildx imagetools inspect mstorsjo/llvm-mingw:<tag>` for
# the digest, then `apt-cache policy make python3 wine64` in that image.
FROM mstorsjo/llvm-mingw:20261006@sha256:3f535cae24312a5bb25968ef670ea070e422da01f3f642bac550bca781bac29e
RUN apt-get update -qq \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
      make=4.3-4.1build2 \
      python3=3.12.3-0ubuntu2.1 \
      wine64=9.0~repack-4build3 \
 && rm -rf /var/lib/apt/lists/*
