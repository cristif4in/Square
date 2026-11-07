/* Minimal pthread shim over Win32 primitives for QuickJS on Windows. */
#ifndef SQRL_PTHREAD_SHIM_H
#define SQRL_PTHREAD_SHIM_H
#include <windows.h>
#include <time.h>

#define PTHREAD_MUTEX_INITIALIZER {{{0}}}
typedef CRITICAL_SECTION pthread_mutex_t;
/* CRITICAL_SECTION supports static zero-init */
typedef void pthread_mutexattr_t;
typedef CONDITION_VARIABLE pthread_cond_t;
typedef void pthread_condattr_t;

static inline int pthread_mutex_init(pthread_mutex_t* m, pthread_mutexattr_t* a) {
  InitializeCriticalSection(m);
  return 0;
}
static inline int pthread_mutex_destroy(pthread_mutex_t* m) {
  DeleteCriticalSection(m);
  return 0;
}
static inline int pthread_mutex_lock(pthread_mutex_t* m) {
  EnterCriticalSection(m);
  return 0;
}
static inline int pthread_mutex_unlock(pthread_mutex_t* m) {
  LeaveCriticalSection(m);
  return 0;
}
static inline int pthread_cond_init(pthread_cond_t* c, pthread_condattr_t* a) {
  InitializeConditionVariable(c);
  return 0;
}
static inline int pthread_cond_destroy(pthread_cond_t*) { return 0; }
static inline int pthread_cond_signal(pthread_cond_t* c) {
  WakeConditionVariable(c);
  return 0;
}
static inline int pthread_cond_wait(pthread_cond_t* c, pthread_mutex_t* m) {
  SleepConditionVariableCS(c, m, INFINITE);
  return 0;
}
/* clock_gettime shim (MSVCRT lacks it on older targets) */
#ifndef SQRL_CLOCK_SHIM
#define SQRL_CLOCK_SHIM
#include <sys/timeb.h>
#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif
static inline int clock_gettime(int clk, struct timespec* ts) {
  (void)clk;
  struct _timeb tb;
  _ftime_s(&tb);
  ts->tv_sec = (time_t)tb.time;
  ts->tv_nsec = tb.millitm * 1000000L;
  return 0;
}
#endif

static inline int pthread_cond_timedwait(pthread_cond_t* c, pthread_mutex_t* m,
                                         const struct timespec* abstime) {
  /* abstime is CLOCK_REALTIME absolute; convert to relative ms */
  struct timespec now;
  timespec_get(&now, TIME_UTC);
  long long ms = (abstime->tv_sec - now.tv_sec) * 1000LL +
                 (abstime->tv_nsec - now.tv_nsec) / 1000000LL;
  if (ms < 0) ms = 0;
  SleepConditionVariableCS(c, m, (DWORD)ms);
  return 0; /* treat timeout as signal; QuickJS re-checks predicates */
}
#endif
