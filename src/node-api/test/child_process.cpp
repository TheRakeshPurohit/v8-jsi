// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "child_process.h"

#include <strsafe.h>
#include <windows.h>
#include <cassert>
#include <cstdio>

#ifndef VerifyElseExit
#define VerifyElseExit(condition)                                              \
  do {                                                                         \
    if (!(condition)) {                                                        \
      exitOnError(#condition);                                                 \
    }                                                                          \
  } while (false)
#endif

namespace node_api_tests {

std::string readFromPipe(HANDLE pipeHandle);
void exitOnError(const char* message);

struct AutoHandle {
  HANDLE handle{NULL};

  AutoHandle() = default;
  AutoHandle(HANDLE handle) : handle(handle) {}
  ~AutoHandle() { ::CloseHandle(handle); }

  AutoHandle(const AutoHandle&) = delete;
  AutoHandle& operator=(const AutoHandle&) = delete;

  void Close() {
    ::CloseHandle(handle);
    handle = NULL;
  }
};

// Create a child process that uses the previously created pipes for STDIN and
// STDOUT.
ProcessResult spawnSync(std::string_view command,
                        std::vector<std::string> args) {
  ProcessResult result{};

  // Set the bInheritHandle flag so pipe handles are inherited.

  SECURITY_ATTRIBUTES handles_are_inheritable = {
      sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};

  AutoHandle out_read_handle, out_write_handle;
  VerifyElseExit(CreatePipe(&out_read_handle.handle,
                            &out_write_handle.handle,
                            &handles_are_inheritable,
                            0));
  // Ensure the read handle to the pipe for STDOUT is not inherited.
  VerifyElseExit(
      SetHandleInformation(out_read_handle.handle, HANDLE_FLAG_INHERIT, 0));

  AutoHandle err_read_handle, err_write_handle;
  VerifyElseExit(CreatePipe(&err_read_handle.handle,
                            &err_write_handle.handle,
                            &handles_are_inheritable,
                            0));
  // Ensure the read handle to the pipe for STDERR is not inherited.
  VerifyElseExit(
      SetHandleInformation(err_read_handle.handle, HANDLE_FLAG_INHERIT, 0));

  // Set up members of the STARTUPINFO structure.
  // This structure specifies the STDIN and STDOUT handles for redirection.
  STARTUPINFOA startup_info{};
  startup_info.cb = sizeof(STARTUPINFOA);
  startup_info.dwFlags |= STARTF_USESTDHANDLES;
  startup_info.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
  startup_info.hStdOutput = out_write_handle.handle;
  startup_info.hStdError = err_write_handle.handle;

  // Create the child process.

  std::string commandLine = std::string(command);
  for (std::string& arg : args) {
    commandLine += " " + arg;
  }
  PROCESS_INFORMATION process_info{};
  VerifyElseExit(
      CreateProcessA(nullptr,
                     const_cast<char*>(commandLine.c_str()),  // command line
                     nullptr,  // process security attributes
                     nullptr,  // primary thread security attributes
                     TRUE,     // handles are inherited
                     CREATE_DEFAULT_ERROR_MODE,  // creation flags
                     nullptr,                    // use parent's environment
                     nullptr,          // use parent's current directory
                     &startup_info,    // STARTUPINFO pointer
                     &process_info));  // receives PROCESS_INFORMATION

  VerifyElseExit(WAIT_OBJECT_0 ==
                 ::WaitForSingleObject(process_info.hProcess, INFINITE));

  DWORD exit_code;
  VerifyElseExit(::GetExitCodeProcess(process_info.hProcess, &exit_code));

  // Close handles to the child process and its primary thread.
  // Some applications might keep these handles to monitor the status
  // of the child process, for example.
  ::CloseHandle(process_info.hProcess);
  ::CloseHandle(process_info.hThread);

  // Close handles to the stdin and stdout pipes no longer needed by the child
  // process. If they are not explicitly closed, there is no way to recognize
  // that the child process has ended.

  out_write_handle.Close();
  err_write_handle.Close();

  result.status = exit_code;
  result.std_output = readFromPipe(out_read_handle.handle);
  result.std_error = readFromPipe(err_read_handle.handle);

  return result;
}

std::string readFromPipe(HANDLE pipeHandle) {
  std::string result;
  constexpr size_t bufferSize = 4096;
  char buffer[bufferSize];

  for (;;) {
    DWORD bytesRead;
    BOOL isSuccess =
        ::ReadFile(pipeHandle, buffer, bufferSize, &bytesRead, nullptr);
    if (!isSuccess || bytesRead == 0) break;

    result.append(buffer, bytesRead);
  }

  return result;
}

// Format a readable error message, display a message box,
// and exit from the application.
void exitOnError(const char* message) {
  LPVOID lpMsgBuf;
  LPVOID lpDisplayBuf;
  DWORD dw = GetLastError();

  ::FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr,
                   dw,
                   MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                   (LPSTR)&lpMsgBuf,
                   0,
                   nullptr);

  lpDisplayBuf = (LPVOID)LocalAlloc(
      LMEM_ZEROINIT, (lstrlenA((LPCSTR)lpMsgBuf) + lstrlenA(message) + 40));
  ::StringCchPrintfA((LPSTR)lpDisplayBuf,
                     LocalSize(lpDisplayBuf),
                     "%s failed with error %d: %s",
                     message,
                     dw,
                     lpMsgBuf);
  fprintf(stderr, "%s\n", (const char*)lpDisplayBuf);

  ::LocalFree(lpMsgBuf);
  ::LocalFree(lpDisplayBuf);
  ::ExitProcess(1);
}
}  // namespace node_api_tests