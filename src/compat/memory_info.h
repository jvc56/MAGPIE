#ifndef COMPAT_MEMORY_INFO_H
#define COMPAT_MEMORY_INFO_H

#include <errno.h>
#include <stdint.h>
#include <stdio.h>

#include "../util/io_util.h"

// Platform-specific includes
#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#elif defined(__APPLE__) || defined(__MACH__)
#include <sys/sysctl.h>
#include <sys/types.h>
#elif defined(__linux__)
#include <stdlib.h>
#include <string.h>
#endif

static uint64_t get_total_memory(void) {
  uint64_t total_memory = 0;

#if defined(_WIN32) || defined(_WIN64)
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

// int main() {
//     uint64_t memory = get_total_memory();
//     if (memory > 0) {
//         printf("Total Physical Memory: %llu MB\n", memory / (1024 * 1024));
//     } else {
//         printf("Failed to retrieve total memory.\n");
//     }
//     return 0;
// }

#endif