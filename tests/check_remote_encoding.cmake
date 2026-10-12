file(READ "${SOURCE_DIR}/include/bdmvauthor/author.hpp" AUTHOR_HPP)
file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)
file(READ "${SOURCE_DIR}/src/cli.cpp" CLI)
file(READ "${SOURCE_DIR}/src/project_file.cpp" PROJECT)
file(READ "${SOURCE_DIR}/flake.nix" FLAKE)

foreach(needle
  "bool remote_encoding = false"
  "std::string ssh = \"ssh\""
  "std::string remote_host"
  "int remote_ssh_port = 22"
  "std::string remote_ffmpeg = \"ffmpeg\""
  "std::string remote_vc1enc = \"vc1enc\""
  "std::string remote_hdmaenc = \"hdmaenc\""
)
  string(FIND "${AUTHOR_HPP}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing remote ToolPaths marker: ${needle}")
  endif()
endforeach()

foreach(needle
  "ssh_remote_command"
  "remote_compressed_feeder"
  "-c:v copy -an -sn -dn -f matroska - |"
  "remote_source_ffvhuff_feeder"
  "remote_source_video_info"
  "stream=codec_name,width,height"
  "remote_ffvhuff_downscale_locally"
  "source.width > geometry.video_width || source.height > geometry.video_height"
  "if (downscale_locally)"
  ":interl=1"
  "input_pre_scaled_for_target"
  "source_video_codec_is_uncompressed"
  "original compressed bitstream with configured remote decoder"
  "two-pass original compressed bitstream with configured remote decoder"
  "remote_ffmpeg_compressed_input"
  " -T -o BatchMode=yes"
  "ServerAliveInterval=15 -p "
  "tools.remote_ssh_port"
  "create_remote_passlog"
  "bdmvauthor-2pass-"
  "remove_remote_passlog"
  ".mbtree"
  ".cutree"
  "pipe:1"
  "tools.remote_vc1enc"
  "tools.remote_hdmaenc"
  "encodingLocation="
  "hdmaLocation="
  "remote:"
)
  string(FIND "${AUTHOR}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing remote streaming marker: ${needle}")
  endif()
endforeach()

foreach(needle
  "remoteEncoding/enabled"
  "remoteEncoding/host"
  "remoteEncoding/user"
  "remoteEncoding/port"
  "QString::number(c.remote_ssh_port)"
  "remoteEncoding/identityFile"
  "class RemoteServersDialog"
  "class RemoteServerEditDialog"
  "Encoding workers and retry policy"
  "SSH port"
  "remote_ffmpeg_hwaccels"
  "remote_ffmpeg_h264_hardware_encoders"
  "remote_ffmpeg_hevc_hardware_encoders"
  "Remote FFmpeg may be ffmpeg, ffmpeg.exe"
)
  string(FIND "${GUI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing remote GUI/settings marker: ${needle}")
  endif()
endforeach()

# Connection/runtime preferences are application settings, never project data.
foreach(forbidden
  "remoteEncoding/"
  "remote_host"
  "remote_ssh_port"
  "remote_identity_file"
)
  string(FIND "${PROJECT}" "${forbidden}" pos)
  if(NOT pos EQUAL -1)
    message(FATAL_ERROR "remote application setting leaked into project serialization: ${forbidden}")
  endif()
endforeach()

foreach(needle
  "--remote-encode HOST"
  "--remote-user USER"
  "--ssh-port PORT"
  "--ssh-identity FILE"
  "--remote-ffmpeg COMMAND"
  "--remote-vc1enc COMMAND"
  "--remote-hdmaenc COMMAND"
)
  string(FIND "${CLI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing remote CLI marker: ${needle}")
  endif()
endforeach()

string(REGEX MATCHALL "pkgs\.openssh" OPENSSH_MATCHES "${FLAKE}")
list(LENGTH OPENSSH_MATCHES OPENSSH_COUNT)
if(OPENSSH_COUNT LESS 2)
  message(FATAL_ERROR "flake.nix must include OpenSSH in both the wrapped runtime tool closure and nix develop shell")
endif()

# Remote title video must never fall back to decoded raw/Y4M transport.  If the
# source is uncompressed, or remote decoding of the original bitstream fails,
# the only network fallback is lossless FFVHUFF.  Downscales may reduce the
# active raster locally; same-size/upscale fallback preserves the source raster.
foreach(forbidden
  "locally decoded raw-frame relay"
  "remote_ffmpeg_raw_input")
  string(FIND "${AUTHOR}" "${forbidden}" pos)
  if(NOT pos EQUAL -1)
    message(FATAL_ERROR "obsolete remote raw-video relay remains: ${forbidden}")
  endif()
endforeach()
