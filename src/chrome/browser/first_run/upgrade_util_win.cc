// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/first_run/upgrade_util.h"

#include <windows.h>
#include <objbase.h>
#include <psapi.h>
#include <shellapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <string>

#include "base/base_paths.h"
#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/logging.h"
#include "base/macros.h"
#include "base/path_service.h"
#include "base/process/launch.h"
#include "base/process/process_handle.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/win/registry.h"
#include "base/win/windows_version.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/first_run/upgrade_util_win.h"
#include "chrome/browser/shell_integration.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/common/pref_names.h"
#include "chrome/install_static/install_util.h"
#include "chrome/installer/util/browser_distribution.h"
#include "chrome/installer/util/google_update_constants.h"
#include "chrome/installer/util/install_util.h"
#include "chrome/installer/util/util_constants.h"
#include "components/prefs/pref_service.h"
#include "ui/base/ui_base_switches.h"

#if defined(GOOGLE_CHROME_BUILD)
#include "google_update/google_update_idl.h"
#endif

namespace {

bool GetNewerChromeFile(base::FilePath* path) {
  if (!base::PathService::Get(base::DIR_EXE, path))
    return false;
  *path = path->Append(installer::kChromeNewExe);
  return true;
}

bool InvokeGoogleUpdateForRename() {
#if defined(GOOGLE_CHROME_BUILD)
  Microsoft::WRL::ComPtr<IProcessLauncher> ipl;
  if (!FAILED(::CoCreateInstance(__uuidof(ProcessLauncherClass), nullptr,
                                 CLSCTX_ALL, IID_PPV_ARGS(&ipl)))) {
    ULONG_PTR phandle = NULL;
    DWORD id = GetCurrentProcessId();
    if (!FAILED(ipl->LaunchCmdElevated(install_static::GetAppGuid(),
                                       google_update::kRegRenameCmdField, id,
                                       &phandle))) {
      HANDLE handle = HANDLE(phandle);
      WaitForSingleObject(handle, INFINITE);
      DWORD exit_code;
      ::GetExitCodeProcess(handle, &exit_code);
      ::CloseHandle(handle);
      if (exit_code == installer::RENAME_SUCCESSFUL)
        return true;
    }
  }
#endif  // GOOGLE_CHROME_BUILD
  return false;
}

}  // namespace

namespace upgrade_util {

bool RelaunchChromeBrowser(const base::CommandLine& command_line) {
  base::FilePath chrome_exe;
  if (!base::PathService::Get(base::FILE_EXE, &chrome_exe)) {
    NOTREACHED();
    return false;
  }

  // Explicitly make sure to relaunch chrome.exe rather than old_chrome.exe.
  // This can happen when old_chrome.exe is launched by a user.
  base::CommandLine chrome_exe_command_line = command_line;
  chrome_exe_command_line.SetProgram(
      chrome_exe.DirName().Append(installer::kChromeExe));

  // Set the working directory to the exe's directory. This avoids a handle to
  // the version directory being kept open in the relaunched child process.
  base::LaunchOptions launch_options;
  launch_options.current_directory = chrome_exe.DirName();
  return base::LaunchProcess(chrome_exe_command_line, launch_options).IsValid();
}

bool IsUpdatePendingRestart() {
  base::FilePath new_chrome_exe;
  if (!GetNewerChromeFile(&new_chrome_exe))
    return false;
  return base::PathExists(new_chrome_exe);
}

bool SwapNewChromeExeIfPresent() {
  if (!IsUpdatePendingRestart())
    return false;

  // If this is a system-level install, ask Google Update to launch an elevated
  // process to rename Chrome executables.
  if (!InstallUtil::IsPerUserInstall())
    return InvokeGoogleUpdateForRename();

  // If this is a user-level install, directly launch a process to rename Chrome
  // executables. Obtain the command to launch the process from the registry.
  base::win::RegKey key;
  if (key.Open(HKEY_CURRENT_USER,
               BrowserDistribution::GetDistribution()->GetVersionKey().c_str(),
               KEY_QUERY_VALUE | KEY_WOW64_32KEY) == ERROR_SUCCESS) {
    std::wstring rename_cmd;
    if (key.ReadValue(google_update::kRegRenameCmdField,
                      &rename_cmd) == ERROR_SUCCESS) {
      base::LaunchOptions options;
      options.wait = true;
      options.start_hidden = true;
      base::Process process = base::LaunchProcess(rename_cmd, options);
      if (process.IsValid()) {
        DWORD exit_code;
        ::GetExitCodeProcess(process.Handle(), &exit_code);
        if (exit_code == installer::RENAME_SUCCESSFUL)
          return true;
      }
    }
  }

  return false;
}

bool IsRunningOldChrome() {
  // This figures out the actual file name that the section containing the
  // mapped exe refers to. This is used instead of GetModuleFileName because the
  // .exe may have been renamed out from under us while we've been running which
  // GetModuleFileName won't notice.
  wchar_t mapped_file_name[MAX_PATH * 2] = {};

  if (!::GetMappedFileName(::GetCurrentProcess(),
                           reinterpret_cast<void*>(::GetModuleHandle(NULL)),
                           mapped_file_name,
                           arraysize(mapped_file_name))) {
    return false;
  }

  base::FilePath file_name(base::FilePath(mapped_file_name).BaseName());
  return base::FilePath::CompareEqualIgnoreCase(file_name.value(),
                                                installer::kChromeOldExe);
}

bool DoUpgradeTasks(const base::CommandLine& command_line) {
  if (!SwapNewChromeExeIfPresent() && !IsRunningOldChrome())
    return false;
  // At this point the chrome.exe has been swapped with the new one.
  if (!RelaunchChromeBrowser(command_line)) {
    // The re-launch fails. Feel free to panic now.
    NOTREACHED();
  }
  return true;
}

}  // namespace upgrade_util
