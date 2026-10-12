// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <filesystem>

namespace bdmvauthor {

// Verify the M2TS packet framing, actual PCR-bearing PID, PMT declaration,
// and CLPI SequenceInfo PCR PID after tsMuxer has written the files.
// Throws std::runtime_error with the offending clip name on failure.
// This is a preflight consistency check, not a replication certificate.
void validate_bluray_pcr_clip(const std::filesystem::path& stream,
                              const std::filesystem::path& clpi);

}  // namespace bdmvauthor
