#include <stdarg.h>
#include "logging.h"

void test_logging_msg(FILE *f, const char *p, unsigned line, const char *func,
		      const char *fmt, ...) {
  fprintf(f, "%s: %d (%s:%d): > ", p, _test_myrank, func, line);
  va_list args;
  va_start(args, fmt);
  vfprintf(f, fmt, args);
  va_end(args);
  fprintf(f, "\n");
  fflush(f);
}
