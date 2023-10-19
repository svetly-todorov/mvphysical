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
#include <thread>

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

// #define ERROR(msg, ...) (mplog.Msg(0, msg, ##__VA_ARGS__))
// #define DEBUG(msg, ...) (mplog.Msg(0, msg, ##__VA_ARGS__))
// #define INFO(msg, ...) (mplog.Msg(1, msg, ##__VA_ARGS__))

#endif