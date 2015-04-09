// Copyright (c) 2013, Cloudera,inc.
// Confidential Cloudera Information: Covered by NDA.

#include "kudu/util/debug-util.h"

#include <execinfo.h>
#include <dirent.h>
#include <glog/logging.h>
#include <signal.h>
#include <string>
#include <sys/syscall.h>

#include "kudu/gutil/macros.h"
#include "kudu/gutil/spinlock.h"
#include "kudu/gutil/stringprintf.h"
#include "kudu/gutil/strings/numbers.h"
#include "kudu/util/env.h"
#include "kudu/util/errno.h"
#include "kudu/util/monotime.h"

// Evil hack to grab a few useful functions from glog
namespace google {

extern int GetStackTrace(void** result, int max_depth, int skip_count);

// Symbolizes a program counter.  On success, returns true and write the
// symbol name to "out".  The symbol name is demangled if possible
// (supports symbols generated by GCC 3.x or newer).  Otherwise,
// returns false.
bool Symbolize(void *pc, char *out, int out_size);

namespace glog_internal_namespace_ {
extern void DumpStackTraceToString(std::string *s);
} // namespace glog_internal_namespace_
} // namespace google

// The %p field width for printf() functions is two characters per byte.
// For some environments, add two extra bytes for the leading "0x".
static const int kPrintfPointerFieldWidth = 2 + 2 * sizeof(void*);

namespace kudu {

namespace {

// Global structure used to communicate between the signal handler
// and a dumping thread.
struct SignalCommunication {
  // The actual stack trace collected from the target thread.
  StackTrace stack;

  // The current target. Signals can be delivered asynchronously, so the
  // dumper thread sets this variable first before sending a signal. If
  // a signal is received on a thread that doesn't match 'target_tid', it is
  // ignored.
  pid_t target_tid;

  // Set to 1 when the target thread has successfully collected its stack.
  // The dumper thread spins waiting for this to become true.
  Atomic32 result_ready;

  // Lock protecting the other members. We use a bare atomic here and a custom
  // lock guard below instead of existing spinlock implementaitons because futex()
  // is not signal-safe.
  Atomic32 lock;

  struct Lock;
};
SignalCommunication g_comm;

// Pared-down SpinLock for SignalCommunication::lock. This doesn't rely on futex
// so it is async-signal safe.
struct SignalCommunication::Lock {
  Lock() {
    while (base::subtle::Acquire_CompareAndSwap(&g_comm.lock, 0, 1) != 0) {
      sched_yield();
    }
  }
  ~Lock() {
    base::subtle::Release_Store(&g_comm.lock, 0);
  }
};

// Signal handler for SIGUSR1.
// We expect that the signal is only sent from DumpThreadStack() -- not by a user.
void HandleStackTraceSignal(int signum) {
  SignalCommunication::Lock l;

  // Check that the dumper thread is still interested in our stack trace.
  // It's possible for signal delivery to be artificially delayed, in which
  // case the dumper thread would have already timed out and moved on with
  // its life. In that case, we don't want to race with some other thread's
  // dump.
  pid_t my_tid = syscall(SYS_gettid);
  if (g_comm.target_tid != my_tid) {
    return;
  }

  g_comm.stack.Collect(2);
  base::subtle::Release_Store(&g_comm.result_ready, 1);
}

} // namespace

std::string DumpThreadStack(pid_t tid) {
  // We only allow a single dumper thread to run at a time. This simplifies the synchronization
  // between the dumper and the target thread.
  static base::SpinLock dumper_thread_lock_(base::LINKER_INITIALIZED);
  base::SpinLockHolder h(&dumper_thread_lock_);

  // Ensure that our SIGUSR1 handler is installed. We don't need any fancy GoogleOnce here
  // because of the mutex above.
  static bool initialized = false;
  if (!initialized) {
    PCHECK(signal(SIGUSR1, HandleStackTraceSignal) == 0);
    initialized = true;
  }

  // Set the target TID in our communication structure, so if we end up with any
  // delayed signal reaching some other thread, it will know to ignore it.
  {
    SignalCommunication::Lock l;
    CHECK_EQ(0, g_comm.target_tid);
    g_comm.target_tid = tid;
  }

  // We use the raw syscall here instead of kill() to ensure that we don't accidentally
  // send a signal to some other process in the case that the thread has exited and
  // the TID been recycled.
  if (syscall(SYS_tgkill, getpid(), tid, SIGUSR1) != 0) {
    {
      SignalCommunication::Lock l;
      g_comm.target_tid = 0;
    }
    return "(unable to deliver signal: process may have exited)";
  }

  // We give the thread ~1s to respond. In testing, threads typically respond within
  // a few iterations of the loop, so this timeout is very conservative.
  //
  // The main reason that a thread would not respond is that it has blocked signals. For
  // example, glibc's timer_thread doesn't respond to our signal, so we always time out
  // on that one.
  string ret;
  int i = 0;
  while (!base::subtle::Acquire_Load(&g_comm.result_ready) &&
         i++ < 100) {
    SleepFor(MonoDelta::FromMilliseconds(10));
  }

  {
    SignalCommunication::Lock l;
    CHECK_EQ(tid, g_comm.target_tid);

    if (!g_comm.result_ready) {
      ret = "(thread did not respond: maybe it is blocking signals)";
    } else {
      ret = g_comm.stack.Symbolize();
    }

    g_comm.target_tid = 0;
    g_comm.result_ready = 0;
  }
  return ret;
}

Status ListThreads(vector<pid_t> *tids) {
  DIR *dir = opendir("/proc/self/task/");
  if (dir == NULL) {
    return Status::IOError("failed to open task dir", ErrnoToString(errno), errno);
  }
  struct dirent *d;
  while ((d = readdir(dir)) != NULL) {
    if (d->d_name[0] != '.') {
      uint32_t tid;
      if (!safe_strtou32(d->d_name, &tid)) {
        LOG(WARNING) << "bad tid found in procfs: " << d->d_name;
        continue;
      }
      tids->push_back(tid);
    }
  }
  closedir(dir);
  return Status::OK();
}

std::string GetStackTrace() {
  std::string s;
  google::glog_internal_namespace_::DumpStackTraceToString(&s);
  return s;
}

std::string GetStackTraceHex() {
  char buf[1024];
  HexStackTraceToString(buf, 1024);
  return std::string(buf);
}

void HexStackTraceToString(char* buf, size_t size) {
  StackTrace trace;
  trace.Collect(1);
  trace.StringifyToHex(buf, size);
}

void StackTrace::Collect(int skip_frames) {
  num_frames_ = google::GetStackTrace(frames_, arraysize(frames_), skip_frames);
}

void StackTrace::StringifyToHex(char* buf, size_t size) const {
  char* dst = buf;

  // Reserve kHexEntryLength for the first iteration of the loop, 1 byte for a
  // space (which we may not need if there's just one frame), and 1 for a nul
  // terminator.
  char* limit = dst + size - kHexEntryLength - 2;
  for (int i = 0; i < num_frames_ && dst < limit; i++) {
    if (i != 0) {
      *dst++ = ' ';
    }
    FastHex64ToBuffer(reinterpret_cast<uintptr_t>(frames_[i]), dst);
    dst += kHexEntryLength;
  }
  *dst = '\0';
}

string StackTrace::ToHexString() const {
  // Each frame requires kHexEntryLength, plus a space
  // We also need one more byte at the end for '\0'
  char buf[kMaxFrames * (kHexEntryLength + 1) + 1];
  StringifyToHex(buf, arraysize(buf));
  return string(buf);
}

// Symbolization function borrowed from glog.
string StackTrace::Symbolize() const {
  string ret;
  for (int i = 0; i < num_frames_; i++) {
    void* pc = frames_[i];

    char tmp[1024];
    const char* symbol = "(unknown)";

    // The return address 'pc' on the stack is the address of the instruction
    // following the 'call' instruction. In the case of calling a function annotated
    // 'noreturn', this address may actually be the first instruction of the next
    // function, because the function we care about ends with the 'call'.
    // So, we subtract 1 from 'pc' so that we're pointing at the 'call' instead
    // of the return address.
    //
    // For example, compiling a C program with -O2 that simply calls 'abort()' yields
    // the following disassembly:
    //     Disassembly of section .text:
    //
    //     0000000000400440 <main>:
    //       400440:	48 83 ec 08          	sub    $0x8,%rsp
    //       400444:	e8 c7 ff ff ff       	callq  400410 <abort@plt>
    //
    //     0000000000400449 <_start>:
    //       400449:	31 ed                	xor    %ebp,%ebp
    //       ...
    //
    // If we were to take a stack trace while inside 'abort', the return pointer
    // on the stack would be 0x400449 (the first instruction of '_start'). By subtracting
    // 1, we end up with 0x400448, which is still within 'main'.
    if (google::Symbolize(
            reinterpret_cast<char *>(pc) - 1, tmp, sizeof(tmp))) {
      symbol = tmp;
    }
    StringAppendF(&ret, "    @ %*p  %s\n", kPrintfPointerFieldWidth, pc, symbol);
  }
  return ret;
}

}  // namespace kudu
