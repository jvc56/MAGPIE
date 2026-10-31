ifeq ($(origin NPROCS), undefined)
NPROCS := $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
endif
ifeq ($(filter -j% --jobs%,$(MAKEFLAGS)),)
MAKEFLAGS += -j$(NPROCS)
endif

SRC_DIR := src
TEST_DIR := test
CMD_DIR := cmd
TOOLS_DIR := tools
BIN_DIR := bin
COV_DIR := cov

# Build knobs that change the compiled objects. Defined up here (before OBJ_DIR)
# so object files can be keyed by them.
# dev is the default. Use `make release` for the production PGO build, or
# BUILD=no_pgo_release for the old optimized build without PGO.
BUILD ?= dev
ifeq ($(BUILD),release)
$(error release is a target; run 'make release', or use BUILD=no_pgo_release)
endif
ifndef BOARD_DIM
BOARD_DIM = 15
endif
ifndef RACK_SIZE
RACK_SIZE = 7
endif

# The portable optimized build. Identical to no_pgo_release except that the
# instruction set is fixed instead of being whatever the building machine
# happens to have.
#
# This exists because a wordmap's and a rack info table's bytes have to be a
# function of their inputs alone. birdtest's server builds a reference copy of
# each derived file and sends its SHA-256 to workers, which build their own and
# refuse to use one whose hash differs (see birdtest's README.md, "MAGPIE on
# the server").
# -march=native makes the binary's instruction set a property of the machine
# that compiled it, so two builds of the same commit can vectorize the same
# floating-point reduction differently and produce different bytes for the same
# inputs.
#
# The target is spelled `nehalem` rather than `x86-64-v2`: they name the same
# instruction set (SSE4.2, POPCNT), which every machine this runs on already
# meets, Fargate included, but `x86-64-v2` only reached GCC in 11 and Clang in
# 12 -- Ubuntu 20.04's clang 10 rejects it outright -- and a target some
# compilers cannot parse is no use for a build meant to be reproducible
# everywhere. `nehalem` additionally tunes for that microarchitecture, which
# changes instruction scheduling and not results.
#
# PORTABLE_MARCH overrides the target for a non-x86 host; a build for one
# architecture is only comparable with another build for that same
# architecture, which is why the target travels with the binary and the server
# records it alongside every hash (see `magpie builders`).
PORTABLE_MARCH ?= nehalem

# A Windows build is a cross-compile with MinGW-w64 (llvm-mingw: see `make
# windows`), recognized from the compiler's target triple. It produces .exe
# files, links statically (one self-contained executable, no DLLs), and keeps
# its objects apart from the host's.
TARGET_TRIPLE := $(shell $(CC) -dumpmachine 2>/dev/null)
# GCC says x86_64-w64-mingw32, clang x86_64-w64-windows-gnu.
ifneq ($(findstring mingw,$(TARGET_TRIPLE))$(findstring windows,$(TARGET_TRIPLE)),)
WINDOWS := 1
EXE := .exe
OBJ_TARGET_PREFIX := $(TARGET_TRIPLE)-
endif

# The compiler, by family and major version (appleclang17, clang18, gcc13). An
# LTO object holds its compiler's own intermediate code, which another
# compiler, or an older version of the same one, cannot link. GCC does not
# always name itself: Ubuntu's cc says only "cc (Ubuntu 13.1.0-...) 13.1.0"
# above the Free Software Foundation's copyright. A compiler of no known
# family still keys on its version.
CC_VERSION_TEXT := $(shell $(CC) --version 2>/dev/null)
CC_MAJOR := $(firstword $(subst ., ,$(shell $(CC) -dumpversion 2>/dev/null)))
ifneq ($(findstring Apple clang,$(CC_VERSION_TEXT)),)
OBJ_CC := appleclang$(CC_MAJOR)
else ifneq ($(findstring clang,$(CC_VERSION_TEXT)),)
OBJ_CC := clang$(CC_MAJOR)
else ifneq ($(findstring GCC,$(CC_VERSION_TEXT))$(findstring gcc,$(CC_VERSION_TEXT))$(findstring Free Software Foundation,$(CC_VERSION_TEXT)),)
OBJ_CC := gcc$(CC_MAJOR)
else
OBJ_CC := cc$(CC_MAJOR)
endif

# Key every object (and its .d fragment) by what changes its contents: build
# flavor, compiler, board dim, rack size. Switching any of them selects a
# different obj subtree instead of relinking objects compiled with mismatched
# flags or by another compiler -- so no `make clean` is needed between them,
# and switching back reuses the cached objects. `clean` wipes the whole
# OBJ_ROOT.
OBJ_ROOT := obj
# portable_release additionally keys on its target: the whole point of that
# flavor is that the target is fixed, so changing it has to recompile rather
# than relink objects built for the old one.
ifeq ($(BUILD),portable_release)
OBJ_DIR := $(OBJ_ROOT)/$(OBJ_TARGET_PREFIX)$(BUILD)-$(PORTABLE_MARCH)-$(OBJ_CC)-b$(BOARD_DIM)-r$(RACK_SIZE)
else
OBJ_DIR := $(OBJ_ROOT)/$(OBJ_TARGET_PREFIX)$(BUILD)-$(OBJ_CC)-b$(BOARD_DIM)-r$(RACK_SIZE)
endif

# Profile-guided optimization data. The default lives below OBJ_ROOT so it is
# ignored by git and removed by `make clean` with the other build artifacts.
PGO_DIR ?= $(OBJ_ROOT)/pgo-data
PGO_RAW_DIR ?= $(PGO_DIR)/raw
PGO_PROFILE ?= $(abspath $(PGO_DIR)/magpie.profdata)
# PGO needs clang's instrumentation and llvm-profdata. Distros often install
# only versioned names (clang-18, llvm-profdata-18), so look for those too. An
# explicit PGO_CC or LLVM_PROFDATA on the command line wins. With no clang at
# all, `make release` builds the plain optimized release and says so.
PGO_TOOL_VERSIONS := 20 19 18 17 16 15
# The oldest clang the PGO build supports; matches the versioned names above.
# Older clangs (Ubuntu 20.04 ships clang 10) lack flags the instrumented build
# relies on, so auto-selection skips them and `make release` degrades to the
# plain optimized build with a one-line explanation instead of failing on
# every object file.
PGO_MIN_CLANG_MAJOR := 15
# Major version of a clang ("Apple clang version 17.0.0", "clang version
# 10.0.0-4ubuntu1"), or empty if the command is missing or is not clang.
pgo_clang_major = $(shell $(1) --version 2>/dev/null | sed -n '1s/.*clang version \([0-9][0-9]*\).*/\1/p')
PGO_CC_CANDIDATES := clang $(addprefix clang-,$(PGO_TOOL_VERSIONS))
# Expanded once: a recursive `?=` would rerun every probe on each reference.
ifeq ($(origin PGO_CC),undefined)
PGO_CC := $(firstword $(foreach c,$(PGO_CC_CANDIDATES),$(shell v=$$($(c) --version 2>/dev/null | sed -n '1s/.*clang version \([0-9][0-9]*\).*/\1/p'); if test -n "$$v" && test "$$v" -ge $(PGO_MIN_CLANG_MAJOR); then echo $(c); fi)))
endif
# For diagnostics: the clang that would have been used, even if it is too old.
PGO_CC_FOUND := $(if $(PGO_CC),$(PGO_CC),$(firstword $(foreach c,$(PGO_CC_CANDIDATES),$(if $(shell command -v $(c) 2>/dev/null),$(c),))))
PGO_CC_FOUND_MAJOR := $(if $(PGO_CC_FOUND),$(call pgo_clang_major,$(PGO_CC_FOUND)))
# Non-empty when PGO_CC exists, is clang, and is new enough to train with.
PGO_CC_USABLE := $(if $(PGO_CC),$(shell if test -n "$(PGO_CC_FOUND_MAJOR)" && test "$(PGO_CC_FOUND_MAJOR)" -ge $(PGO_MIN_CLANG_MAJOR); then echo yes; fi))
PGO_LDFLAGS ?=
PGO_TRAIN_GAMES ?= 16
PGO_TRAIN_TIME_MS ?= 1000
PGO_TRAIN_SECONDS ?= 0.05
PGO_TRAIN_THREADS ?= $(NPROCS)
PGO_LEAVEGEN_TARGET ?= 100
PGO_LEAVEGEN_DATA_DIR ?= $(abspath $(PGO_DIR)/leavegen-data)
# Prefer the llvm-profdata matching a versioned clang (clang-18 pairs with
# llvm-profdata-18), then the unversioned one, then any versioned one; on macOS
# Xcode provides it through xcrun.
PGO_CC_VERSION_SUFFIX := $(patsubst clang%,%,$(notdir $(PGO_CC)))
LLVM_PROFDATA ?= $(firstword $(foreach p,llvm-profdata$(PGO_CC_VERSION_SUFFIX) llvm-profdata $(addprefix llvm-profdata-,$(PGO_TOOL_VERSIONS)),$(if $(shell command -v $(p) 2>/dev/null),$(p),)))
ifeq ($(LLVM_PROFDATA),)
ifneq ($(shell command -v xcrun 2>/dev/null),)
LLVM_PROFDATA := xcrun llvm-profdata
endif
endif

# find, not a wildcard: `**` is not recursive in make, and vendored code sits
# two levels down (src/compat/sha256/sha256.c).
SRC  := $(shell find $(SRC_DIR) -name '*.c')
TEST := $(wildcard $(TEST_DIR)/*.c)
CMD := $(wildcard $(CMD_DIR)/*.c)
OBJ_SRC := $(SRC:$(SRC_DIR)/%.c=$(OBJ_DIR)/$(SRC_DIR)/%.o)
OBJ_TEST := $(TEST:$(TEST_DIR)/%.c=$(OBJ_DIR)/$(TEST_DIR)/%.o)
OBJ_CMD := $(CMD:$(CMD_DIR)/%.c=$(OBJ_DIR)/$(CMD_DIR)/%.o)
PGO_TRAIN_OBJ := $(OBJ_DIR)/$(TOOLS_DIR)/pgo_train.o
CONVERT_OBJ := $(OBJ_DIR)/$(TOOLS_DIR)/convert.o

SRC_SUBDIRS := $(shell find $(SRC_DIR) -type d)
SRC_OBJ_SUBDIRS := $(patsubst $(SRC_DIR)/%,$(OBJ_DIR)/$(SRC_DIR)/%,$(SRC_SUBDIRS))

TEST_SUBDIRS := $(shell find $(TEST_DIR) -type d)
TEST_OBJ_SUBDIRS := $(patsubst $(TEST_DIR)/%,$(OBJ_DIR)/$(TEST_DIR)/%,$(TEST_SUBDIRS))

ifdef WINDOWS
    # MinGW's sanitizer runtimes need DLLs, which a static, self-contained
    # executable cannot use; Windows builds are checked by running the tests.
    FSAN_ARG :=
else ifeq ($(BUILD),thread)
    FSAN_ARG := -fsanitize=thread
else
    FSAN_ARG := -fsanitize=address,undefined,pointer-compare,pointer-subtract
    # Test whether the leak flag is supported by the compiler
    ifeq ($(shell echo "int main() { return 0; }" | $(CC) -x c - -fsanitize=leak -o /dev/null >/dev/null 2>&1; echo $$?),0)
        FSAN_ARG += -fsanitize=leak
    endif
endif

cflags.dev := -g -O0 -Wall -Wno-trigraphs -Wextra -Wshadow -Wstrict-prototypes -Werror $(FSAN_ARG)
# dev's checks at -O1, the level the sanitizers' own documentation asks for:
# locals live in registers rather than a stack slot each, so ASAN has far
# fewer loads and stores to instrument, and small inline functions inline.
# Frame pointers and sibling calls are kept so its stack traces stay whole.
# Optimizing turns on checks -O0 never runs, which GCC 13 fails on today, so
# they are turned back off to hold it to dev's warnings: _FORTIFY_SOURCE,
# whose call checks ASAN makes itself, and -Wmaybe-uninitialized and
# -Wformat-truncation, which guess from what the optimizer sees. Clang has no
# -Wmaybe-uninitialized and, under -Werror, refuses to turn off a warning it
# does not know, so each is passed only to a compiler that accepts it; the
# test compiles run only when this build is selected. CI's sanitizer jobs
# build this; dev stays -O0, for the debugger.
sanitize_quiet_warnings = $(foreach warning,-Wno-maybe-uninitialized -Wno-format-truncation,$(shell echo "int main(void) { return 0; }" | $(CC) -x c - -Werror $(warning) -o /dev/null >/dev/null 2>&1 && echo $(warning)))
cflags.sanitize = -g -O1 -fno-omit-frame-pointer -fno-optimize-sibling-calls -U_FORTIFY_SOURCE -Wall -Wno-trigraphs -Wextra -Wshadow -Wstrict-prototypes $(sanitize_quiet_warnings) -Werror $(FSAN_ARG)
cflags.thread := -g -O0 -Wall -Wno-trigraphs -Wextra -Wshadow -Wstrict-prototypes -Werror -fsanitize=thread
cflags.vlg := -g -O0 -Wall -Wno-trigraphs -Wextra
cflags.cov := -g -O0 -Wall -Wno-trigraphs -Wextra --coverage
# Every function of an optimized build starts on a 64-byte boundary. A
# function's offset in its cache line, and on x86 in the instruction
# decoder's windows, otherwise shifts with any edit that adds or removes code
# before it, which can change its speed by a few percent without changing
# its code. Measured against the default layout in #887: on an Intel
# i7-9750H aligned builds ran 0.6% to 3.4% faster; on an Apple M2, from 1.6%
# faster to 0.5% slower, by workload. Aligned, a change that moves code no
# longer moves the timings of code it leaves alone.
#
# GCC aligns every function from -falign-functions, which its LTO link takes
# too. With LTO, clang's -falign-functions misses functions made at the link
# (a third of them on macOS), so clang's link also passes LLVM's
# align-all-functions, in the form its linker takes: -mllvm for ld64 and
# lld, -plugin-opt for the LTO plugin of GNU ld and gold. Probed only for the
# optimized builds, with the compiler that will link.
align_cflags := -falign-functions=64
ALIGNED_BUILDS := no_pgo_release portable_release lib bai_stats pgo_generate pgo_use \
                  test_no_pgo_release test_portable_release test_bai_stats \
                  test_pgo_generate test_pgo_use
ifneq ($(filter $(ALIGNED_BUILDS),$(BUILD)),)
ifneq ($(findstring clang,$(CC_VERSION_TEXT)),)
align_ldflags := $(shell for flag in -Wl,-mllvm,-align-all-functions=6 -Wl,-plugin-opt=-align-all-functions=6; do if printf 'int main(void) { return 0; }\n' | $(CC) -x c - -flto $$flag -o /dev/null >/dev/null 2>&1; then echo $$flag; break; fi; done)
else
align_ldflags := $(align_cflags)
endif
endif

# The optimized builds carry debug information, so a profile of the binary
# that runs attributes its time to source lines and to the functions inlined
# there. It costs build time, not run time: it changes the code only a little
# (clang's registers and instruction order in a few functions, GCC 13's LTO
# partitioning) and not its speed. DWARF 4, since the binutils of older Linux
# distributions, which perf reads source lines through, cannot read DWARF 5.
cflags.no_pgo_release := -O3 -flto -march=native -DNDEBUG -Wall -Wno-trigraphs -gdwarf-4 $(align_cflags)
cflags.portable_release := -O3 -flto -march=$(PORTABLE_MARCH) -DNDEBUG -Wall -Wno-trigraphs $(align_cflags)
cflags.test_portable_release := -O3 -flto -march=$(PORTABLE_MARCH) -Wall -Wno-trigraphs $(align_cflags)
# Test-specific flags: like no_pgo_release but without DNDEBUG (asserts always enabled in tests)
cflags.test_no_pgo_release := -O3 -flto -march=native -Wall -Wno-trigraphs -gdwarf-4 $(align_cflags)
# no_pgo_release plus BAI scheduling instrumentation (src/ent/bai_sched_stats.h),
# read by the on-demand baisched test. Production builds compile it out.
cflags.bai_stats := $(cflags.no_pgo_release) -DBAI_SCHED_STATS
cflags.test_bai_stats := $(cflags.test_no_pgo_release) -DBAI_SCHED_STATS
# Training runs multithreaded, so counter updates must be atomic. clang 17
# added the GCC-style -fprofile-update=atomic; older clangs get the LLVM-native
# equivalent. Probed only for the instrumented builds, with the compiler that
# will do the compiling.
ifneq ($(filter pgo_generate test_pgo_generate,$(BUILD)),)
pgo_atomic_flag := $(shell if printf 'int main(void) { return 0; }\n' | $(CC) -fprofile-instr-generate -fprofile-update=atomic -x c -c - -o /dev/null >/dev/null 2>&1; then echo -fprofile-update=atomic; else echo -mllvm -instrprof-atomic-counter-update-all; fi)
endif
cflags.pgo_generate := $(cflags.no_pgo_release) -fprofile-instr-generate $(pgo_atomic_flag)
cflags.test_pgo_generate := $(cflags.test_no_pgo_release) -fprofile-instr-generate $(pgo_atomic_flag)
pgo_use_flags := -fprofile-instr-use=$(PGO_PROFILE) \
                 -Werror=profile-instr-out-of-date \
                 -Wno-profile-instr-unprofiled
cflags.pgo_use := $(cflags.no_pgo_release) $(pgo_use_flags)
cflags.test_pgo_use := $(cflags.test_no_pgo_release) $(pgo_use_flags)
# Shared library flavor: release optimization plus -fPIC, no sanitizers
cflags.lib := -O3 -flto -march=native -DNDEBUG -Wall -Wno-trigraphs -fPIC $(align_cflags)
lflags.cov := --coverage

ldflags.dev := -pthread $(FSAN_ARG)
ldflags.sanitize := -pthread $(FSAN_ARG)
ldflags.lib := -pthread $(align_ldflags)
ldflags.thread := -pthread -fsanitize=thread
ldflags.vlg := -pthread
# GCC compiles an LTO build's code at the link, so the link takes the debug
# flag too.
ldflags.no_pgo_release := -pthread -flto -gdwarf-4 $(align_ldflags)
ldflags.portable_release := -pthread -flto $(align_ldflags)
ldflags.bai_stats := -pthread -flto -gdwarf-4 $(align_ldflags)
ldflags.pgo_generate := -pthread -flto -gdwarf-4 -fprofile-instr-generate $(PGO_LDFLAGS) $(align_ldflags)
ldflags.pgo_use := -pthread -flto -gdwarf-4 -fprofile-instr-use=$(PGO_PROFILE) $(PGO_LDFLAGS) $(align_ldflags)
ldflags.cov := -pthread
# The test builds link as the builds they test: without -pthread, a test that
# starts a thread itself failed to link under test_no_pgo_release.
ldflags.test_no_pgo_release := $(ldflags.no_pgo_release)
ldflags.test_portable_release := $(ldflags.portable_release)
ldflags.test_bai_stats := $(ldflags.bai_stats)
ldflags.test_pgo_generate := $(ldflags.pgo_generate)
ldflags.test_pgo_use := $(ldflags.pgo_use)

CFLAGS := ${cflags.${BUILD}}

# The instruction-set target compiled into this binary, reported by
# `magpie builders` and recorded by birdtest beside every derived-file hash.
# See src/def/builder_defs.h for why a hash is only meaningful together with
# the build that produced it.
ifeq ($(BUILD),portable_release)
BUILD_TARGET := $(PORTABLE_MARCH)
else ifneq ($(findstring -march=native,${cflags.${BUILD}}),)
BUILD_TARGET := native
else
BUILD_TARGET := default
endif
CFLAGS += -DMAGPIE_BUILD_TARGET=\"$(BUILD_TARGET)\"

# Emit a .d makefile fragment next to each .o listing the headers it includes
# (-MMD) plus phony targets for those headers (-MP, so deleting a header does
# not break the build). These fragments are -included at the bottom, so editing
# a header recompiles exactly the .c files that include it -- no `make clean`.
DEPFLAGS := -MMD -MP

CFLAGS += -DBOARD_DIM=$(BOARD_DIM) -DRACK_SIZE=$(RACK_SIZE)
ifeq ($(BUILD),pgo_use)
CMD_CFLAGS := $(cflags.no_pgo_release) -DBOARD_DIM=$(BOARD_DIM) -DRACK_SIZE=$(RACK_SIZE)
else
CMD_CFLAGS := $(CFLAGS)
endif


LFLAGS := ${lflags.${BUILD}}
# Expanded at each link, for the macOS LTO object path below.
LDFLAGS  = ${ldflags.${BUILD}}
LDLIBS   := -lm

# On macOS the debug information stays in the object files, which the
# executable points to. Clang's LTO link compiles its code into a temporary
# object and deletes it afterwards, and the debug information of that code with
# it, so keep that object beside the build's others, one per linked file. GCC
# runs its own LTO, and its link driver misreads the option.
ifndef WINDOWS
ifeq ($(shell uname -s),Darwin)
ifneq ($(filter -flto,${ldflags.${BUILD}}),)
ifneq ($(filter -gdwarf-4,${ldflags.${BUILD}}),)
ifneq ($(findstring clang,$(CC_VERSION_TEXT)),)
LDFLAGS += -Wl,-object_path_lto,$(OBJ_DIR)/$(@F).lto.o
endif
endif
endif
endif
endif

# The HTTP compat layer resolves libcurl with dlopen at runtime rather than
# linking it, so a machine without libcurl still runs every offline command.
# glibc before 2.34 keeps dlopen in a separate libdl; macOS and newer glibc
# have it in libc.
ifdef WINDOWS
  # Windows 10 APIs (FileRenameInfoEx's POSIX rename, VT processing). The
  # 8 MB main-thread stack matches Linux's default: PEG, the endgame and BAI
  # recurse on the calling thread, and MinGW's default is 2 MB.
  CFLAGS += -D_WIN32_WINNT=0x0A00
  CMD_CFLAGS += -D_WIN32_WINNT=0x0A00
  LDFLAGS += -static -Wl,--stack,8388608
  LDLIBS += -lwinhttp
else ifeq ($(shell uname -s),Linux)
  LDLIBS += -ldl
endif

# The resources (manifest, icon, version) linked into Windows executables.
comma := ,
ifdef WINDOWS
WINDRES ?= $(subst -gcc,-windres,$(subst -clang,-windres,$(CC)))
MAGPIE_VERSION := $(shell sed -n 's/^\#define MAGPIE_VERSION "\(.*\)"/\1/p' $(SRC_DIR)/impl/config.c)
MAGPIE_VERSION_COMMAS := $(subst .,$(comma),$(MAGPIE_VERSION)),0
RES_OBJ := $(OBJ_DIR)/magpie_resources.o
endif

.PHONY: windows windows_test windows_wine_test windows_image
.PHONY: all clean iwyu release leavegen_pgo_release pgo pgo_sim pgo_peg word_frequencies \
	pgo_toolchain_check \
	pgo_eg peg_eg pgo_workload prepare_data libmagpie examples

all: magpie magpie_test

libmagpie_core.a: $(OBJ_SRC)
	ar rcs $@ $^

# Shared library for embedding magpie; the API surface is src/impl/cmd_api.h
# and only magpie_* symbols are exported. Re-invokes make with BUILD=lib so
# objects are always compiled with -fPIC and without sanitizers.
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
SHARED_LIB := $(BIN_DIR)/libmagpie.dylib
else
SHARED_LIB := $(BIN_DIR)/libmagpie.so
endif

libmagpie:
	@$(MAKE) BUILD=lib $(SHARED_LIB)

$(OBJ_DIR)/libmagpie.map: | $(OBJ_DIR)
	printf '{ global: magpie_*; local: *; };\n' > $@

$(OBJ_DIR)/libmagpie.exports: | $(OBJ_DIR)
	printf '_magpie_*\n' > $@

$(BIN_DIR)/libmagpie.so: $(OBJ_SRC) $(OBJ_DIR)/libmagpie.map | $(BIN_DIR)
	$(CC) -shared $(LDFLAGS) $(LFLAGS) $(OBJ_SRC) $(LDLIBS) -Wl,--version-script=$(OBJ_DIR)/libmagpie.map -o $@

$(BIN_DIR)/libmagpie.dylib: $(OBJ_SRC) $(OBJ_DIR)/libmagpie.exports | $(BIN_DIR)
	$(CC) -dynamiclib $(LDFLAGS) $(LFLAGS) $(OBJ_SRC) $(LDLIBS) -Wl,-exported_symbols_list,$(OBJ_DIR)/libmagpie.exports -o $@

# Example programs embedding magpie through the shared library
ifeq ($(UNAME_S),Darwin)
EXAMPLE_RPATH := -Wl,-rpath,@loader_path
else
EXAMPLE_RPATH := -Wl,-rpath,'$$ORIGIN'
endif

examples: libmagpie
	$(CC) -O2 -Wall -Werror examples/generate_moves.c -L$(BIN_DIR) -lmagpie $(EXAMPLE_RPATH) -o $(BIN_DIR)/generate_moves

magpie: $(OBJ_SRC) $(OBJ_CMD) $(RES_OBJ) | $(BIN_DIR)
	$(CC) $(LDFLAGS) $(LFLAGS) $^ $(LDLIBS) -o $(BIN_DIR)/$@$(EXE)

magpie_test: $(OBJ_SRC) $(OBJ_TEST) $(RES_OBJ) | $(BIN_DIR)
	$(CC) $(LDFLAGS) $(LFLAGS) $^ $(LDLIBS) -o $(BIN_DIR)/$@$(EXE)

$(RES_OBJ): windows/magpie.rc windows/magpie.manifest windows/magpie.ico $(SRC_DIR)/impl/config.c | $(OBJ_DIR)
	$(WINDRES) -I windows -DMAGPIE_VERSION_TEXT=$(MAGPIE_VERSION) -DMAGPIE_VERSION_COMMAS=$(MAGPIE_VERSION_COMMAS) $< -o $@

magpie_pgo_train: $(OBJ_SRC) $(PGO_TRAIN_OBJ) | $(BIN_DIR)
	$(CC) $(LDFLAGS) $(LFLAGS) $^ $(LDLIBS) -o $(BIN_DIR)/$@

word_frequencies: $(OBJ_SRC) $(OBJ_DIR)/$(TOOLS_DIR)/word_frequencies.o | $(BIN_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(LFLAGS) $^ $(LDLIBS) -o $(BIN_DIR)/word_frequencies

magpie_convert: $(OBJ_SRC) $(CONVERT_OBJ) | $(BIN_DIR)
	$(CC) $(LDFLAGS) $(LFLAGS) $^ $(LDLIBS) -o $(BIN_DIR)/$@

$(OBJ_DIR)/$(SRC_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR) $(OBJ_DIR)/$(SRC_DIR) $(SRC_OBJ_SUBDIRS)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

# The training executable and CLI executable have different `main` functions.
# Keep the CLI entry point out of profile use so its shared symbol name is not
# mistaken for stale profile data; all shared Magpie code remains profiled.
$(OBJ_DIR)/$(CMD_DIR)/%.o: $(CMD_DIR)/%.c | $(OBJ_DIR) $(OBJ_DIR)/$(CMD_DIR)
	$(CC) $(CMD_CFLAGS) $(DEPFLAGS) -c $< -o $@

$(OBJ_DIR)/$(TOOLS_DIR)/%.o: $(TOOLS_DIR)/%.c | $(OBJ_DIR) $(OBJ_DIR)/$(TOOLS_DIR)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

# Compiler and linker flag changes in this file invalidate every object.
$(OBJ_SRC) $(OBJ_CMD) $(OBJ_TEST) $(PGO_TRAIN_OBJ) $(CONVERT_OBJ) $(OBJ_DIR)/$(TOOLS_DIR)/word_frequencies.o: Makefile

# A newly merged profile must rebuild every profile-use object. Without this
# dependency, make would reuse objects optimized against an older corpus.
ifeq ($(BUILD),pgo_use)
$(OBJ_SRC) $(OBJ_CMD) $(OBJ_TEST): $(PGO_PROFILE)
endif

# Optimized test builds keep assertions enabled.
$(OBJ_DIR)/$(TEST_DIR)/%.o: $(TEST_DIR)/%.c | $(OBJ_DIR) $(OBJ_DIR)/$(TEST_DIR) $(TEST_OBJ_SUBDIRS)
	$(CC) $(if $(filter no_pgo_release portable_release bai_stats pgo_generate pgo_use,$(BUILD)),${cflags.test_$(BUILD)},$(CFLAGS)) $(DEPFLAGS) -DBOARD_DIM=$(BOARD_DIM) -DRACK_SIZE=$(RACK_SIZE) -c $< -o $@

$(BIN_DIR) $(OBJ_DIR) $(OBJ_DIR)/$(SRC_DIR) $(OBJ_DIR)/$(CMD_DIR) $(OBJ_DIR)/$(TEST_DIR) $(OBJ_DIR)/$(TOOLS_DIR) $(SRC_OBJ_SUBDIRS) $(TEST_OBJ_SUBDIRS):
	mkdir -p $@

clean:
	@$(RM) -rv $(BIN_DIR) $(OBJ_ROOT) libmagpie_core.a

# Windows executables, cross-compiled in the pinned llvm-mingw image
# (windows/Dockerfile), so any developer with Docker can build and test them:
#   make windows                       bin/magpie.exe, x86_64
#   make windows WINDOWS_ARCH=aarch64  bin/magpie.exe, arm64
#   make windows_test                  bin/magpie_test.exe
#   make windows_wine_test TESTS=compat  run magpie_test.exe under Wine (x86_64)
# Both use the portable build: the same instruction set as contributor
# binaries (nehalem on x86_64; armv8-a, which every Windows on ARM device
# meets, on arm64), so derived files hash the same as Linux builds for it.
WINDOWS_ARCH ?= x86_64
WINDOWS_IMAGE ?= magpie-llvm-mingw:20261006
ifeq ($(WINDOWS_ARCH),aarch64)
WINDOWS_MARCH := armv8-a
else
WINDOWS_MARCH := nehalem
endif
WINDOWS_MAKE := docker run --rm -u "$$(id -u):$$(id -g)" -e HOME=/tmp/wine-home \
	-v "$(CURDIR)":/src -w /src $(WINDOWS_IMAGE) make BUILD=portable_release \
	PORTABLE_MARCH=$(WINDOWS_MARCH) CC=$(WINDOWS_ARCH)-w64-mingw32-clang

windows_image:
	docker build -q -t $(WINDOWS_IMAGE) windows

windows: windows_image
	$(WINDOWS_MAKE) magpie

windows_test: windows_image
	$(WINDOWS_MAKE) magpie_test

windows_wine_test: windows_test
	docker run --rm -u "$$(id -u):$$(id -g)" -e HOME=/tmp/wine-home -e WINEDEBUG=-all \
		-v "$(CURDIR)":/src -w /src $(WINDOWS_IMAGE) \
		sh -c 'mkdir -p "$$HOME" && wine bin/magpie_test.exe $(TESTS)'

# The production release is trained on static autoplay, the best general
# profile in the measured workload matrix. Without clang there is nothing to
# train with, so build the plain optimized release rather than fail.
release:
	@if test -n "$(PGO_CC_USABLE)"; then \
		$(MAKE) pgo_workload PGO_WORKLOAD=static; \
	else \
		if test -n "$(PGO_CC_FOUND_MAJOR)"; then \
			echo '*** $(PGO_CC_FOUND) is clang $(PGO_CC_FOUND_MAJOR); the profile-guided release needs clang $(PGO_MIN_CLANG_MAJOR) or newer. Building the plain optimized release instead.'; \
		elif test -n "$(PGO_CC_FOUND)"; then \
			echo '*** $(PGO_CC_FOUND) is not clang; the profile-guided release needs clang $(PGO_MIN_CLANG_MAJOR) or newer. Building the plain optimized release instead.'; \
		else \
			echo '*** No clang on PATH for the profile-guided release; building the plain optimized release instead.'; \
		fi; \
		echo '*** Install clang $(PGO_MIN_CLANG_MAJOR) or newer (Debian/Ubuntu: apt.llvm.org), or pass PGO_CC=<clang> LLVM_PROFDATA=<llvm-profdata>, to train the PGO build.'; \
		$(MAKE) magpie BUILD=no_pgo_release; \
	fi

# Leave generation benefits from its own focused profile.
leavegen_pgo_release:
	$(MAKE) pgo_workload PGO_WORKLOAD=leavegen

# Build a balanced profile using fast PlayChooser games. The chooser sims in
# the midgame, runs PEG with 1..4 tiles in the bag, and solves the endgame.
pgo:
	$(MAKE) pgo_workload PGO_WORKLOAD=general

# Focused profiles for long-running workloads. These all replace bin/magpie
# with a fresh build optimized for the selected workload.
pgo_sim:
	$(MAKE) pgo_workload PGO_WORKLOAD=sim

pgo_peg:
	$(MAKE) pgo_workload PGO_WORKLOAD=peg

pgo_eg:
	$(MAKE) pgo_workload PGO_WORKLOAD=eg

# Accept the originally proposed endgame spelling as an alias.
peg_eg: pgo_eg

# Refuse to start a PGO build without its toolchain, instead of failing on the
# first object file. The instrumentation flags are clang's, so PGO_CC must be
# some clang; LLVM_PROFDATA may carry an `xcrun` prefix.
pgo_toolchain_check:
	@if test -z "$(PGO_CC)"; then \
		echo '*** PGO needs clang $(PGO_MIN_CLANG_MAJOR) or newer and none was found on PATH (clang, clang-20 .. clang-15).'; \
		echo '*** Install clang, or pass PGO_CC=<clang> LLVM_PROFDATA=<llvm-profdata>.'; \
		exit 1; \
	fi
	@if ! command -v "$(PGO_CC)" >/dev/null 2>&1; then \
		echo '*** PGO_CC=$(PGO_CC) was not found on PATH.'; exit 1; \
	fi
	@if ! "$(PGO_CC)" --version 2>/dev/null | grep -qi clang; then \
		echo '*** PGO_CC=$(PGO_CC) is not clang; the PGO instrumentation flags are clang-specific.'; exit 1; \
	fi
	@if test -z "$(PGO_CC_USABLE)"; then \
		echo '*** PGO_CC=$(PGO_CC) is clang $(PGO_CC_FOUND_MAJOR); PGO needs clang $(PGO_MIN_CLANG_MAJOR) or newer (Debian/Ubuntu: apt.llvm.org, e.g. PGO_CC=clang-18 LLVM_PROFDATA=llvm-profdata-18).'; exit 1; \
	fi
	@if test -z "$(LLVM_PROFDATA)" || ! command -v $(firstword $(LLVM_PROFDATA)) >/dev/null 2>&1; then \
		echo '*** PGO needs llvm-profdata (llvm-profdata, llvm-profdata-20 .. -15, or xcrun on macOS); pass LLVM_PROFDATA=<path>.'; exit 1; \
	fi

# Validate or upgrade the production WIT with the native C converter. This
# separate uninstrumented executable cannot load stale tables through engine
# configuration, and a conversion error exits nonzero before training begins.
prepare_data:
	$(MAKE) magpie_convert BUILD=no_pgo_release
	./$(BIN_DIR)/magpie_convert kwg2witifneeded CSW24 ./data
	@if test -f data/lexica/CSW24.rit; then \
		echo 'Using existing data/lexica/CSW24.rit'; \
	else \
		./$(BIN_DIR)/magpie_convert klvwmp2rit CSW24 ./data; \
	fi

# Prepare the production RIT and WIT, discard all previous profile data,
# train a freshly instrumented production engine, and replace bin/magpie with
# the profile-guided native build. The dedicated driver contains no benchmark
# harness; it invokes real engine workloads directly.
pgo_workload: pgo_toolchain_check
	$(MAKE) prepare_data CC="$(PGO_CC)" LDFLAGS="-pthread -flto $(PGO_LDFLAGS)"
	$(RM) -r $(PGO_RAW_DIR) $(PGO_PROFILE)
	mkdir -p $(PGO_RAW_DIR)
	@if test "$(PGO_WORKLOAD)" = leavegen; then \
		$(RM) -r "$(PGO_LEAVEGEN_DATA_DIR)"; \
		mkdir -p "$(PGO_LEAVEGEN_DATA_DIR)/lexica"; \
	fi
	$(MAKE) -B magpie_pgo_train BUILD=pgo_generate PGO_PROFILE=$(PGO_PROFILE) CC="$(PGO_CC)"
	LLVM_PROFILE_FILE="$(abspath $(PGO_RAW_DIR))/magpie-%p.profraw" \
		./$(BIN_DIR)/magpie_pgo_train \
			"$(PGO_WORKLOAD)" \
			"$(PGO_TRAIN_GAMES)" \
			"$(PGO_TRAIN_TIME_MS)" \
			"$(PGO_TRAIN_SECONDS)" \
			"$(PGO_TRAIN_THREADS)" \
			"$(PGO_LEAVEGEN_DATA_DIR):./data" \
			"$(PGO_LEAVEGEN_TARGET)"
	$(LLVM_PROFDATA) merge -sparse -o $(PGO_PROFILE) $(PGO_RAW_DIR)/*.profraw
	$(MAKE) -B magpie \
		BUILD=pgo_use \
		PGO_PROFILE="$(PGO_PROFILE)" \
		CC="$(PGO_CC)" \
		PGO_LDFLAGS="$(PGO_LDFLAGS)"

-include $(OBJ_SRC:.o=.d)
-include $(OBJ_CMD:.o=.d)
-include $(OBJ_TEST:.o=.d)
-include $(PGO_TRAIN_OBJ:.o=.d)
-include $(CONVERT_OBJ:.o=.d)

-include $(OBJ_DIR)/$(TOOLS_DIR)/word_frequencies.d
