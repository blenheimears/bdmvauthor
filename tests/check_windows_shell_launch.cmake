if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR not supplied")
endif()
file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)

foreach(needle
  "std::vector<wchar_t> windows_shell_command_line"
  "L\"\\\"\" + command_processor + L\"\\\" /D /S /C \\\"\""
  "int shell_system(const std::string& command)"
  "FILE* shell_popen(const std::string& command, const char* mode)"
  "CreateProcessW(command_processor.c_str()"
  "CREATE_NO_WINDOW"
  "CreatePipe(&read_handle, &write_handle"
  "windows_pipe_processes.emplace(pipe, child)"
  "return shell_system(command) == 0;"
  "const int rc = shell_system(guarded);"
  "return shell_system(guarded_command);"
  "FILE* pipe = shell_popen(command, \"r\");")
  string(FIND "${AUTHOR}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "Windows Program Files shell-launch regression: missing ${needle}")
  endif()
endforeach()

# Helper execution uses explicit child processes on both Windows and POSIX.
# Windows must use CreateProcessW/CREATE_NO_WINDOW. POSIX must use posix_spawn
# with a dedicated process group so cancellation remains tree-wide without the
# unsafe fork-from-a-multithreaded-Qt-process window that regressed Linux.
# The new process group must also have stdin detached from the controlling TTY;
# otherwise FFmpeg can be job-control stopped with SIGTTIN/SIGTTOU and appear hung.
string(FIND "${AUTHOR}" "std::system(" BAD_SYSTEM)
if(NOT BAD_SYSTEM EQUAL -1)
  message(FATAL_ERROR "Windows/cancellation shell-launch regression: direct std::system() remains")
endif()
string(FIND "${AUTHOR}" "int shell_system(const std::string& command)" SHELL_SYSTEM)
string(FIND "${AUTHOR}" [=[posix_spawn(&pid, "/bin/sh"]=] POSIX_SPAWN)
string(FIND "${AUTHOR}" "POSIX_SPAWN_SETPGROUP" POSIX_PGROUP)
string(FIND "${AUTHOR}" "POSIX_SPAWN_SETSIGMASK" POSIX_SIGMASK)
string(FIND "${AUTHOR}" "POSIX_SPAWN_SETSIGDEF" POSIX_SIGDEF)
string(FIND "${AUTHOR}" [=[posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0)]=] POSIX_NULL_STDIN)
string(FIND "${AUTHOR}" [=[posix_spawn(&pid, "/bin/sh", &actions, &attr, argv, environ)]=] POSIX_ACTIONS)
string(FIND "${AUTHOR}" "WNOHANG | WUNTRACED" POSIX_STOP_DETECT)
string(FIND "${AUTHOR}" "WIFSTOPPED(status)" POSIX_STOP_HANDLE)
string(FIND "${AUTHOR}" "const pid_t pid = fork()" BAD_POSIX_FORK)
string(FIND "${AUTHOR}" "CreateProcessW(command_processor.c_str()" WINDOWS_CREATE)
if(SHELL_SYSTEM EQUAL -1 OR POSIX_SPAWN EQUAL -1 OR POSIX_PGROUP EQUAL -1 OR
   POSIX_SIGMASK EQUAL -1 OR POSIX_SIGDEF EQUAL -1 OR POSIX_NULL_STDIN EQUAL -1 OR
   POSIX_ACTIONS EQUAL -1 OR POSIX_STOP_DETECT EQUAL -1 OR POSIX_STOP_HANDLE EQUAL -1 OR
   WINDOWS_CREATE EQUAL -1)
  message(FATAL_ERROR "cross-platform cancellable shell launcher is incomplete")
endif()
if(NOT BAD_POSIX_FORK EQUAL -1)
  message(FATAL_ERROR "Linux launcher regression: fork() returned to the multithreaded authoring path")
endif()

# Likewise, Windows popen must receive the prepared command rather than the
# original command that can begin with a quoted Program Files executable.
string(FIND "${AUTHOR}" "_popen(command.c_str()" BAD_POPEN)
if(NOT BAD_POPEN EQUAL -1)
  message(FATAL_ERROR "Windows Program Files shell-launch regression: direct _popen(command.c_str()) remains")
endif()

# Lock in the rationale that exposed this bug: the installed sibling ffprobe
# path is quoted and title probing uses shell redirection.
foreach(needle
  "Program Files executable"
  "shq_s(tools.ffprobe)"
  "ffprobe could not inspect title streams"
  "--- ffprobe output ---"
  "const int rc = shell_system(command);"
  " > "
  " 2> ")
  string(FIND "${AUTHOR}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "Windows Program Files shell-launch regression: missing probe/quote evidence ${needle}")
  endif()
endforeach()

foreach(needle
  "void configure_hidden_tool_process(QProcess& process)"
  "setCreateProcessArgumentsModifier"
  "args->flags|=CREATE_NO_WINDOW"
  "args->startupInfo->wShowWindow=SW_HIDE"
  "start_tool_process(p,program,args)"
  "start_tool_process(probe,g_tool_capabilities.ffprobe")
  string(FIND "${GUI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "Windows hidden-tool regression: missing GUI marker ${needle}")
  endif()
endforeach()

# No Windows helper launch may fall back to CRT _popen(), which would create a
# visible console window for a GUI application.
string(FIND "${AUTHOR}" "return _popen(" BAD_CRT_POPEN)
if(NOT BAD_CRT_POPEN EQUAL -1)
  message(FATAL_ERROR "Windows hidden-tool regression: CRT _popen() remains in author.cpp")
endif()

message(STATUS "Windows Program Files/hidden shell-launch checks ok")
