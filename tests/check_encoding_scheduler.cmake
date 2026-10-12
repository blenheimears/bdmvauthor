if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR not supplied")
endif()
file(READ "${SOURCE_DIR}/include/bdmvauthor/author.hpp" H)
file(READ "${SOURCE_DIR}/src/author.cpp" A)
file(READ "${SOURCE_DIR}/src/gui.cpp" G)
file(READ "${SOURCE_DIR}/src/project_file.cpp" P)

foreach(needle
  "struct RemoteEncodingServer"
  "int port = 22;"
  "int parallel_encodes = 1;"
  "struct EncodingSchedulerSettings"
  "bool local_enabled = true;"
  "int local_parallel_encodes = 1;"
  "int retry_delay_seconds = 60;"
  "int maximum_retries = 15;"
  "bool retry_forever = false;"
  "int failover_after_consecutive_failures = 3;"
  "std::vector<RemoteEncodingServer> remote_servers"
  "EncodeJobProgressCallback")
  string(FIND "${H}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "scheduler API regression: missing ${needle}")
  endif()
endforeach()

foreach(needle
  "class EncodingWorkerPool"
  "no enabled encoding worker supports codec"
  "std::async(std::launch::async"
  "classify_encode_failure"
  "connection reset"
  "network is unreachable"
  "broken pipe"
  "device busy"
  "retrying on another worker in "
  "std::optional<std::size_t> avoid"
  "if(codec=="hdma")"
  "encode_audio_on_pool"
  "settings_.retry_delay_seconds"
  "settings_.retry_forever"
  "settings_.maximum_retries"
  "settings_.failover_after_consecutive_failures"
  "create_remote_passlog"
  "mktemp /tmp/bdmvauthor-2pass-"
  "remove_remote_passlog"
  "read_ffmpeg_progress_snapshot")
  string(FIND "${A}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "scheduler implementation regression: missing ${needle}")
  endif()
endforeach()

foreach(needle
  "remoteServers"
  "encodingScheduler/localEnabled"
  "encodingScheduler/localParallelEncodes"
  "encodingScheduler/retryDelaySeconds"
  "encodingScheduler/maximumRetries"
  "encodingScheduler/retryForever"
  "encodingScheduler/failoverAfterFailures"
  "class RemoteServersDialog"
  "class RemoteServerEditDialog"
  "Parallel encodes on this server"
  "Retry transient failures forever"
  "Encoding &workers and servers…"
  "class EncodeProgressDialog"
  "Each active encode is shown under the worker"
  "\"FPS\",\"Speed\",\"Status\",\"Attempt\""
  "encode_progress_dialog_->update_encode(ev)")
  string(FIND "${G}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "scheduler GUI/settings regression: missing ${needle}")
  endif()
endforeach()

foreach(forbidden
  "remoteServers"
  "encodingScheduler/"
  "retryForever"
  "parallelEncodes")
  string(FIND "${P}" "${forbidden}" pos)
  if(NOT pos EQUAL -1)
    message(FATAL_ERROR "application-only scheduler setting leaked into project file: ${forbidden}")
  endif()
endforeach()

message(STATUS "multi-server parallel scheduler/retry/progress source checks ok")
