// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "bdmvauthor/model.hpp"

#include <QString>
#include <optional>

class QWidget;

namespace bdmvauthor {

// Save/load the complete GUI authoring project. Media files are referenced by
// path and accompanied by SHA-256 fingerprints. Returns false/nullopt when the
// operation is cancelled or fails; error receives a user-facing explanation.
bool save_project_file(QWidget* parent, const Project& project, const QString& file_name,
                       QString* error = nullptr);
std::optional<Project> load_project_file(QWidget* parent, const QString& file_name,
                                         QString* error = nullptr,
                                         bool* font_replacements_applied = nullptr);

} // namespace bdmvauthor
