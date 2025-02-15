/*
 * Copyright (C) 2023 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "common/libs/utils/proc_file_utils.h"

#include <sys/stat.h>

#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <android-base/file.h>
#include <android-base/parseint.h>
#include <android-base/strings.h>

#include "common/libs/fs/shared_buf.h"
#include "common/libs/fs/shared_fd.h"
#include "common/libs/utils/files.h"

namespace cuttlefish {

// TODO(kwstephenkim): This logic is used broadly, so consider
// to create a new library.
template <typename... Args>
static std::string ConcatToString(Args&&... args) {
  std::stringstream concatenator;
  (concatenator << ... << std::forward<Args>(args));
  return concatenator.str();
}

static std::string PidDirPath(const pid_t pid) {
  return ConcatToString(kProcDir, "/", pid);
}

/* ReadFile does not work for /proc/<pid>/<some files>
 * ReadFile requires the file size to be known in advance,
 * which is not the case here.
 */
static Result<std::string> ReadAll(const std::string& file_path) {
  SharedFD fd = SharedFD::Open(file_path, O_RDONLY);
  CF_EXPECT(fd->IsOpen());
  // should be good size to read all Envs or Args,
  // whichever bigger
  const int buf_size = 1024;
  std::string output;
  ssize_t nread = 0;
  do {
    std::vector<char> buf(buf_size);
    nread = ReadExact(fd, buf.data(), buf_size);
    CF_EXPECT(nread >= 0, "ReadExact returns " << nread);
    output.append(buf.begin(), buf.end());
  } while (nread > 0);
  return output;
}

/**
 * Tokenizes the given string, using '\0' as a delimiter
 *
 * android::base::Tokenize works mostly except the delimiter can't be '\0'.
 * The /proc/<pid>/environ file has the list of environment variables, delimited
 * by '\0'. Needs a dedicated tokenizer.
 *
 */
static std::vector<std::string> TokenizeByNullChar(const std::string& input) {
  if (input.empty()) {
    return {};
  }
  std::vector<std::string> tokens;
  std::string token;
  for (int i = 0; i < input.size(); i++) {
    if (input.at(i) != '\0') {
      token.append(1, input.at(i));
    } else {
      if (token.empty()) {
        break;
      }
      tokens.push_back(token);
      token.clear();
    }
  }
  if (!token.empty()) {
    tokens.push_back(token);
  }
  return tokens;
}

Result<std::vector<pid_t>> CollectPids(const uid_t uid) {
  CF_EXPECT(DirectoryExists(kProcDir));
  auto subdirs = CF_EXPECT(DirectoryContents(kProcDir));
  std::regex pid_dir_pattern("[0-9]+");
  std::vector<pid_t> pids;
  for (const auto& subdir : subdirs) {
    if (!std::regex_match(subdir, pid_dir_pattern)) {
      continue;
    }
    int pid;
    // Shouldn't failed here. If failed, either regex or
    // android::base::ParseInt needs serious fixes
    CF_EXPECT(android::base::ParseInt(subdir, &pid));
    struct stat dir_stat_buf;
    if (::stat(PidDirPath(pid).data(), &dir_stat_buf) != 0) {
      continue;
    }
    if (dir_stat_buf.st_uid != uid) {
      continue;
    }
    // as we collect cuttlefish-related stuff, we want exe to be
    // shared by the same owner
    struct stat exe_stat_buf;
    std::string exe_path = PidDirPath(pid) + "/exe";
    if (::stat(exe_path.data(), &exe_stat_buf) != 0) {
      continue;
    }
    if (exe_stat_buf.st_uid != uid) {
      continue;
    }
    pids.push_back(pid);
  }
  return pids;
}

Result<std::vector<std::string>> GetCmdArgs(const pid_t pid) {
  std::string cmdline_file_path = PidDirPath(pid) + "/cmdline";
  auto owner = CF_EXPECT(OwnerUid(cmdline_file_path));
  CF_EXPECT(getuid() == owner);
  std::string contents = CF_EXPECT(ReadAll(cmdline_file_path));
  return TokenizeByNullChar(contents);
}

Result<std::string> GetExecutablePath(const pid_t pid) {
  std::string exec_target_path;
  std::string proc_exe_path = ConcatToString("/proc/", pid, "/exe");
  CF_EXPECT(
      android::base::Readlink(proc_exe_path, std::addressof(exec_target_path)),
      proc_exe_path << " Should be a symbolic link but it is not.");
  std::string suffix(" (deleted)");
  if (android::base::EndsWith(exec_target_path, suffix)) {
    return exec_target_path.substr(0, exec_target_path.size() - suffix.size());
  }
  return exec_target_path;
}

Result<std::vector<pid_t>> CollectPidsByExecName(const std::string& exec_name,
                                                 const uid_t uid) {
  CF_EXPECT(cpp_basename(exec_name) == exec_name);
  auto input_pids = CF_EXPECT(CollectPids(uid));
  std::vector<pid_t> output_pids;
  for (const auto pid : input_pids) {
    auto pid_exec_path = GetExecutablePath(pid);
    if (!pid_exec_path.ok()) {
      LOG(ERROR) << pid_exec_path.error().FormatForEnv();
      continue;
    }
    if (cpp_basename(*pid_exec_path) == exec_name) {
      output_pids.push_back(pid);
    }
  }
  return output_pids;
}

Result<std::vector<pid_t>> CollectPidsByExecPath(const std::string& exec_path,
                                                 const uid_t uid) {
  auto input_pids = CF_EXPECT(CollectPids(uid));
  std::vector<pid_t> output_pids;
  for (const auto pid : input_pids) {
    auto pid_exec_path = GetExecutablePath(pid);
    if (!pid_exec_path.ok()) {
      continue;
    }
    if (*pid_exec_path == exec_path) {
      output_pids.push_back(pid);
    }
  }
  return output_pids;
}

Result<std::vector<pid_t>> CollectPidsByArgv0(const std::string& expected_argv0,
                                              const uid_t uid) {
  auto input_pids = CF_EXPECT(CollectPids(uid));
  std::vector<pid_t> output_pids;
  for (const auto pid : input_pids) {
    auto argv_result = GetCmdArgs(pid);
    if (!argv_result.ok()) {
      continue;
    }
    if (argv_result->empty()) {
      continue;
    }
    if (argv_result->front() == expected_argv0) {
      output_pids.push_back(pid);
    }
  }
  return output_pids;
}

Result<uid_t> OwnerUid(const pid_t pid) {
  auto proc_pid_path = PidDirPath(pid);
  auto uid = CF_EXPECT(OwnerUid(proc_pid_path));
  return uid;
}

Result<uid_t> OwnerUid(const std::string& path) {
  struct stat buf;
  CF_EXPECT_EQ(::stat(path.data(), &buf), 0);
  return buf.st_uid;
}

Result<std::unordered_map<std::string, std::string>> GetEnvs(const pid_t pid) {
  std::string environ_file_path = PidDirPath(pid) + "/environ";
  auto owner = CF_EXPECT(OwnerUid(environ_file_path));
  CF_EXPECT(getuid() == owner, "Owned by another user of uid" << owner);
  std::string environ = CF_EXPECT(ReadAll(environ_file_path));
  std::vector<std::string> lines = TokenizeByNullChar(environ);
  // now, each line looks like:  HOME=/home/user
  std::unordered_map<std::string, std::string> envs;
  for (const auto& line : lines) {
    auto pos = line.find_first_of('=');
    if (pos == std::string::npos) {
      LOG(ERROR) << "Found an invalid env: " << line << " and ignored.";
      continue;
    }
    std::string key = line.substr(0, pos);
    std::string value = line.substr(pos + 1);
    envs[key] = value;
  }
  return envs;
}

Result<ProcInfo> ExtractProcInfo(const pid_t pid) {
  return ProcInfo{.pid_ = pid,
                  .actual_exec_path_ = CF_EXPECT(GetExecutablePath(pid)),
                  .envs_ = CF_EXPECT(GetEnvs(pid)),
                  .args_ = CF_EXPECT(GetCmdArgs(pid))};
}

}  // namespace cuttlefish
