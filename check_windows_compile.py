#!/usr/bin/env python3
"""Compiles every C file for Windows, to catch breaks without a Windows machine.

Each .c file under src, cmd, tools and test is compiled on its own with a
MinGW-w64 cross compiler (llvm-mingw in CI) and the warning flags of the dev
build, -Werror included. Every file must compile: there are no exceptions.

Usage:
  python3 check_windows_compile.py [--cc CC]...

--cc may be given more than once (CI passes the x86_64 and aarch64
compilers); a file counts as compiling only if it compiles with every one.
The default is x86_64-w64-mingw32-clang.
"""

import concurrent.futures
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
SCAN_DIRS = ["src", "cmd", "tools", "test"]
DEFAULT_CC = "x86_64-w64-mingw32-clang"
CFLAGS = [
    "-fsyntax-only", "-Wall", "-Wno-trigraphs", "-Wextra", "-Wshadow",
    "-Wstrict-prototypes", "-Werror", "-DBOARD_DIM=15", "-DRACK_SIZE=7",
    '-DMAGPIE_BUILD_TARGET="default"',
]


def source_files():
    files = []
    for scan_dir in SCAN_DIRS:
        for dirpath, _, filenames in os.walk(os.path.join(ROOT, scan_dir)):
            for filename in filenames:
                if filename.endswith(".c"):
                    path = os.path.relpath(os.path.join(dirpath, filename),
                                           ROOT)
                    files.append(path.replace(os.sep, "/"))
    return sorted(files)


def compile_file(compiler, path):
    result = subprocess.run([compiler] + CFLAGS + [path], cwd=ROOT,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, check=False)
    return result.returncode == 0, result.stdout


def main():
    args = sys.argv[1:]
    compilers = [args[idx + 1] for idx, arg in enumerate(args)
                 if arg == "--cc" and idx + 1 < len(args)] or [DEFAULT_CC]
    files = source_files()

    failures = {}
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 4) as pool:
        jobs = {pool.submit(compile_file, compiler, path): (compiler, path)
                for compiler in compilers for path in files}
        for job in concurrent.futures.as_completed(jobs):
            compiler, path = jobs[job]
            compiled, output = job.result()
            if not compiled:
                failures.setdefault(path, []).append((compiler, output))

    for path in sorted(failures):
        for compiler, output in failures[path]:
            print(f"=== {path} does not compile with {compiler}:\n{output}")
    compiled = len(files) - len(failures)
    print(f"{compiled} of {len(files)} files compile for Windows with "
          f"{', '.join(compilers)}.")
    if failures:
        print(f"Windows compile check failed: {len(failures)} file(s).")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
