#include "runtime/php_assert.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cxxabi.h>
#include <execinfo.h>
#include <unistd.h>
#include <wait.h>

#include "common/fast-backtrace.h"

#include "runtime/critical_section.h"
#include "runtime/kphp_backtrace.h"
#include "runtime/on_kphp_warning_callback.h"
#include "runtime/resumable.h"
#include "PHP/worker/php-engine-vars.h"

const char *engine_tag = "[";
const char *engine_pid = "] ";

int php_disable_warnings = 0;
int php_warning_level = 2;

// linker magic: run_scheduler function is declared in separate section.
// their addresses could be used to check if address is inside run_scheduler
struct nothing {};
extern nothing __start_run_scheduler_section;
extern nothing __stop_run_scheduler_section;
static bool is_address_inside_run_scheduler(void *address) {
  return &__start_run_scheduler_section <= address && address <= &__stop_run_scheduler_section;
};

static void print_demangled_adresses(void **buffer, int nptrs, int num_shift) {
  if (php_warning_level == 1) {
    for (int i = 0; i < nptrs; i++) {
      fprintf(stderr, "%p\n", buffer[i]);
    }
  } else if (php_warning_level == 2) {
    bool was_printed = get_demangled_backtrace(buffer, nptrs, num_shift, [](const char *, const char *trace_str) {
      fprintf(stderr, "%s", trace_str);
    });
    if (!was_printed) {
      backtrace_symbols_fd(buffer, nptrs, 2);
    }
  } else if (php_warning_level == 3) {
    char pid_buf[30];
    sprintf(pid_buf, "%d", getpid());
    char name_buf[512];
    ssize_t res = readlink("/proc/self/exe", name_buf, 511);
    if (res >= 0) {
      name_buf[res] = 0;
      int child_pid = fork();
      if (!child_pid) {
        dup2(2, 1); //redirect output to stderr
        execlp("gdb", "gdb", "--batch", "-n", "-ex", "thread", "-ex", "bt", name_buf, pid_buf, nullptr);
        fprintf(stderr, "Can't print backtrace with gdb: gdb failed to start\n");
      } else {
        if (child_pid > 0) {
          waitpid(child_pid, nullptr, 0);
        } else {
          fprintf(stderr, "Can't print backtrace with gdb: fork failed\n");
        }
      }
    } else {
      fprintf(stderr, "Can't print backtrace with gdb: can't get name of executable file\n");
    }
  }
}

void php_warning(char const *message, ...) {
  if (php_warning_level == 0 || php_disable_warnings) {
    return;
  }
  static const int BUF_SIZE = 1000;
  static char buf[BUF_SIZE];
  static const int warnings_time_period = 300;
  static const int warnings_time_limit = 1000;

  static int warnings_printed = 0;
  static int warnings_count_time = 0;
  static int skipped = 0;
  int cur_time = (int)time(nullptr);

  if (cur_time >= warnings_count_time + warnings_time_period) {
    warnings_printed = 0;
    warnings_count_time = cur_time;
    if (skipped > 0) {
      fprintf(stderr, "[time=%d] Resuming writing warnings: %d skipped\n", (int)time(nullptr), skipped);
      skipped = 0;
    }
  }

  if (++warnings_printed >= warnings_time_limit) {
    if (warnings_printed == warnings_time_limit) {
      fprintf(stderr, "[time=%d] Warnings limit reached. No more will be printed till %d\n", cur_time, warnings_count_time + warnings_time_period);
    }
    ++skipped;
    return;
  }

  dl::enter_critical_section();//OK

  va_list args;
  va_start (args, message);

  fprintf(stderr, "%s%d%sWarning: ", engine_tag, cur_time, engine_pid);
  vsnprintf(buf, BUF_SIZE, message, args);
  fprintf(stderr, "%s\n", buf);
  va_end (args);

  if (php_warning_level >= 1) {
    fprintf(stderr, "------- Stack Backtrace -------\n");
    void *buffer[64];
    int nptrs = fast_backtrace(buffer, sizeof(buffer) / sizeof(buffer[0]));
    if (php_warning_level == 1) {
      nptrs -= 2;
      if (nptrs < 0) {
        nptrs = 0;
      }
    }

    int scheduler_id = std::find_if(buffer, buffer + nptrs, is_address_inside_run_scheduler) - buffer;
    if (scheduler_id == nptrs) {
      print_demangled_adresses(buffer, nptrs, 0);
    } else {
      print_demangled_adresses(buffer, scheduler_id, 0);
      void *buffer2[64];
      int res_ptrs = get_resumable_stack(buffer2, sizeof(buffer2) / sizeof(buffer2[0]));
      print_demangled_adresses(buffer2, res_ptrs, scheduler_id);
      print_demangled_adresses(buffer + scheduler_id, nptrs - scheduler_id, scheduler_id + res_ptrs);
    }

    fprintf(stderr, "-------------------------------\n\n");
  }

  dl::leave_critical_section();
  if (!dl::in_critical_section) {
    OnKphpWarningCallback::get().invoke_callback(string(buf));
  }
  if (die_on_fail) {
    raise(SIGPHPASSERT);
    fprintf(stderr, "_exiting in php_warning, since such option is enabled\n");
    _exit(1);
  }
}

void php_assert__(const char *msg, const char *file, int line) {
  php_warning("Assertion \"%s\" failed in file %s on line %d", msg, file, line);
  raise(SIGPHPASSERT);
  fprintf(stderr, "_exiting in php_assert\n");
  _exit(1);
}

void raise_php_assert_signal__() {
  raise(SIGPHPASSERT);
}