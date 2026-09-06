file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)

# FFmpeg 9 rejects the legacy `-top 1` AVOption for mpeg2video with:
# "Codec AVOption top (top field first) is not a encoding option."  Use the
# generic encoding/video field_order option instead.  The filter graph still
# marks actual frames TFF with setfield=tff where required.
string(FIND "${AUTHOR}" "-top 1" LEGACY_TOP_POS)
if(NOT LEGACY_TOP_POS EQUAL -1)
  message(FATAL_ERROR "legacy FFmpeg -top 1 option must not be emitted for MPEG-2 encoding")
endif()

string(REGEX MATCHALL "-flags \\+ildct\\+ilme -field_order tt" FIELD_ORDER_MATCHES "${AUTHOR}")
list(LENGTH FIELD_ORDER_MATCHES FIELD_ORDER_COUNT)
if(NOT FIELD_ORDER_COUNT EQUAL 2)
  message(FATAL_ERROR "expected DVD and Blu-ray interlaced MPEG-2 paths to emit -field_order tt; found ${FIELD_ORDER_COUNT}")
endif()

string(FIND "${AUTHOR}" "tinterlace=mode=interleave_top,setfield=tff" FILTER_TFF_POS)
if(FILTER_TFF_POS EQUAL -1)
  message(FATAL_ERROR "DVD menu filter chain must continue marking generated interlaced frames top-field-first")
endif()
