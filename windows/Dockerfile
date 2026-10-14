# The Windows build environment: the pinned llvm-mingw toolchain (clang for
# x86_64 and aarch64 Windows, UCRT) plus make, python3 and Wine, which `make
# windows`, `make windows_test` and `make windows_wine_test` run in. CI uses
# the same release, unpacked from its tarball (.github/workflows/run-tests.yml).
FROM mstorsjo/llvm-mingw:20261006
RUN apt-get update -qq \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends make python3 wine64 \
 && rm -rf /var/lib/apt/lists/*
