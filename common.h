#ifndef COMMON_H
#define COMMON_H

#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <chrono>

static inline void error(int err, const char *format, ...) {
  va_list v_args;
  va_start(v_args, format);
  vprintf(format, v_args);
  va_end(v_args);
  exit(err);
}

static FILE *out = stdout;
static inline void print(const char *format, ...) {
  va_list v_args;
  va_start(v_args, format);
  vfprintf(out, format, v_args);
  va_end(v_args);
}

static inline void fancy(const char *name, const char *format, ...) {
  /* Print the function name */
  int len = strlen(name);
  if (len > 13) fprintf(out, "[%.10s...] ", name);
  else fprintf(out, "[%.13s] ", name);
  /* Print tabs to pad to 16 characters */
  // len = 16 - len;
  // do {
  //   fprintf(out, "\t");
  //   len -= 8;
  // } while (len > 0);
  /* Print message contents */
  va_list v_args;
  va_start(v_args, format);
  vfprintf(out, format, v_args);
  va_end(v_args);
}

static inline void fancy_err(int err, const char *name, const char *format, ...) {
  va_list v_args;
  va_start(v_args, format);
  fancy(name, format, v_args);
  va_end(v_args);
  exit(err);
}

#define PRINT(msg, ...) (fancy(__func__, msg, ##__VA_ARGS__))
#define ERROR(err, msg, ...) (fancy_err(err, __func__, msg, ##__VA_ARGS__))

#define TIME(cmd) {                          \
  size_t ms = util::GetTimeMs();             \
  cmd;                                       \
  PRINT("[%lu ms]\n", util::GetNumMs(ms));   \
  PRINT("--------\n");                       \
  }

#define BREAK() {                            \
  PRINT("--------\n");                       \
  PRINT("        \n");                       \
  PRINT("--------\n");                       \
  }

#endif