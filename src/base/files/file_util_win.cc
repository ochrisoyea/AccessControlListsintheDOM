// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/files/file_util.h"

#include <windows.h>
#include <io.h>
#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <winsock2.h>

#include <algorithm>
#include <limits>
#include <string>

#include "base/files/file_enumerator.h"
#include "base/files/file_path.h"
#include "base/guid.h"
#include "base/logging.h"
#include "base/macros.h"
#include "base/metrics/histogram_functions.h"
#include "base/process/process_handle.h"
#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_piece.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/threading/thread_restrictions.h"
#include "base/time/time.h"
#include "base/win/scoped_handle.h"
#include "base/win/windows_version.h"

namespace base {

namespace {

const DWORD kFileShareAll =
    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

// Records a sample in a histogram named
// "Windows.PostOperationState.|operation|" indicating the state of |path|
// following the named operation. If |operation_succeeded| is true, the
// "operation succeeded" sample is recorded. Otherwise, the state of |path| is
// queried and the most meaningful sample is recorded.
void RecordPostOperationState(const FilePath& path,
                              StringPiece operation,
                              bool operation_succeeded) {
  // The state of a filesystem item after an operation.
  // These values are persisted to logs. Entries should not be renumbered and
  // numeric values should never be reused.
  enum class PostOperationState {
    kOperationSucceeded = 0,
    kFileNotFoundAfterFailure = 1,
    kPathNotFoundAfterFailure = 2,
    kAccessDeniedAfterFailure = 3,
    kNoAttributesAfterFailure = 4,
    kEmptyDirectoryAfterFailure = 5,
    kNonEmptyDirectoryAfterFailure = 6,
    kNotDirectoryAfterFailure = 7,
    kCount
  } metric = PostOperationState::kOperationSucceeded;

  if (!operation_succeeded) {
    const DWORD attributes = ::GetFileAttributes(path.value().c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
      // On failure to delete, one might expect the file/directory to still be
      // in place. Slice a failure to get its attributes into a few common error
      // buckets.
      const DWORD error_code = ::GetLastError();
      if (error_code == ERROR_FILE_NOT_FOUND)
        metric = PostOperationState::kFileNotFoundAfterFailure;
      else if (error_code == ERROR_PATH_NOT_FOUND)
        metric = PostOperationState::kPathNotFoundAfterFailure;
      else if (error_code == ERROR_ACCESS_DENIED)
        metric = PostOperationState::kAccessDeniedAfterFailure;
      else
        metric = PostOperationState::kNoAttributesAfterFailure;
    } else if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
      if (IsDirectoryEmpty(path))
        metric = PostOperationState::kEmptyDirectoryAfterFailure;
      else
        metric = PostOperationState::kNonEmptyDirectoryAfterFailure;
    } else {
      metric = PostOperationState::kNotDirectoryAfterFailure;
    }
  }

  std::string histogram_name = "Windows.PostOperationState.";
  operation.AppendToString(&histogram_name);
  UmaHistogramEnumeration(histogram_name, metric, PostOperationState::kCount);
}

// Records the sample |error| in a histogram named
// "Windows.FilesystemError.|operation|".
void RecordFilesystemError(StringPiece operation, DWORD error) {
  std::string histogram_name = "Windows.FilesystemError.";
  operation.AppendToString(&histogram_name);
  UmaHistogramSparse(histogram_name, error);
}

// Deletes all files and directories in a path.
// Returns ERROR_SUCCESS on success or the Windows error code corresponding to
// the first error encountered.
DWORD DeleteFileRecursive(const FilePath& path,
                          const FilePath::StringType& pattern,
                          bool recursive) {
  FileEnumerator traversal(path, false,
                           FileEnumerator::FILES | FileEnumerator::DIRECTORIES,
                           pattern);
  DWORD result = ERROR_SUCCESS;
  for (FilePath current = traversal.Next(); !current.empty();
       current = traversal.Next()) {
    // Try to clear the read-only bit if we find it.
    FileEnumerator::FileInfo info = traversal.GetInfo();
    if ((info.find_data().dwFileAttributes & FILE_ATTRIBUTE_READONLY) &&
        (recursive || !info.IsDirectory())) {
      ::SetFileAttributes(
          current.value().c_str(),
          info.find_data().dwFileAttributes & ~FILE_ATTRIBUTE_READONLY);
    }

    DWORD this_result = ERROR_SUCCESS;
    if (info.IsDirectory()) {
      if (recursive) {
        this_result = DeleteFileRecursive(current, pattern, true);
        if (this_result == ERROR_SUCCESS &&
            !::RemoveDirectory(current.value().c_str())) {
          this_result = ::GetLastError();
        }
      }
    } else if (!::DeleteFile(current.value().c_str())) {
      this_result = ::GetLastError();
    }
    if (result == ERROR_SUCCESS)
      result = this_result;
  }
  return result;
}

// Appends |mode_char| to |mode| before the optional character set encoding; see
// https://msdn.microsoft.com/library/yeby3zcb.aspx for details.
void AppendModeCharacter(base::char16 mode_char, base::string16* mode) {
  size_t comma_pos = mode->find(L',');
  mode->insert(comma_pos == base::string16::npos ? mode->length() : comma_pos,
               1, mode_char);
}

bool DoCopyFile(const FilePath& from_path,
                const FilePath& to_path,
                bool fail_if_exists) {
  AssertBlockingAllowed();
  if (from_path.ReferencesParent() || to_path.ReferencesParent())
    return false;

  // NOTE: I suspect we could support longer paths, but that would involve
  // analyzing all our usage of files.
  if (from_path.value().length() >= MAX_PATH ||
      to_path.value().length() >= MAX_PATH) {
    return false;
  }

  // Unlike the posix implementation that copies the file manually and discards
  // the ACL bits, CopyFile() copies the complete SECURITY_DESCRIPTOR and access
  // bits, which is usually not what we want. We can't do much about the
  // SECURITY_DESCRIPTOR but at least remove the read only bit.
  const wchar_t* dest = to_path.value().c_str();
  if (!::CopyFile(from_path.value().c_str(), dest, fail_if_exists)) {
    // Copy failed.
    return false;
  }
  DWORD attrs = GetFileAttributes(dest);
  if (attrs == INVALID_FILE_ATTRIBUTES) {
    return false;
  }
  if (attrs & FILE_ATTRIBUTE_READONLY) {
    SetFileAttributes(dest, attrs & ~FILE_ATTRIBUTE_READONLY);
  }
  return true;
}

bool DoCopyDirectory(const FilePath& from_path,
                     const FilePath& to_path,
                     bool recursive,
                     bool fail_if_exists) {
  // NOTE(maruel): Previous version of this function used to call
  // SHFileOperation().  This used to copy the file attributes and extended
  // attributes, OLE structured storage, NTFS file system alternate data
  // streams, SECURITY_DESCRIPTOR. In practice, this is not what we want, we
  // want the containing directory to propagate its SECURITY_DESCRIPTOR.
  AssertBlockingAllowed();

  // NOTE: I suspect we could support longer paths, but that would involve
  // analyzing all our usage of files.
  if (from_path.value().length() >= MAX_PATH ||
      to_path.value().length() >= MAX_PATH) {
    return false;
  }

  // This function does not properly handle destinations within the source.
  FilePath real_to_path = to_path;
  if (PathExists(real_to_path)) {
    real_to_path = MakeAbsoluteFilePath(real_to_path);
    if (real_to_path.empty())
      return false;
  } else {
    real_to_path = MakeAbsoluteFilePath(real_to_path.DirName());
    if (real_to_path.empty())
      return false;
  }
  FilePath real_from_path = MakeAbsoluteFilePath(from_path);
  if (real_from_path.empty())
    return false;
  if (real_to_path == real_from_path || real_from_path.IsParent(real_to_path))
    return false;

  int traverse_type = FileEnumerator::FILES;
  if (recursive)
    traverse_type |= FileEnumerator::DIRECTORIES;
  FileEnumerator traversal(from_path, recursive, traverse_type);

  if (!PathExists(from_path)) {
    DLOG(ERROR) << "CopyDirectory() couldn't stat source directory: "
                << from_path.value().c_str();
    return false;
  }
  // TODO(maruel): This is not necessary anymore.
  DCHECK(recursive || DirectoryExists(from_path));

  FilePath current = from_path;
  bool from_is_dir = DirectoryExists(from_path);
  bool success = true;
  FilePath from_path_base = from_path;
  if (recursive && DirectoryExists(to_path)) {
    // If the destination already exists and is a directory, then the
    // top level of source needs to be copied.
    from_path_base = from_path.DirName();
  }

  while (success && !current.empty()) {
    // current is the source path, including from_path, so append
    // the suffix after from_path to to_path to create the target_path.
    FilePath target_path(to_path);
    if (from_path_base != current) {
      if (!from_path_base.AppendRelativePath(current, &target_path)) {
        success = false;
        break;
      }
    }

    if (from_is_dir) {
      if (!DirectoryExists(target_path) &&
          !::CreateDirectory(target_path.value().c_str(), NULL)) {
        DLOG(ERROR) << "CopyDirectory() couldn't create directory: "
                    << target_path.value().c_str();
        success = false;
      }
    } else if (!DoCopyFile(current, target_path, fail_if_exists)) {
      DLOG(ERROR) << "CopyDirectory() couldn't create file: "
                  << target_path.value().c_str();
      success = false;
    }

    current = traversal.Next();
    if (!current.empty())
      from_is_dir = traversal.GetInfo().IsDirectory();
  }

  return success;
}

// Returns ERROR_SUCCESS on success, or a Windows error code on failure.
DWORD DoDeleteFile(const FilePath& path, bool recursive) {
  AssertBlockingAllowed();

  if (path.empty())
    return ERROR_SUCCESS;

  if (path.value().length() >= MAX_PATH)
    return ERROR_BAD_PATHNAME;

  // Handle any path with wildcards.
  if (path.BaseName().value().find_first_of(L"*?") !=
      FilePath::StringType::npos) {
    return DeleteFileRecursive(path.DirName(), path.BaseName().value(),
                               recursive);
  }

  // Report success if the file or path does not exist.
  const DWORD attr = ::GetFileAttributes(path.value().c_str());
  if (attr == INVALID_FILE_ATTRIBUTES) {
    const DWORD error_code = ::GetLastError();
    return (error_code == ERROR_FILE_NOT_FOUND ||
            error_code == ERROR_PATH_NOT_FOUND)
               ? ERROR_SUCCESS
               : error_code;
  }

  // Clear the read-only bit if it is set.
  if ((attr & FILE_ATTRIBUTE_READONLY) &&
      !::SetFileAttributes(path.value().c_str(),
                           attr & ~FILE_ATTRIBUTE_READONLY)) {
    return ::GetLastError();
  }

  // Perform a simple delete on anything that isn't a directory.
  if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) {
    return ::DeleteFile(path.value().c_str()) ? ERROR_SUCCESS
                                              : ::GetLastError();
  }

  if (recursive) {
    const DWORD error_code = DeleteFileRecursive(path, L"*", true);
    if (error_code != ERROR_SUCCESS)
      return error_code;
  }
  return ::RemoveDirectory(path.value().c_str()) ? ERROR_SUCCESS
                                                 : ::GetLastError();
}

}  // namespace

FilePath MakeAbsoluteFilePath(const FilePath& input) {
  AssertBlockingAllowed();
  wchar_t file_path[MAX_PATH];
  if (!_wfullpath(file_path, input.value().c_str(), MAX_PATH))
    return FilePath();
  return FilePath(file_path);
}

bool DeleteFile(const FilePath& path, bool recursive) {
  static constexpr char kRecursive[] = "DeleteFile.Recursive";
  static constexpr char kNonRecursive[] = "DeleteFile.NonRecursive";
  const StringPiece operation(recursive ? kRecursive : kNonRecursive);

  AssertBlockingAllowed();

  // Metrics for delete failures tracked in https://crbug.com/599084. Delete may
  // fail for a number of reasons. Log some metrics relating to failures in the
  // current code so that any improvements or regressions resulting from
  // subsequent code changes can be detected.
  const DWORD error = DoDeleteFile(path, recursive);
  RecordPostOperationState(path, operation, error == ERROR_SUCCESS);
  if (error == ERROR_SUCCESS)
    return true;

  RecordFilesystemError(operation, error);
  return false;
}

bool DeleteFileAfterReboot(const FilePath& path) {
  AssertBlockingAllowed();

  if (path.value().length() >= MAX_PATH)
    return false;

  return MoveFileEx(path.value().c_str(), NULL,
                    MOVEFILE_DELAY_UNTIL_REBOOT |
                        MOVEFILE_REPLACE_EXISTING) != FALSE;
}

bool ReplaceFile(const FilePath& from_path,
                 const FilePath& to_path,
                 File::Error* error) {
  AssertBlockingAllowed();
  // Try a simple move first.  It will only succeed when |to_path| doesn't
  // already exist.
  if (::MoveFile(from_path.value().c_str(), to_path.value().c_str()))
    return true;
  File::Error move_error = File::OSErrorToFileError(GetLastError());

  // Try the full-blown replace if the move fails, as ReplaceFile will only
  // succeed when |to_path| does exist. When writing to a network share, we may
  // not be able to change the ACLs. Ignore ACL errors then
  // (REPLACEFILE_IGNORE_MERGE_ERRORS).
  if (::ReplaceFile(to_path.value().c_str(), from_path.value().c_str(), NULL,
                    REPLACEFILE_IGNORE_MERGE_ERRORS, NULL, NULL)) {
    return true;
  }
  // In the case of FILE_ERROR_NOT_FOUND from ReplaceFile, it is likely that
  // |to_path| does not exist. In this case, the more relevant error comes
  // from the call to MoveFile.
  if (error) {
    File::Error replace_error = File::OSErrorToFileError(GetLastError());
    *error = replace_error == File::FILE_ERROR_NOT_FOUND ? move_error
                                                         : replace_error;
  }
  return false;
}

bool CopyDirectory(const FilePath& from_path,
                   const FilePath& to_path,
                   bool recursive) {
  return DoCopyDirectory(from_path, to_path, recursive, false);
}

bool CopyDirectoryExcl(const FilePath& from_path,
                       const FilePath& to_path,
                       bool recursive) {
  return DoCopyDirectory(from_path, to_path, recursive, true);
}

bool PathExists(const FilePath& path) {
  AssertBlockingAllowed();
  return (GetFileAttributes(path.value().c_str()) != INVALID_FILE_ATTRIBUTES);
}

bool PathIsWritable(const FilePath& path) {
  AssertBlockingAllowed();
  HANDLE dir =
      CreateFile(path.value().c_str(), FILE_ADD_FILE, kFileShareAll,
                 NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);

  if (dir == INVALID_HANDLE_VALUE)
    return false;

  CloseHandle(dir);
  return true;
}

bool DirectoryExists(const FilePath& path) {
  AssertBlockingAllowed();
  DWORD fileattr = GetFileAttributes(path.value().c_str());
  if (fileattr != INVALID_FILE_ATTRIBUTES)
    return (fileattr & FILE_ATTRIBUTE_DIRECTORY) != 0;
  return false;
}

bool GetTempDir(FilePath* path) {
  wchar_t temp_path[MAX_PATH + 1];
  DWORD path_len = ::GetTempPath(MAX_PATH, temp_path);
  if (path_len >= MAX_PATH || path_len <= 0)
    return false;
  // TODO(evanm): the old behavior of this function was to always strip the
  // trailing slash.  We duplicate this here, but it shouldn't be necessary
  // when everyone is using the appropriate FilePath APIs.
  *path = FilePath(temp_path).StripTrailingSeparators();
  return true;
}

FilePath GetHomeDir() {
  char16 result[MAX_PATH];
  if (SUCCEEDED(SHGetFolderPath(NULL, CSIDL_PROFILE, NULL, SHGFP_TYPE_CURRENT,
                                result)) &&
      result[0]) {
    return FilePath(result);
  }

  // Fall back to the temporary directory on failure.
  FilePath temp;
  if (GetTempDir(&temp))
    return temp;

  // Last resort.
  return FilePath(L"C:\\");
}

bool CreateTemporaryFile(FilePath* path) {
  AssertBlockingAllowed();

  FilePath temp_file;

  if (!GetTempDir(path))
    return false;

  if (CreateTemporaryFileInDir(*path, &temp_file)) {
    *path = temp_file;
    return true;
  }

  return false;
}

// On POSIX we have semantics to create and open a temporary file
// atomically.
// TODO(jrg): is there equivalent call to use on Windows instead of
// going 2-step?
FILE* CreateAndOpenTemporaryFileInDir(const FilePath& dir, FilePath* path) {
  AssertBlockingAllowed();
  if (!CreateTemporaryFileInDir(dir, path)) {
    return NULL;
  }
  // Open file in binary mode, to avoid problems with fwrite. On Windows
  // it replaces \n's with \r\n's, which may surprise you.
  // Reference: http://msdn.microsoft.com/en-us/library/h9t88zwz(VS.71).aspx
  return OpenFile(*path, "wb+");
}

bool CreateTemporaryFileInDir(const FilePath& dir, FilePath* temp_file) {
  AssertBlockingAllowed();

  // Use GUID instead of ::GetTempFileName() to generate unique file names.
  // "Due to the algorithm used to generate file names, GetTempFileName can
  // perform poorly when creating a large number of files with the same prefix.
  // In such cases, it is recommended that you construct unique file names based
  // on GUIDs."
  // https://msdn.microsoft.com/library/windows/desktop/aa364991.aspx

  FilePath temp_name;
  bool create_file_success = false;

  // Although it is nearly impossible to get a duplicate name with GUID, we
  // still use a loop here in case it happens.
  for (int i = 0; i < 100; ++i) {
    temp_name = dir.Append(ASCIIToUTF16(base::GenerateGUID()) + L".tmp");
    File file(temp_name,
              File::FLAG_CREATE | File::FLAG_READ | File::FLAG_WRITE);
    if (file.IsValid()) {
      file.Close();
      create_file_success = true;
      break;
    }
  }

  if (!create_file_success) {
    DPLOG(WARNING) << "Failed to get temporary file name in "
                   << UTF16ToUTF8(dir.value());
    return false;
  }

  wchar_t long_temp_name[MAX_PATH + 1];
  DWORD long_name_len =
      GetLongPathName(temp_name.value().c_str(), long_temp_name, MAX_PATH);
  if (long_name_len > MAX_PATH || long_name_len == 0) {
    // GetLongPathName() failed, but we still have a temporary file.
    *temp_file = std::move(temp_name);
    return true;
  }

  FilePath::StringType long_temp_name_str;
  long_temp_name_str.assign(long_temp_name, long_name_len);
  *temp_file = FilePath(std::move(long_temp_name_str));
  return true;
}

bool CreateTemporaryDirInDir(const FilePath& base_dir,
                             const FilePath::StringType& prefix,
                             FilePath* new_dir) {
  AssertBlockingAllowed();

  FilePath path_to_create;

  for (int count = 0; count < 50; ++count) {
    // Try create a new temporary directory with random generated name. If
    // the one exists, keep trying another path name until we reach some limit.
    string16 new_dir_name;
    new_dir_name.assign(prefix);
    new_dir_name.append(IntToString16(GetCurrentProcId()));
    new_dir_name.push_back('_');
    new_dir_name.append(
        IntToString16(RandInt(0, std::numeric_limits<int16_t>::max())));

    path_to_create = base_dir.Append(new_dir_name);
    if (::CreateDirectory(path_to_create.value().c_str(), NULL)) {
      *new_dir = path_to_create;
      return true;
    }
  }

  return false;
}

bool CreateNewTempDirectory(const FilePath::StringType& prefix,
                            FilePath* new_temp_path) {
  AssertBlockingAllowed();

  FilePath system_temp_dir;
  if (!GetTempDir(&system_temp_dir))
    return false;

  return CreateTemporaryDirInDir(system_temp_dir, prefix, new_temp_path);
}

bool CreateDirectoryAndGetError(const FilePath& full_path,
                                File::Error* error) {
  AssertBlockingAllowed();

  // If the path exists, we've succeeded if it's a directory, failed otherwise.
  const wchar_t* full_path_str = full_path.value().c_str();
  DWORD fileattr = ::GetFileAttributes(full_path_str);
  if (fileattr != INVALID_FILE_ATTRIBUTES) {
    if ((fileattr & FILE_ATTRIBUTE_DIRECTORY) != 0) {
      DVLOG(1) << "CreateDirectory(" << full_path_str << "), "
               << "directory already exists.";
      return true;
    }
    DLOG(WARNING) << "CreateDirectory(" << full_path_str << "), "
                  << "conflicts with existing file.";
    if (error) {
      *error = File::FILE_ERROR_NOT_A_DIRECTORY;
    }
    return false;
  }

  // Invariant:  Path does not exist as file or directory.

  // Attempt to create the parent recursively.  This will immediately return
  // true if it already exists, otherwise will create all required parent
  // directories starting with the highest-level missing parent.
  FilePath parent_path(full_path.DirName());
  if (parent_path.value() == full_path.value()) {
    if (error) {
      *error = File::FILE_ERROR_NOT_FOUND;
    }
    return false;
  }
  if (!CreateDirectoryAndGetError(parent_path, error)) {
    DLOG(WARNING) << "Failed to create one of the parent directories.";
    if (error) {
      DCHECK(*error != File::FILE_OK);
    }
    return false;
  }

  if (!::CreateDirectory(full_path_str, NULL)) {
    DWORD error_code = ::GetLastError();
    if (error_code == ERROR_ALREADY_EXISTS && DirectoryExists(full_path)) {
      // This error code ERROR_ALREADY_EXISTS doesn't indicate whether we
      // were racing with someone creating the same directory, or a file
      // with the same path.  If DirectoryExists() returns true, we lost the
      // race to create the same directory.
      return true;
    } else {
      if (error)
        *error = File::OSErrorToFileError(error_code);
      DLOG(WARNING) << "Failed to create directory " << full_path_str
                    << ", last error is " << error_code << ".";
      return false;
    }
  } else {
    return true;
  }
}

bool NormalizeFilePath(const FilePath& path, FilePath* real_path) {
  AssertBlockingAllowed();
  FilePath mapped_file;
  if (!NormalizeToNativeFilePath(path, &mapped_file))
    return false;
  // NormalizeToNativeFilePath() will return a path that starts with
  // "\Device\Harddisk...".  Helper DevicePathToDriveLetterPath()
  // will find a drive letter which maps to the path's device, so
  // that we return a path starting with a drive letter.
  return DevicePathToDriveLetterPath(mapped_file, real_path);
}

bool DevicePathToDriveLetterPath(const FilePath& nt_device_path,
                                 FilePath* out_drive_letter_path) {
  AssertBlockingAllowed();

  // Get the mapping of drive letters to device paths.
  const int kDriveMappingSize = 1024;
  wchar_t drive_mapping[kDriveMappingSize] = {'\0'};
  if (!::GetLogicalDriveStrings(kDriveMappingSize - 1, drive_mapping)) {
    DLOG(ERROR) << "Failed to get drive mapping.";
    return false;
  }

  // The drive mapping is a sequence of null terminated strings.
  // The last string is empty.
  wchar_t* drive_map_ptr = drive_mapping;
  wchar_t device_path_as_string[MAX_PATH];
  wchar_t drive[] = L" :";

  // For each string in the drive mapping, get the junction that links
  // to it.  If that junction is a prefix of |device_path|, then we
  // know that |drive| is the real path prefix.
  while (*drive_map_ptr) {
    drive[0] = drive_map_ptr[0];  // Copy the drive letter.

    if (QueryDosDevice(drive, device_path_as_string, MAX_PATH)) {
      FilePath device_path(device_path_as_string);
      if (device_path == nt_device_path ||
          device_path.IsParent(nt_device_path)) {
        *out_drive_letter_path = FilePath(drive +
            nt_device_path.value().substr(wcslen(device_path_as_string)));
        return true;
      }
    }
    // Move to the next drive letter string, which starts one
    // increment after the '\0' that terminates the current string.
    while (*drive_map_ptr++) {}
  }

  // No drive matched.  The path does not start with a device junction
  // that is mounted as a drive letter.  This means there is no drive
  // letter path to the volume that holds |device_path|, so fail.
  return false;
}

bool NormalizeToNativeFilePath(const FilePath& path, FilePath* nt_path) {
  AssertBlockingAllowed();
  // In Vista, GetFinalPathNameByHandle() would give us the real path
  // from a file handle.  If we ever deprecate XP, consider changing the
  // code below to a call to GetFinalPathNameByHandle().  The method this
  // function uses is explained in the following msdn article:
  // http://msdn.microsoft.com/en-us/library/aa366789(VS.85).aspx
  win::ScopedHandle file_handle(
      ::CreateFile(path.value().c_str(),
                   GENERIC_READ,
                   kFileShareAll,
                   NULL,
                   OPEN_EXISTING,
                   FILE_ATTRIBUTE_NORMAL,
                   NULL));
  if (!file_handle.IsValid())
    return false;

  // Create a file mapping object.  Can't easily use MemoryMappedFile, because
  // we only map the first byte, and need direct access to the handle. You can
  // not map an empty file, this call fails in that case.
  win::ScopedHandle file_map_handle(
      ::CreateFileMapping(file_handle.Get(),
                          NULL,
                          PAGE_READONLY,
                          0,
                          1,  // Just one byte.  No need to look at the data.
                          NULL));
  if (!file_map_handle.IsValid())
    return false;

  // Use a view of the file to get the path to the file.
  void* file_view = MapViewOfFile(file_map_handle.Get(),
                                  FILE_MAP_READ, 0, 0, 1);
  if (!file_view)
    return false;

  // The expansion of |path| into a full path may make it longer.
  // GetMappedFileName() will fail if the result is longer than MAX_PATH.
  // Pad a bit to be safe.  If kMaxPathLength is ever changed to be less
  // than MAX_PATH, it would be nessisary to test that GetMappedFileName()
  // not return kMaxPathLength.  This would mean that only part of the
  // path fit in |mapped_file_path|.
  const int kMaxPathLength = MAX_PATH + 10;
  wchar_t mapped_file_path[kMaxPathLength];
  bool success = false;
  HANDLE cp = GetCurrentProcess();
  if (::GetMappedFileNameW(cp, file_view, mapped_file_path, kMaxPathLength)) {
    *nt_path = FilePath(mapped_file_path);
    success = true;
  }
  ::UnmapViewOfFile(file_view);
  return success;
}

// TODO(rkc): Work out if we want to handle NTFS junctions here or not, handle
// them if we do decide to.
bool IsLink(const FilePath& file_path) {
  return false;
}

bool GetFileInfo(const FilePath& file_path, File::Info* results) {
  AssertBlockingAllowed();

  WIN32_FILE_ATTRIBUTE_DATA attr;
  if (!GetFileAttributesEx(file_path.value().c_str(),
                           GetFileExInfoStandard, &attr)) {
    return false;
  }

  ULARGE_INTEGER size;
  size.HighPart = attr.nFileSizeHigh;
  size.LowPart = attr.nFileSizeLow;
  results->size = size.QuadPart;

  results->is_directory =
      (attr.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  results->last_modified = Time::FromFileTime(attr.ftLastWriteTime);
  results->last_accessed = Time::FromFileTime(attr.ftLastAccessTime);
  results->creation_time = Time::FromFileTime(attr.ftCreationTime);

  return true;
}

FILE* OpenFile(const FilePath& filename, const char* mode) {
  // 'N' is unconditionally added below, so be sure there is not one already
  // present before a comma in |mode|.
  DCHECK(
      strchr(mode, 'N') == nullptr ||
      (strchr(mode, ',') != nullptr && strchr(mode, 'N') > strchr(mode, ',')));
  AssertBlockingAllowed();
  string16 w_mode = ASCIIToUTF16(mode);
  AppendModeCharacter(L'N', &w_mode);
  return _wfsopen(filename.value().c_str(), w_mode.c_str(), _SH_DENYNO);
}

FILE* FileToFILE(File file, const char* mode) {
  if (!file.IsValid())
    return NULL;
  int fd =
      _open_osfhandle(reinterpret_cast<intptr_t>(file.GetPlatformFile()), 0);
  if (fd < 0)
    return NULL;
  file.TakePlatformFile();
  FILE* stream = _fdopen(fd, mode);
  if (!stream)
    _close(fd);
  return stream;
}

int ReadFile(const FilePath& filename, char* data, int max_size) {
  AssertBlockingAllowed();
  win::ScopedHandle file(CreateFile(filename.value().c_str(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    NULL,
                                    OPEN_EXISTING,
                                    FILE_FLAG_SEQUENTIAL_SCAN,
                                    NULL));
  if (!file.IsValid())
    return -1;

  DWORD read;
  if (::ReadFile(file.Get(), data, max_size, &read, NULL))
    return read;

  return -1;
}

int WriteFile(const FilePath& filename, const char* data, int size) {
  AssertBlockingAllowed();
  win::ScopedHandle file(CreateFile(filename.value().c_str(), GENERIC_WRITE, 0,
                                    NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                    NULL));
  if (!file.IsValid()) {
    DPLOG(WARNING) << "CreateFile failed for path "
                   << UTF16ToUTF8(filename.value());
    return -1;
  }

  DWORD written;
  BOOL result = ::WriteFile(file.Get(), data, size, &written, NULL);
  if (result && static_cast<int>(written) == size)
    return written;

  if (!result) {
    // WriteFile failed.
    DPLOG(WARNING) << "writing file " << UTF16ToUTF8(filename.value())
                   << " failed";
  } else {
    // Didn't write all the bytes.
    DLOG(WARNING) << "wrote" << written << " bytes to "
                  << UTF16ToUTF8(filename.value()) << " expected " << size;
  }
  return -1;
}

bool AppendToFile(const FilePath& filename, const char* data, int size) {
  AssertBlockingAllowed();
  win::ScopedHandle file(CreateFile(filename.value().c_str(),
                                    FILE_APPEND_DATA,
                                    0,
                                    NULL,
                                    OPEN_EXISTING,
                                    0,
                                    NULL));
  if (!file.IsValid()) {
    VPLOG(1) << "CreateFile failed for path " << UTF16ToUTF8(filename.value());
    return false;
  }

  DWORD written;
  BOOL result = ::WriteFile(file.Get(), data, size, &written, NULL);
  if (result && static_cast<int>(written) == size)
    return true;

  if (!result) {
    // WriteFile failed.
    VPLOG(1) << "Writing file " << UTF16ToUTF8(filename.value()) << " failed";
  } else {
    // Didn't write all the bytes.
    VPLOG(1) << "Only wrote " << written << " out of " << size << " byte(s) to "
             << UTF16ToUTF8(filename.value());
  }
  return false;
}

bool GetCurrentDirectory(FilePath* dir) {
  AssertBlockingAllowed();

  wchar_t system_buffer[MAX_PATH];
  system_buffer[0] = 0;
  DWORD len = ::GetCurrentDirectory(MAX_PATH, system_buffer);
  if (len == 0 || len > MAX_PATH)
    return false;
  // TODO(evanm): the old behavior of this function was to always strip the
  // trailing slash.  We duplicate this here, but it shouldn't be necessary
  // when everyone is using the appropriate FilePath APIs.
  std::wstring dir_str(system_buffer);
  *dir = FilePath(dir_str).StripTrailingSeparators();
  return true;
}

bool SetCurrentDirectory(const FilePath& directory) {
  AssertBlockingAllowed();
  return ::SetCurrentDirectory(directory.value().c_str()) != 0;
}

int GetMaximumPathComponentLength(const FilePath& path) {
  AssertBlockingAllowed();

  wchar_t volume_path[MAX_PATH];
  if (!GetVolumePathNameW(path.NormalizePathSeparators().value().c_str(),
                          volume_path,
                          arraysize(volume_path))) {
    return -1;
  }

  DWORD max_length = 0;
  if (!GetVolumeInformationW(volume_path, NULL, 0, NULL, &max_length, NULL,
                             NULL, 0)) {
    return -1;
  }

  // Length of |path| with path separator appended.
  size_t prefix = path.StripTrailingSeparators().value().size() + 1;
  // The whole path string must be shorter than MAX_PATH. That is, it must be
  // prefix + component_length < MAX_PATH (or equivalently, <= MAX_PATH - 1).
  int whole_path_limit = std::max(0, MAX_PATH - 1 - static_cast<int>(prefix));
  return std::min(whole_path_limit, static_cast<int>(max_length));
}

bool CopyFile(const FilePath& from_path, const FilePath& to_path) {
  return DoCopyFile(from_path, to_path, false);
}

bool SetNonBlocking(int fd) {
  unsigned long nonblocking = 1;
  if (ioctlsocket(fd, FIONBIO, &nonblocking) == 0)
    return true;
  return false;
}

// -----------------------------------------------------------------------------

namespace internal {

bool MoveUnsafe(const FilePath& from_path, const FilePath& to_path) {
  AssertBlockingAllowed();

  // NOTE: I suspect we could support longer paths, but that would involve
  // analyzing all our usage of files.
  if (from_path.value().length() >= MAX_PATH ||
      to_path.value().length() >= MAX_PATH) {
    return false;
  }
  if (MoveFileEx(from_path.value().c_str(), to_path.value().c_str(),
                 MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING) != 0)
    return true;

  // Keep the last error value from MoveFileEx around in case the below
  // fails.
  bool ret = false;
  DWORD last_error = ::GetLastError();

  if (DirectoryExists(from_path)) {
    // MoveFileEx fails if moving directory across volumes. We will simulate
    // the move by using Copy and Delete. Ideally we could check whether
    // from_path and to_path are indeed in different volumes.
    ret = internal::CopyAndDeleteDirectory(from_path, to_path);
  }

  if (!ret) {
    // Leave a clue about what went wrong so that it can be (at least) picked
    // up by a PLOG entry.
    ::SetLastError(last_error);
  }

  return ret;
}

bool CopyAndDeleteDirectory(const FilePath& from_path,
                            const FilePath& to_path) {
  AssertBlockingAllowed();
  if (CopyDirectory(from_path, to_path, true)) {
    if (DeleteFile(from_path, true))
      return true;

    // Like Move, this function is not transactional, so we just
    // leave the copied bits behind if deleting from_path fails.
    // If to_path exists previously then we have already overwritten
    // it by now, we don't get better off by deleting the new bits.
  }
  return false;
}

}  // namespace internal
}  // namespace base
