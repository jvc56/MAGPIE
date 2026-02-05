#ifndef COMPAT_MEMORY_INFO_H
#define COMPAT_MEMORY_INFO_H

#include <errno.h>
#include <stdint.h>
#include <stdio.h>

#include "../util/io_util.h"

// Platform-specific includes
#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#elif defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#elif defined(__APPLE__) || defined(__MACH__)
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#elif defined(__linux__) || defined(__unix__) || defined(__posix__)
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#endif

static inline uint64_t get_total_memory(void) {
  uint64_t total_memory = 0;

#if defined(__EMSCRIPTEN__)
  // WASM/Emscripten implementation
  // Return the heap size via EM_ASM
  // cppcheck-suppress-begin syntaxError
  total_memory = EM_ASM_INT({
    return HEAP8.length;
  });
  // cppcheck-suppress-end

#elif defined(_WIN32) || defined(_WIN64)
  // Windows implementation
  MEMORYSTATUSEX statex;
  statex.dwLength = sizeof(statex);
  if (GlobalMemoryStatusEx(&statex)) {
    total_memory = statex.ullTotalPhys;
  } else {
    log_fatal("error retrieving memory info on Windows");
  }

#elif defined(__APPLE__) || defined(__MACH__)
  // macOS implementation
  int64_t memsize;
  size_t len = sizeof(memsize);
  if (sysctlbyname("hw.memsize", &memsize, &len, NULL, 0) == 0) {
    total_memory = memsize;
  } else {
    perror("sysctlbyname failed");
  }

#elif defined(__linux__)
  // Linux implementation
  FILE *file = fopen_or_die("/proc/meminfo", "r");
  if (!file) {
    perror("Unable to open /proc/meminfo");
    return 0;
  }

  char line[256];
  while (fgets(line, sizeof(line), file)) {
    if (strncmp(line, "MemTotal:", 9) == 0) {
      char *endptr;
      errno = 0;
      unsigned long memtotal_kb = strtoul(line + 9, &endptr, 10);

      // Check for conversion errors
      if (errno != 0 || endptr == line + 9) {
        log_fatal("error parsing memtotal");
      }

      // Skip whitespace after the number
      while (*endptr == ' ' || *endptr == '\t') {
        endptr++;
      }

      // Verify we found "kB" (case insensitive check)
      if (strncasecmp(endptr, "kB", 2) != 0) {
        log_fatal("expected 'kB' unit in meminfo");
      }

      total_memory = memtotal_kb * 1024; // Convert from KB to Bytes
      break;
    }
  }
  fclose_or_die(file);
#else
  log_fatal("unsupported platform");
#endif

  return total_memory;
}

static inline int get_num_cores(void) {
  int core_count = 1; // Default fallback value

#if defined(__EMSCRIPTEN__)
  // WASM/Emscripten: Use navigator.hardwareConcurrency via JS
  // For now, return a reasonable default
  // cppcheck-suppress-begin syntaxError
  core_count = EM_ASM_INT({
    return (typeof navigator !== 'undefined' && navigator.hardwareConcurrency)
           ? navigator.hardwareConcurrency
           : 4;
  });
  // cppcheck-suppress-end

#elif defined(_WIN32) || defined(_WIN64)
                      // === Implementation for Windows ===
  SYSTEM_INFO sys_info;
  GetSystemInfo(&sys_info);
  core_count = (int)sys_info.dwNumberOfProcessors;
  // The core_count will be at least 1, as GetSystemInfo will succeed.

#elif defined(__APPLE__) || defined(__MACH__)
                      // === Implementation for macOS (Darwin) ===
  int name[] = {CTL_HW, HW_NCPU};
  size_t size = sizeof(core_count);

  if (sysctl(name, 2, &core_count, &size, NULL, 0) == -1) {
    // sysctl failed, return default
    core_count = 1;
  }

#elif defined(__linux__) || defined(__unix__) || defined(__posix__)
                      // === Implementation for Linux and other POSIX-compatible
                      // systems ===
  long num_cores = sysconf(_SC_NPROCESSORS_ONLN);
  if (num_cores > 0) {
    core_count = (int)num_cores;
  }

#else
                      // If the OS is not recognized, a warning is printed and
                      // the default value (1) is used.
  fprintf(stderr, "Warning: Unknown operating system. Returning 1 core.\n");
  core_count = 1;

#endif

  return core_count;
}

#endif