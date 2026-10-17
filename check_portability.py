#!/usr/bin/env python3
"""Keeps platform-specific code inside src/compat.

Outside src/compat, a C file may:
  - include only ISO C99 headers, <stdatomic.h>, and the project's own headers;
  - not test platform macros (_WIN32, __APPLE__, __linux__, ...);
  - not call POSIX or platform functions that compat wraps (rename, access,
    stat, mkdir, getline, isatty, pthread_*, ...), nor name platform types
    (pthread_t, ssize_t, DIR, ...).

test/test_platform.c is exempt as well: it is the test suite's counterpart
of src/compat, the one place for what tests need from the platform and
src/compat has no reason to offer (temporary directories, permission bits,
symlinks, pipes, file size limits). It keeps its own header portable.

Comments and string literals are skipped, line continuations are joined, and
a banned function only counts as a call: `stat(` and `(stat)(` are calls,
`struct stat`, `->close(` and `my_open(` are not. There are no exceptions.

This is a lint that catches the usual slips early and says where they are,
not a proof: a macro that renames a call, a function pointer, or a compat
header that includes a platform header for its own use gets past it. The
Windows cross-compile (check_windows_compile.py, and the Windows builds in
CI) is what shows the code builds there.

Usage:
  python3 check_portability.py
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
SCAN_DIRS = ["src", "cmd", "tools", "test"]
COMPAT_DIR = os.path.join("src", "compat")
# The test suite's platform layer (see the docstring).
TEST_PLATFORM_FILE = "test/test_platform.c"

ISO_C_HEADERS = {
    "assert.h", "complex.h", "ctype.h", "errno.h", "fenv.h", "float.h",
    "inttypes.h", "iso646.h", "limits.h", "locale.h", "math.h", "setjmp.h",
    "signal.h", "stdarg.h", "stdbool.h", "stddef.h", "stdint.h", "stdio.h",
    "stdlib.h", "string.h", "tgmath.h", "time.h", "wchar.h", "wctype.h",
    # The one C11 header AGENTS.md allows.
    "stdatomic.h",
}

PLATFORM_MACROS = {
    "_WIN32", "_WIN64", "WIN32", "WIN64", "__WIN32__", "__WIN32", "_WINDOWS",
    "__MINGW32__", "__MINGW64__", "_MSC_VER", "__CYGWIN__", "__APPLE__",
    "__MACH__", "TARGET_OS_MAC", "TARGET_OS_IPHONE", "__linux__", "__linux",
    "linux", "__gnu_linux__", "__ANDROID__", "__unix__", "__unix", "unix",
    "__FreeBSD__", "__NetBSD__", "__OpenBSD__", "__DragonFly__", "__sun",
    "__HAIKU__", "_AIX", "__GLIBC__", "_POSIX_VERSION", "__wasm__",
    "__wasm32__", "__wasm64__", "__EMSCRIPTEN__",
}

# Functions that are POSIX or platform specific, or that are ISO C but behave
# differently enough across platforms that compat wraps them (rename does not
# replace an existing file on Windows; localtime is not thread safe).
BANNED_CALLS = {
    "access", "alarm", "chdir", "chmod", "clock_gettime", "close",
    "closedir", "dlclose", "dlopen", "dlsym", "dup", "dup2", "fchmod",
    "fcntl", "fdopen", "fileno", "flock", "fmemopen", "fork", "fseeko",
    "fstat", "fsync", "ftello", "ftruncate", "getcwd", "getdelim",
    "getline", "getpid", "getrlimit", "gettimeofday", "glob", "globfree",
    "gmtime_r", "ioctl", "isatty", "kill", "localtime", "localtime_r",
    "lseek", "lstat", "madvise", "mkdir", "mkdtemp", "mkfifo", "mkstemp",
    "mmap", "munmap", "nanosleep", "open", "opendir", "pclose", "pipe",
    "poll", "popen", "read", "readdir", "readlink", "realpath", "regcomp",
    "regerror", "regexec", "regfree", "rename", "rmdir",
    "sched_getaffinity", "sched_yield", "select", "setenv", "setrlimit",
    "sigaction", "sleep", "stat", "statvfs", "strcasecmp", "strncasecmp",
    "strndup", "strtok_r", "symlink", "sysconf", "tcgetattr", "tcsetattr",
    "truncate", "unlink", "unsetenv", "usleep", "utime", "waitpid",
    "write",
}

# Platform types and macros that may not be named at all outside compat.
BANNED_IDENTIFIERS = {
    "DIR", "dirent", "glob_t", "mode_t", "off_t", "pid_t", "regex_t",
    "regmatch_t", "ssize_t", "useconds_t",
}
BANNED_IDENTIFIER_PREFIXES = ("pthread_", "PTHREAD_")

INCLUDE_RE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*([<"])([^>"]+)[>"]',
                        re.MULTILINE)
IDENTIFIER_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


def strip_comments(text):
    """Replaces comments with spaces, keeping newlines and string literals."""
    out = []
    idx = 0
    length = len(text)
    while idx < length:
        char = text[idx]
        if text.startswith("//", idx):
            end = text.find("\n", idx)
            end = length if end == -1 else end
            out.append(" " * (end - idx))
            idx = end
        elif text.startswith("/*", idx):
            end = text.find("*/", idx + 2)
            end = length if end == -1 else end + 2
            out.append(re.sub(r"[^\n]", " ", text[idx:end]))
            idx = end
        elif char in "\"'":
            end = idx + 1
            while end < length and text[end] != char and text[end] != "\n":
                end += 2 if text[end] == "\\" else 1
            end = min(end + 1, length)
            out.append(text[idx:end])
            idx = end
        else:
            out.append(char)
            idx += 1
    return "".join(out)


def blank_literals(text):
    """Replaces string and character literals with spaces."""
    def blank(match):
        return " " * len(match.group(0))
    return re.sub(r'"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', blank, text)


def line_of(text, offset):
    return text.count("\n", 0, offset) + 1


def join_continuations(text):
    """Joins lines that end in a backslash, moving their newlines to the end
    of the joined line so that every later line keeps its number."""
    joined = []
    pending = 0
    for line in text.split("\n"):
        if line.endswith("\\"):
            joined.append(line[:-1])
            pending += 1
        else:
            joined.append(line + "\n" * (pending + 1))
            pending = 0
    return "".join(joined)[:-1]


def scan_file(rel_path):
    """Returns a list of (kind, name, line) violations in one file."""
    with open(os.path.join(ROOT, rel_path), encoding="utf-8") as handle:
        text = strip_comments(join_continuations(handle.read()))
    violations = []
    for match in INCLUDE_RE.finditer(text):
        delimiter, header = match.group(1), match.group(2)
        if delimiter == "<" and header not in ISO_C_HEADERS:
            violations.append(("include", header, line_of(text, match.start())))
    code = blank_literals(INCLUDE_RE.sub(lambda m: " " * len(m.group(0)),
                                         text))
    for match in IDENTIFIER_RE.finditer(code):
        name = match.group(0)
        line = line_of(code, match.start())
        if name in PLATFORM_MACROS:
            violations.append(("macro", name, line))
        elif name in BANNED_IDENTIFIERS or name.startswith(
                BANNED_IDENTIFIER_PREFIXES):
            violations.append(("identifier", name, line))
        elif name in BANNED_CALLS:
            before = code[:match.start()].rstrip()
            after = code[match.end():].lstrip()
            is_member = before.endswith(".") or before.endswith("->")
            is_struct_tag = re.search(r"\bstruct$", before) is not None
            # `(rename)(...)` calls the function past any macro of its name.
            parenthesized = (before.endswith("(") and after.startswith(")") and
                             after[1:].lstrip().startswith("("))
            if ((after.startswith("(") or parenthesized) and not is_member and
                    not is_struct_tag):
                violations.append(("call", name, line))
    return violations


def source_files():
    for scan_dir in SCAN_DIRS:
        for dirpath, _, filenames in os.walk(os.path.join(ROOT, scan_dir)):
            rel_dir = os.path.relpath(dirpath, ROOT)
            if rel_dir == COMPAT_DIR or rel_dir.startswith(COMPAT_DIR + os.sep):
                continue
            for filename in sorted(filenames):
                path = os.path.join(rel_dir, filename).replace(os.sep, "/")
                if (filename.endswith((".c", ".h")) and
                        path != TEST_PLATFORM_FILE):
                    yield path


def main():
    violations = []
    for rel_path in sorted(source_files()):
        for kind, name, line in scan_file(rel_path):
            violations.append((rel_path, line, kind, name))
    for rel_path, line, kind, name in violations:
        print(f"{rel_path}:{line}: {kind} '{name}' is platform specific; "
              "move it behind src/compat")
    if violations:
        print(f"\nPortability check failed: {len(violations)} violation(s).")
        return 1
    print("Portability check passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
