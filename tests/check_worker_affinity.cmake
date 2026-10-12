if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR not supplied")
endif()
file(READ "${SOURCE_DIR}/include/bdmvauthor/model.hpp" M)
file(READ "${SOURCE_DIR}/include/bdmvauthor/author.hpp" H)
file(READ "${SOURCE_DIR}/src/author.cpp" A)
file(READ "${SOURCE_DIR}/src/gui.cpp" G)
file(READ "${SOURCE_DIR}/src/project_file.cpp" P)

foreach(needle
  "enum class EncodingWorkerAffinityMode"
  "Preferred = 1"
  "Required = 2"
  "std::string encoding_worker;"
  "EncodingWorkerAffinityMode encoding_worker_mode")
  string(FIND "${M}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "worker-affinity model regression: missing ${needle}")
  endif()
endforeach()

foreach(needle
  "bool local_fallback_when_remote_unavailable = false;"
  "EncodingWorkerAffinityMode affinity_mode"
  "affinity_mode==EncodingWorkerAffinityMode::Required"
  "affinity_mode==EncodingWorkerAffinityMode::Preferred"
  "fallback_only"
  "UnavailableCapability"
  "encoder unavailable; trying another worker"
  "mark_codec_unavailable"
  "unavailable_codecs"
  "audio-"+audio_codec_name(encoding.audio_codec)
  "audio_affinity"
  "encode_dvd_audio_on_pool")
  string(FIND "${H}${A}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "worker-affinity scheduler regression: missing ${needle}")
  endif()
endforeach()

foreach(needle
  "encodingScheduler/localFallbackWhenRemoteUnavailable"
  "encodingScheduler/workerAffinityPersistence"
  "Allow Local only when the requested encoder is unavailable on every remote worker"
  "Machine-local companion settings (recommended)"
  "Save worker assignments in project file"
  "Session only — do not save worker assignments"
  "Preferred — %1%2"
  "Required — %1%2"
  "Title video worker"
  "Encoding worker"
  "Inherit title assignment"
  "local_worker_affinity_path"
  "save_local_worker_affinities"
  "apply_local_worker_affinities"
  "clear_project_worker_affinities")
  string(FIND "${G}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "worker-affinity GUI/persistence regression: missing ${needle}")
  endif()
endforeach()

foreach(needle
  "encodingWorker"
  "encodingWorkerMode"
  "kProjectFormatVersion = 29")
  string(FIND "${P}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "worker-affinity project serialization regression: missing ${needle}")
  endif()
endforeach()

# Connection/server definitions remain app settings; the optional project fields
# contain only worker affinity names/modes and never SSH credentials or ports.
foreach(forbidden
  "remoteServers"
  "remoteHost"
  "remoteUser"
  "remoteSshPort"
  "identityFile"
  "localFallbackWhenRemoteUnavailable"
  "workerAffinityPersistence")
  string(FIND "${P}" "${forbidden}" pos)
  if(NOT pos EQUAL -1)
    message(FATAL_ERROR "application-only worker/server setting leaked into project serialization: ${forbidden}")
  endif()
endforeach()

message(STATUS "per-item worker affinity/fallback/persistence source checks ok")
