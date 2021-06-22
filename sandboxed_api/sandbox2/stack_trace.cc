// Copyright 2019 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Implementation of the sandbox2::StackTrace class.

#include "sandboxed_api/sandbox2/stack_trace.h"

#include <sys/resource.h>
#include <syscall.h>

#include <memory>
#include <utility>
#include <vector>

#include <glog/logging.h>
#include "sandboxed_api/util/flag.h"
#include "absl/memory/memory.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/strip.h"
#include "libcap/include/sys/capability.h"
#include "sandboxed_api/config.h"
#include "sandboxed_api/sandbox2/comms.h"
#include "sandboxed_api/sandbox2/executor.h"
#include "sandboxed_api/sandbox2/ipc.h"
#include "sandboxed_api/sandbox2/limits.h"
#include "sandboxed_api/sandbox2/policy.h"
#include "sandboxed_api/sandbox2/policybuilder.h"
#include "sandboxed_api/sandbox2/regs.h"
#include "sandboxed_api/sandbox2/result.h"
#include "sandboxed_api/sandbox2/sandbox2.h"
#include "sandboxed_api/sandbox2/unwind/unwind.h"
#include "sandboxed_api/sandbox2/unwind/unwind.pb.h"
#include "sandboxed_api/sandbox2/util/bpf_helper.h"
#include "sandboxed_api/util/fileops.h"
#include "sandboxed_api/util/path.h"

ABSL_FLAG(bool, sandbox_disable_all_stack_traces, false,
          "Completely disable stack trace collection for sandboxees");

ABSL_FLAG(bool, sandbox_libunwind_crash_handler, true,
          "Sandbox libunwind when handling violations (preferred)");

namespace sandbox2 {

namespace file = ::sapi::file;
namespace file_util = ::sapi::file_util;

class StackTracePeer {
 public:
  static std::unique_ptr<Policy> GetPolicy(pid_t target_pid,
                                           const std::string& maps_file,
                                           const std::string& app_path,
                                           const std::string& exe_path,
                                           const Mounts& mounts);

  static bool LaunchLibunwindSandbox(const Regs* regs, const Mounts& mounts,
                                     UnwindResult* result);
};

std::unique_ptr<Policy> StackTracePeer::GetPolicy(pid_t target_pid,
                                                  const std::string& maps_file,
                                                  const std::string& app_path,
                                                  const std::string& exe_path,
                                                  const Mounts& mounts) {
  PolicyBuilder builder;
  builder
      // Use the mounttree of the original executable as starting point.
      .SetMounts(mounts)
      .AllowOpen()
      .AllowRead()
      .AllowWrite()
      .AllowSyscall(__NR_close)
      .AllowMmap()
      .AllowExit()
      .AllowHandleSignals()

      // libunwind
      .AllowSyscall(__NR_fstat)
#ifdef __NR_fstat64
      .AllowSyscall(__NR_fstat64)
#endif
      .AllowSyscall(__NR_lseek)
#ifdef __NR__llseek
      .AllowSyscall(__NR__llseek)  // Newer glibc on PPC
#endif
      .AllowSyscall(__NR_mincore)
      .AllowSyscall(__NR_mprotect)
      .AllowSyscall(__NR_munmap)
      .AllowSyscall(__NR_pipe2)

      // Symbolizer
      .AllowSyscall(__NR_brk)
      .AllowSyscall(__NR_clock_gettime)

      // Other
      .AllowSyscall(__NR_dup)
      .AllowSyscall(__NR_fcntl)
      .AllowSyscall(__NR_getpid)
      .AllowSyscall(__NR_gettid)
      .AllowSyscall(__NR_madvise)

      // Required for our ptrace replacement.
      .AddPolicyOnSyscall(
          __NR_process_vm_readv,
          {
              // The pid technically is a 64bit int, however Linux usually uses
              // max 16 bit, so we are fine with comparing only 32 bits here.
              ARG_32(0),
              JEQ32(static_cast<unsigned int>(target_pid), ALLOW),
              JEQ32(static_cast<unsigned int>(1), ALLOW),
          })

      // Add proc maps.
      .AddFileAt(maps_file,
                 file::JoinPath("/proc", absl::StrCat(target_pid), "maps"))
      .AddFileAt(maps_file,
                 file::JoinPath("/proc", absl::StrCat(target_pid), "task",
                                absl::StrCat(target_pid), "maps"))

      // Add the binary itself.
      .AddFileAt(exe_path, app_path);

  // Add all possible libraries without the need of parsing the binary
  // or /proc/pid/maps.
  for (const auto& library_path : {
           "/usr/lib",
           "/lib",
       }) {
    if (access(library_path, F_OK) != -1) {
      VLOG(1) << "Adding library folder '" << library_path << "'";
      builder.AddDirectory(library_path);
    } else {
      VLOG(1) << "Could not add library folder '" << library_path
              << "' as it does not exist";
    }
  }

  auto policy = builder.TryBuild();
  if (!policy.ok()) {
    LOG(ERROR) << "Creating stack unwinder sandbox policy failed";
    return nullptr;
  }
  auto keep_capabilities = absl::make_unique<std::vector<int>>();
  keep_capabilities->push_back(CAP_SYS_PTRACE);
  (*policy)->AllowUnsafeKeepCapabilities(std::move(keep_capabilities));
  // Use no special namespace flags when cloning. We will join an existing
  // user namespace and will unshare() afterwards (See forkserver.cc).
  (*policy)->GetNamespace()->clone_flags_ = 0;
  return std::move(*policy);
}

bool StackTracePeer::LaunchLibunwindSandbox(const Regs* regs,
                                            const Mounts& mounts,
                                            sandbox2::UnwindResult* result) {
  const pid_t pid = regs->pid();

  // Tell executor to use this special internal mode.
  std::vector<std::string> argv;
  std::vector<std::string> envp;

  // We're not using absl::make_unique here as we're a friend of this specific
  // constructor and using make_unique won't work.
  auto executor = absl::WrapUnique(new Executor(pid));

  executor->limits()
      ->set_rlimit_as(RLIM64_INFINITY)
      .set_rlimit_cpu(10)
      .set_walltime_limit(absl::Seconds(5));

  // Temporary directory used to provide files from /proc to the unwind sandbox.
  char unwind_temp_directory_template[] = "/tmp/.sandbox2_unwind_XXXXXX";
  char* unwind_temp_directory = mkdtemp(unwind_temp_directory_template);
  if (!unwind_temp_directory) {
    LOG(WARNING) << "Could not create temporary directory for unwinding";
    return false;
  }
  struct UnwindTempDirectoryCleanup {
    ~UnwindTempDirectoryCleanup() {
      file_util::fileops::DeleteRecursively(capture);
    }
    char* capture;
  } cleanup{unwind_temp_directory};

  // Copy over important files from the /proc directory as we can't mount them.
  const std::string unwind_temp_maps_path =
      file::JoinPath(unwind_temp_directory, "maps");

  if (!file_util::fileops::CopyFile(
          file::JoinPath("/proc", absl::StrCat(pid), "maps"),
          unwind_temp_maps_path, 0400)) {
    LOG(WARNING) << "Could not copy maps file";
    return false;
  }

  // Get path to the binary.
  // app_path contains the path like it is also in /proc/pid/maps. It is
  // relative to the sandboxee's mount namespace. If it is not existing
  // (anymore) it will have a ' (deleted)' suffix.
  std::string app_path;
  std::string proc_pid_exe = file::JoinPath("/proc", absl::StrCat(pid), "exe");
  if (!file_util::fileops::ReadLinkAbsolute(proc_pid_exe, &app_path)) {
    LOG(WARNING) << "Could not obtain absolute path to the binary";
    return false;
  }

  // The exe_path will have a mountable path of the application, even if it was
  // removed.
  // Resolve app_path backing file.
  std::string exe_path = mounts.ResolvePath(app_path).value_or("");

  if (exe_path.empty()) {
    // File was probably removed.
    LOG(WARNING) << "File was removed, using /proc/pid/exe.";
    app_path = std::string(absl::StripSuffix(app_path, " (deleted)"));
    // Create a copy of /proc/pid/exe, mount that one.
    exe_path = file::JoinPath(unwind_temp_directory, "exe");
    if (!file_util::fileops::CopyFile(proc_pid_exe, exe_path, 0700)) {
      LOG(WARNING) << "Could not copy /proc/pid/exe";
      return false;
    }
  }

  VLOG(1) << "Resolved binary: " << app_path << " / " << exe_path;

  // Add mappings for the binary (as they might not have been added due to the
  // forkserver).
  auto policy = StackTracePeer::GetPolicy(pid, unwind_temp_maps_path, app_path,
                                          exe_path, mounts);
  if (!policy) {
    return false;
  }
  Sandbox2 sandbox(std::move(executor), std::move(policy));

  VLOG(1) << "Running libunwind sandbox";
  sandbox.RunAsync();
  Comms* comms = sandbox.comms();

  UnwindSetup msg;
  msg.set_pid(pid);
  msg.set_regs(reinterpret_cast<const char*>(&regs->user_regs_),
               sizeof(regs->user_regs_));
  msg.set_default_max_frames(kDefaultMaxFrames);

  bool success = true;
  if (!comms->SendProtoBuf(msg)) {
    LOG(ERROR) << "Sending libunwind setup message failed";
    success = false;
  }

  if (success && !comms->RecvProtoBuf(result)) {
    LOG(ERROR) << "Receiving libunwind result failed";
    success = false;
  }

  if (!success) {
    sandbox.Kill();
  }
  auto sandbox_result = sandbox.AwaitResult();

  LOG(INFO) << "Libunwind execution status: " << sandbox_result.ToString();

  return success && sandbox_result.final_status() == Result::OK;
}

std::vector<std::string> GetStackTrace(const Regs* regs, const Mounts& mounts) {
  if constexpr (sapi::host_cpu::IsArm64()) {
    return {"[Stack traces unavailable]"};
  }
  if (absl::GetFlag(FLAGS_sandbox_disable_all_stack_traces)) {
    return {"[Stacktraces disabled]"};
  }
  if (!regs) {
    LOG(WARNING) << "Could not obtain stacktrace, regs == nullptr";
    return {"[ERROR (noregs)]"};
  }

  // Show a warning if sandboxed libunwind is requested but we're running in
  // an ASAN/coverage build (= we can't use sandboxed libunwind).
  if (const bool coverage_enabled =
          getenv("COVERAGE") != nullptr;
      absl::GetFlag(FLAGS_sandbox_libunwind_crash_handler) &&
      (sapi::sanitizers::IsAny() || coverage_enabled)) {
    LOG_IF(WARNING, sapi::sanitizers::IsAny())
        << "Sanitizer build, using non-sandboxed libunwind";
    LOG_IF(WARNING, coverage_enabled)
        << "Coverage build, using non-sandboxed libunwind";
    return UnsafeGetStackTrace(regs->pid());
  }

  if (!absl::GetFlag(FLAGS_sandbox_libunwind_crash_handler)) {
    return UnsafeGetStackTrace(regs->pid());
  }

  UnwindResult res;
  if (!StackTracePeer::LaunchLibunwindSandbox(regs, mounts, &res)) {
    return {};
  }
  return {res.stacktrace().begin(), res.stacktrace().end()};
}

std::vector<std::string> UnsafeGetStackTrace(pid_t pid) {
  LOG(WARNING) << "Using non-sandboxed libunwind";
  std::vector<uintptr_t> ips;
  return RunLibUnwindAndSymbolizer(pid, &ips, kDefaultMaxFrames);
}

std::vector<std::string> CompactStackTrace(
    const std::vector<std::string>& stack_trace) {
  std::vector<std::string> compact_trace;
  compact_trace.reserve(stack_trace.size() / 2);
  const std::string* prev = nullptr;
  int seen = 0;
  auto add_repeats = [&compact_trace](int seen) {
    if (seen != 0) {
      compact_trace.push_back(
          absl::StrCat("(previous frame repeated ", seen, " times)"));
    }
  };
  for (const auto& frame : stack_trace) {
    if (prev && frame == *prev) {
      ++seen;
    } else {
      prev = &frame;
      add_repeats(seen);
      seen = 0;
      compact_trace.push_back(frame);
    }
  }
  add_repeats(seen);
  return compact_trace;
}

}  // namespace sandbox2
