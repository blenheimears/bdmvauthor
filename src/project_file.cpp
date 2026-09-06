// SPDX-License-Identifier: Apache-2.0
#include "bdmvauthor/project_file.hpp"
#include "bdmvauthor/media_fingerprint.hpp"
#include "bdmvauthor/font_renderer.hpp"

#include <QAbstractButton>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QFontDialog>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QStringList>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace bdmvauthor {
namespace {

constexpr int kProjectFormatVersion = 24;

[[noreturn]] void fail(const QString& message) {
    throw std::runtime_error(message.toStdString());
}

QString exact_installed_qt_font_family(const QString& requested) {
    const auto families = QFontDatabase::families();
    for (const auto& family : families)
        if (family.compare(requested, Qt::CaseInsensitive) == 0) return family;
    return {};
}

QString default_installed_qt_sans_font() {
    static const std::array<const char*,14> fallbacks{{
        "Arial", "Segoe UI", "Tahoma", "Verdana", "Calibri", "Liberation Sans", "DejaVu Sans",
        "Noto Sans", "Carlito", "Ubuntu", "FreeSans", "Nimbus Sans", "TeX Gyre Heros", "Helvetica"
    }};
    for (const char* candidate : fallbacks) {
        const auto installed = exact_installed_qt_font_family(QString::fromUtf8(candidate));
        if (!installed.isEmpty()) return installed;
    }
    fail("No suitable sans-serif font is installed. Install Arial or another common sans-serif font before opening or creating a project.");
}

bool choose_project_font_replacement(QWidget* parent, std::string& family, const QString& context,
                                     QHash<QString,QString>& replacements) {
    const QString missing = QString::fromStdString(family);
    const QString key = missing.toCaseFolded();
    if (const auto it = replacements.constFind(key); it != replacements.constEnd()) {
        family = it.value().toStdString();
        return true;
    }

    const bool generic = detail::is_generic_font_family(family);
    QMessageBox box(QMessageBox::Critical, "Project font unavailable",
        generic
            ? QString("%1 specifies the generic font family '%2'. Disc authoring requires a concrete installed font.\n\nChoose a suitable replacement font to continue opening this project.").arg(context, missing)
            : QString("%1 specifies the font '%2', but that font is not installed on this system.\n\nChoose a suitable replacement font to continue opening this project.").arg(context, missing),
        QMessageBox::NoButton, parent);
    auto* choose = box.addButton("Choose replacement font…", QMessageBox::AcceptRole);
    box.addButton("Cancel opening project", QMessageBox::RejectRole);
    box.exec();
    if (box.clickedButton() != choose) return false;

    bool accepted = false;
    const QFont initial = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
    const QFont selected = QFontDialog::getFont(&accepted, initial, parent, "Choose replacement font");
    if (!accepted) return false;
    const auto concrete = exact_installed_qt_font_family(selected.family());
    if (concrete.isEmpty()) fail("The selected replacement font is not available in the system font database.");
    replacements.insert(key, concrete);
    family = concrete.toStdString();
    return true;
}

bool concretize_project_font(QWidget* parent, std::string& family, const QString& context,
                             bool allow_legacy_generic_auto, QHash<QString,QString>& replacements) {
    const bool generic = detail::is_generic_font_family(family);
    if (family.empty() || (allow_legacy_generic_auto && generic)) {
        family = default_installed_qt_sans_font().toStdString();
        return true;
    }
    if (generic) return choose_project_font_replacement(parent, family, context, replacements);
    const auto installed = exact_installed_qt_font_family(QString::fromStdString(family));
    if (!installed.isEmpty()) {
        family = installed.toStdString();
        return true;
    }
    return choose_project_font_replacement(parent, family, context, replacements);
}

bool concretize_project_fonts(QWidget* parent, Project& project, bool allow_legacy_generic_auto,
                              bool* replacement_applied) {
    QHash<QString,QString> replacements;
    bool ok = true;
    auto font = [&](std::string& family, const QString& context) {
        if (ok) ok = concretize_project_font(parent, family, context, allow_legacy_generic_auto, replacements);
    };
    auto style = [&](ButtonStyle& s, const QString& context) { font(s.font_family, context); };
    std::function<void(Menu&, const QString&)> menu_fonts = [&](Menu& menu, const QString& parent_context) {
        if (!ok) return;
        const QString name = menu.name.empty() ? QStringLiteral("menu") : QString("menu '%1'").arg(QString::fromStdString(menu.name));
        const QString context = parent_context.isEmpty() ? name : parent_context + " / " + name;
        style(menu.default_button_style, context + " default button style");
        style(menu.back_button.style, context + " Back button style");
        for (std::size_t i = 0; i < menu.buttons.size() && ok; ++i)
            style(menu.buttons[i].style, context + QString(" button %1 style").arg(i + 1));
        for (std::size_t i = 0; i < menu.overlays.size() && ok; ++i)
            if (menu.overlays[i].kind == MenuOverlayKind::Text)
                font(menu.overlays[i].font_family, context + QString(" text label %1").arg(i + 1));
        for (auto& child : menu.submenus) menu_fonts(child, context);
    };
    menu_fonts(project.menu, {});
    for (std::size_t i = 0; i < project.titles.size() && ok; ++i) {
        auto& title = project.titles[i];
        const QString context = title.name.empty() ? QString("title %1").arg(i + 1)
                                                   : QString("title '%1'").arg(QString::fromStdString(title.name));
        font(title.subtitle_default_style.font_family, context + " subtitle defaults");
        for (std::size_t j = 0; j < title.subtitle_stream_settings.size() && ok; ++j)
            font(title.subtitle_stream_settings[j].style.font_family, context + QString(" subtitle stream %1").arg(j + 1));
        for (std::size_t j = 0; j < title.external_subtitles.size() && ok; ++j)
            font(title.external_subtitles[j].style.font_family, context + QString(" attached subtitle %1").arg(j + 1));
    }
    if (replacement_applied) *replacement_applied = !replacements.isEmpty();
    return ok;
}

void migrate_legacy_subtitle_defaults(Project& project) {
    auto migrate = [](SubtitleStyle& style) {
        // Before project format v23 these values were dormant whenever the
        // style override was disabled. Treat that state as unspecified rather
        // than converting the old 28 px / 0 px implementation defaults into a
        // new explicit size/placement.
        if (!style.override_style) {
            style.font_family.clear();
            style.font_size_px = 80;
            style.bottom_offset_px = 80;
        }
    };
    for (auto& title : project.titles) {
        migrate(title.subtitle_default_style);
        for (auto& stream : title.subtitle_stream_settings) migrate(stream.style);
        for (auto& subtitle : title.external_subtitles) migrate(subtitle.style);
    }
}

QString path_qstring(const std::filesystem::path& p) {
    return QString::fromStdString(p.string());
}
std::filesystem::path fs_path(const QString& s) { return std::filesystem::path(s.toStdString()); }

QJsonObject rgba_json(const Rgba& c) {
    return {{"r", static_cast<int>(c.r)}, {"g", static_cast<int>(c.g)},
            {"b", static_cast<int>(c.b)}, {"a", static_cast<int>(c.a)}};
}
Rgba rgba_from_json(const QJsonValue& v, const Rgba& fallback = {}) {
    if (!v.isObject()) return fallback;
    const auto o = v.toObject();
    auto component = [&](const char* key, std::uint8_t def) {
        const int n = o.value(QLatin1String(key)).toInt(static_cast<int>(def));
        return static_cast<std::uint8_t>(std::clamp(n, 0, 255));
    };
    return {component("r", fallback.r), component("g", fallback.g),
            component("b", fallback.b), component("a", fallback.a)};
}
QJsonObject rect_json(const Rect& r) {
    return {{"x", r.x}, {"y", r.y}, {"width", r.width}, {"height", r.height}};
}
Rect rect_from_json(const QJsonValue& v, const Rect& fallback = {}) {
    if (!v.isObject()) return fallback;
    const auto o = v.toObject();
    return {o.value("x").toInt(fallback.x), o.value("y").toInt(fallback.y),
            o.value("width").toInt(fallback.width), o.value("height").toInt(fallback.height)};
}

QString audio_name(AudioCodec v) {
    switch (v) {
        case AudioCodec::Ac3: return "ac3";
        case AudioCodec::Lpcm: return "lpcm";
        case AudioCodec::Dca: return "dca";
        case AudioCodec::TrueHdAc3: return "truehd-ac3";
    }
    return "ac3";
}
AudioCodec audio_from_name(const QString& s) {
    if (s == "lpcm") return AudioCodec::Lpcm;
    if (s == "dca") return AudioCodec::Dca;
    if (s == "truehd-ac3") return AudioCodec::TrueHdAc3;
    return AudioCodec::Ac3;
}
QString video_name(VideoCodec v) {
    switch (v) {
        case VideoCodec::X264: return "x264";
        case VideoCodec::Mpeg2: return "mpeg2";
        case VideoCodec::Hevc: return "hevc";
    }
    return "x264";
}
VideoCodec video_from_name(const QString& s) {
    if (s == "mpeg2") return VideoCodec::Mpeg2;
    if (s == "hevc") return VideoCodec::Hevc;
    return VideoCodec::X264;
}
QString target_name(DiscTarget target) {
    switch (target) {
        case DiscTarget::UltraHdBluRay2160: return "uhd-bluray-2160p";
        case DiscTarget::DvdVideo480p: return "dvd-video-480p";
        case DiscTarget::BluRay1080: return "bluray-1080p";
    }
    return "bluray-1080p";
}
DiscTarget target_from_name(const QString& s) {
    if (s == "uhd-bluray-2160p") return DiscTarget::UltraHdBluRay2160;
    if (s == "dvd-video-480p") return DiscTarget::DvdVideo480p;
    return DiscTarget::BluRay1080;
}

QJsonObject encoding_json(const EncodingProfile& e) {
    return {{"videoCodec", video_name(e.video_codec)},
            {"videoBitrateKbps", e.video_bitrate_kbps},
            {"videoMinrateKbps", e.video_min_bitrate_kbps},
            {"videoMaxrateKbps", e.video_max_bitrate_kbps},
            {"x264Preset", QString::fromStdString(e.x264_preset)},
            {"x265Preset", QString::fromStdString(e.x265_preset)},
            {"twoPass", e.two_pass},
            {"keyframeIntervalFrames", e.keyframe_interval_frames},
            {"audioCodec", audio_name(e.audio_codec)},
            {"ac3BitrateKbps", e.ac3_bitrate_kbps},
            {"dcaBitrateKbps", e.dca_bitrate_kbps},
            {"lpcmSampleRateHz", e.lpcm_sample_rate_hz},
            {"lpcmBitDepth", e.lpcm_bit_depth},
            {"x264AdvancedOptions", QString::fromStdString(e.x264_advanced_options)},
            {"x265AdvancedOptions", QString::fromStdString(e.x265_advanced_options)},
            {"mpeg2AdvancedOptions", QString::fromStdString(e.mpeg2_advanced_options)},
            {"ac3AdvancedOptions", QString::fromStdString(e.ac3_advanced_options)},
            {"dcaAdvancedOptions", QString::fromStdString(e.dca_advanced_options)},
            {"lpcmAdvancedOptions", QString::fromStdString(e.lpcm_advanced_options)},
            {"truehdAdvancedOptions", QString::fromStdString(e.truehd_advanced_options)}};
}
EncodingProfile encoding_from_json(const QJsonValue& v, EncodingProfile e = {}) {
    if (!v.isObject()) return e;
    const auto o = v.toObject();
    e.video_codec = video_from_name(o.value("videoCodec").toString("x264"));
    e.video_bitrate_kbps = o.value("videoBitrateKbps").toInt(e.video_bitrate_kbps);
    e.video_min_bitrate_kbps = o.value("videoMinrateKbps").toInt(e.video_min_bitrate_kbps);
    e.video_max_bitrate_kbps = o.value("videoMaxrateKbps").toInt(e.video_max_bitrate_kbps);
    e.x264_preset = o.value("x264Preset").toString(QString::fromStdString(e.x264_preset)).toStdString();
    e.x265_preset = o.value("x265Preset").toString(QString::fromStdString(e.x265_preset)).toStdString();
    e.two_pass = o.value("twoPass").toBool(e.two_pass);
    e.keyframe_interval_frames = o.value("keyframeIntervalFrames").toInt(e.keyframe_interval_frames);
    e.audio_codec = audio_from_name(o.value("audioCodec").toString("ac3"));
    e.ac3_bitrate_kbps = o.value("ac3BitrateKbps").toInt(e.ac3_bitrate_kbps);
    e.dca_bitrate_kbps = o.value("dcaBitrateKbps").toInt(e.dca_bitrate_kbps);
    e.lpcm_sample_rate_hz = o.value("lpcmSampleRateHz").toInt(e.lpcm_sample_rate_hz);
    e.lpcm_bit_depth = o.value("lpcmBitDepth").toInt(e.lpcm_bit_depth);
    e.x264_advanced_options = o.value("x264AdvancedOptions").toString().toStdString();
    e.x265_advanced_options = o.value("x265AdvancedOptions").toString().toStdString();
    e.mpeg2_advanced_options = o.value("mpeg2AdvancedOptions").toString().toStdString();
    e.ac3_advanced_options = o.value("ac3AdvancedOptions").toString().toStdString();
    e.dca_advanced_options = o.value("dcaAdvancedOptions").toString().toStdString();
    e.lpcm_advanced_options = o.value("lpcmAdvancedOptions").toString().toStdString();
    e.truehd_advanced_options = o.value("truehdAdvancedOptions").toString().toStdString();
    return e;
}
QJsonObject state_style_json(const ButtonStateStyle& s) {
    return {{"textColor", rgba_json(s.text_color)}, {"backgroundColor", rgba_json(s.background_color)},
            {"borderColor", rgba_json(s.border_color)}, {"borderWidth", s.border_width}};
}
ButtonStateStyle state_style_from_json(const QJsonValue& v, const ButtonStateStyle& fallback) {
    if (!v.isObject()) return fallback;
    const auto o = v.toObject();
    ButtonStateStyle s = fallback;
    s.text_color = rgba_from_json(o.value("textColor"), s.text_color);
    s.background_color = rgba_from_json(o.value("backgroundColor"), s.background_color);
    s.border_color = rgba_from_json(o.value("borderColor"), s.border_color);
    s.border_width = o.value("borderWidth").toInt(s.border_width);
    return s;
}
QJsonObject style_json(const ButtonStyle& s) {
    return {{"fontFamily", QString::fromStdString(s.font_family)}, {"fontSizePx", s.font_size_px},
            {"bold", s.bold}, {"italic", s.italic}, {"cornerRadius", s.corner_radius},
            {"normal", state_style_json(s.normal)}, {"active", state_style_json(s.active)},
            {"selected", state_style_json(s.selected)}, {"activated", state_style_json(s.activated)}};
}
ButtonStyle style_from_json(const QJsonValue& v) {
    ButtonStyle s;
    if (!v.isObject()) return s;
    const auto o = v.toObject();
    s.font_family = o.value("fontFamily").toString(QString::fromStdString(s.font_family)).toStdString();
    s.font_size_px = o.value("fontSizePx").toInt(s.font_size_px);
    s.bold = o.value("bold").toBool(s.bold);
    s.italic = o.value("italic").toBool(s.italic);
    s.corner_radius = o.value("cornerRadius").toInt(s.corner_radius);
    s.normal = state_style_from_json(o.value("normal"), s.normal);
    s.active = state_style_from_json(o.value("active"), s.active);
    s.selected = state_style_from_json(o.value("selected"), s.selected);
    s.activated = state_style_from_json(o.value("activated"), s.activated);
    return s;
}

QJsonObject fingerprint_file(const QString& path) {
    MediaFingerprint fp;
    try { fp = fingerprint_media_file(fs_path(path)); }
    catch (const std::exception& e) { fail(QString::fromUtf8(e.what())); }
    QJsonObject out{{"algorithm", "SHA-256"}, {"schemeVersion", 1},
                    {"fileSize", QString::number(static_cast<qulonglong>(fp.file_size))}};
    if (fp.whole_file) {
        out.insert("mode", "whole-file");
        out.insert("sha256", QString::fromStdString(fp.whole_sha256));
    } else {
        out.insert("mode", "first-middle-last-1MiB");
        out.insert("sampleBytes", QString::number(static_cast<qulonglong>(MediaFingerprint::SampleBytes)));
        out.insert("firstOffset", "0");
        out.insert("middleOffset", QString::number(static_cast<qulonglong>(fp.middle_offset)));
        out.insert("lastOffset", QString::number(static_cast<qulonglong>(fp.file_size - MediaFingerprint::SampleBytes)));
        out.insert("firstSha256", QString::fromStdString(fp.first_sha256));
        out.insert("middleSha256", QString::fromStdString(fp.middle_sha256));
        out.insert("lastSha256", QString::fromStdString(fp.last_sha256));
    }
    return out;
}

bool fingerprint_equal(const QJsonObject& a, const QJsonObject& b) {
    if (a.value("algorithm").toString() != b.value("algorithm").toString() ||
        a.value("schemeVersion").toInt() != b.value("schemeVersion").toInt() ||
        a.value("fileSize").toString() != b.value("fileSize").toString() ||
        a.value("mode").toString() != b.value("mode").toString()) return false;
    if (a.value("mode").toString() == "whole-file")
        return a.value("sha256").toString() == b.value("sha256").toString();
    return a.value("sampleBytes").toString() == b.value("sampleBytes").toString() &&
           a.value("firstOffset").toString() == b.value("firstOffset").toString() &&
           a.value("middleOffset").toString() == b.value("middleOffset").toString() &&
           a.value("lastOffset").toString() == b.value("lastOffset").toString() &&
           a.value("firstSha256").toString() == b.value("firstSha256").toString() &&
           a.value("middleSha256").toString() == b.value("middleSha256").toString() &&
           a.value("lastSha256").toString() == b.value("lastSha256").toString();
}

QString fingerprint_summary(const QJsonObject& fp) {
    if (fp.value("mode").toString() == "whole-file")
        return QString("SHA-256 (whole file): %1").arg(fp.value("sha256").toString());
    return QString("SHA-256 first 1 MiB: %1\nSHA-256 middle 1 MiB: %2\nSHA-256 last 1 MiB: %3")
        .arg(fp.value("firstSha256").toString(), fp.value("middleSha256").toString(),
             fp.value("lastSha256").toString());
}

struct FingerprintCache {
    QHash<QString, QJsonObject> values;
    QJsonObject get(const QString& path) {
        const QString absolute = QFileInfo(path).absoluteFilePath();
        const auto it = values.constFind(absolute);
        if (it != values.cend()) return *it;
        const auto fp = fingerprint_file(absolute);
        values.insert(absolute, fp);
        return fp;
    }
};

QJsonValue media_json(const std::filesystem::path& path, FingerprintCache& cache) {
    if (path.empty()) return QJsonValue();
    const QString absolute = QFileInfo(path_qstring(path)).absoluteFilePath();
    return QJsonObject{{"path", absolute}, {"fingerprint", cache.get(absolute)}};
}

QString media_filter(const QString& role) {
    if (role.contains("video", Qt::CaseInsensitive)) return "Video/media files (*)";
    if (role.contains("image", Qt::CaseInsensitive) || role.contains("background", Qt::CaseInsensitive))
        return "Image files (*)";
    if (role.contains("audio", Qt::CaseInsensitive)) return "Audio files (*)";
    return "Video/media files (*)";
}

enum class CandidateChoice { Use, Locate, Cancel };
CandidateChoice mismatch_dialog(QWidget* parent, const QString& role, const QString& path,
                                const QJsonObject& expected, const QJsonObject& actual) {
    QMessageBox box(QMessageBox::Warning, "Media file checksum mismatch",
        QString("The %1 file exists, but its stored SHA-256 fingerprint does not match:\n\n%2\n\nExpected:\n%3\n\nCurrent file:\n%4")
            .arg(role, path, fingerprint_summary(expected), fingerprint_summary(actual)),
        QMessageBox::NoButton, parent);
    auto* use = box.addButton("Use this file anyway", QMessageBox::AcceptRole);
    auto* locate = box.addButton("Select the correct file…", QMessageBox::ActionRole);
    auto* cancel = box.addButton("Cancel opening project", QMessageBox::RejectRole);
    box.exec();
    if (box.clickedButton() == use) return CandidateChoice::Use;
    if (box.clickedButton() == locate) return CandidateChoice::Locate;
    (void)cancel;
    return CandidateChoice::Cancel;
}

QString resolve_media(QWidget* parent, const QJsonValue& value, const QString& role, bool* cancelled) {
    if (!value.isObject()) return {};
    const auto o = value.toObject();
    QString candidate = o.value("path").toString();
    const auto expected = o.value("fingerprint").toObject();
    if (candidate.isEmpty()) return {};
    if (expected.value("algorithm").toString() != "SHA-256")
        fail(QString("Project uses an unsupported media fingerprint algorithm for %1").arg(role));

    while (true) {
        const QFileInfo info(candidate);
        if (!info.exists() || !info.isFile()) {
            QMessageBox box(QMessageBox::Warning, "Referenced media file is missing",
                QString("The %1 file referenced by this project cannot be found:\n\n%2\n\nSelect the appropriate file to relink it, or open the project with the path still missing.")
                    .arg(role, candidate), QMessageBox::NoButton, parent);
            auto* locate = box.addButton("Select file…", QMessageBox::AcceptRole);
            auto* keep = box.addButton("Open with missing file", QMessageBox::DestructiveRole);
            auto* cancel = box.addButton("Cancel opening project", QMessageBox::RejectRole);
            box.exec();
            if (box.clickedButton() == locate) {
                const QString selected = QFileDialog::getOpenFileName(parent, QString("Relink %1").arg(role), QFileInfo(candidate).absolutePath(), media_filter(role));
                if (selected.isEmpty()) continue;
                candidate = selected;
                continue;
            }
            if (box.clickedButton() == keep) return candidate;
            (void)cancel;
            *cancelled = true;
            return {};
        }

        QJsonObject actual;
        try { actual = fingerprint_file(candidate); }
        catch (const std::exception& e) { fail(QString::fromUtf8(e.what())); }
        if (fingerprint_equal(expected, actual)) return QFileInfo(candidate).absoluteFilePath();

        switch (mismatch_dialog(parent, role, candidate, expected, actual)) {
            case CandidateChoice::Use: return QFileInfo(candidate).absoluteFilePath();
            case CandidateChoice::Locate: {
                const QString selected = QFileDialog::getOpenFileName(parent, QString("Relink %1").arg(role), QFileInfo(candidate).absolutePath(), media_filter(role));
                if (!selected.isEmpty()) candidate = selected;
                break;
            }
            case CandidateChoice::Cancel:
                *cancelled = true;
                return {};
        }
    }
}


QString action_kind_name(NavigationActionKind kind) {
    switch (kind) {
        case NavigationActionKind::PlayTitle: return "play-title";
        case NavigationActionKind::Menu: return "menu";
        case NavigationActionKind::AudioTrack: return "audio-track";
        case NavigationActionKind::SubtitleTrack: return "subtitle-track";
        case NavigationActionKind::SubtitleOff: return "subtitle-off";
        case NavigationActionKind::RepeatBegin: return "repeat-begin";
        case NavigationActionKind::RepeatEnd: return "repeat-end";
    }
    return "play-title";
}
QJsonObject action_json(const NavigationAction& action) {
    return {{"kind", action_kind_name(action.kind)},
            {"targetTitle", static_cast<int>(action.target_title)},
            {"targetChapter", static_cast<int>(action.target_chapter)},
            {"targetStream", static_cast<int>(action.target_stream)},
            {"targetMenuId", QString::fromStdString(action.target_menu_id)},
            {"repeatCount", static_cast<int>(action.repeat_count)}};
}
NavigationAction action_from_json(const QJsonValue& value) {
    NavigationAction action;
    const auto o = value.toObject();
    const auto kind = o.value("kind").toString("play-title");
    if (kind == "menu") action.kind = NavigationActionKind::Menu;
    else if (kind == "audio-track") action.kind = NavigationActionKind::AudioTrack;
    else if (kind == "subtitle-track") action.kind = NavigationActionKind::SubtitleTrack;
    else if (kind == "subtitle-off") action.kind = NavigationActionKind::SubtitleOff;
    else if (kind == "repeat-begin") action.kind = NavigationActionKind::RepeatBegin;
    else if (kind == "repeat-end") action.kind = NavigationActionKind::RepeatEnd;
    else action.kind = NavigationActionKind::PlayTitle;
    action.target_title = static_cast<std::uint16_t>(std::clamp(o.value("targetTitle").toInt(1), 0, 65535));
    action.target_chapter = static_cast<std::uint16_t>(std::clamp(o.value("targetChapter").toInt(0), 0, 65535));
    action.target_stream = static_cast<std::uint16_t>(std::clamp(o.value("targetStream").toInt(1), 0, 65535));
    action.target_menu_id = o.value("targetMenuId").toString().toStdString();
    action.repeat_count = static_cast<std::uint16_t>(std::clamp(o.value("repeatCount").toInt(1), 0, 65535));
    return action;
}
QJsonArray actions_json(const std::vector<NavigationAction>& actions) {
    QJsonArray out;
    for (const auto& action : actions) out.append(action_json(action));
    return out;
}
std::vector<NavigationAction> actions_from_json(const QJsonValue& value) {
    std::vector<NavigationAction> out;
    for (const auto& item : value.toArray()) out.push_back(action_from_json(item));
    return out;
}

QJsonObject button_json(const MenuButton& b, FingerprintCache& cache) {
    const auto target_kind = [&]() -> QString {
        switch (b.target_kind) {
            case MenuButtonTargetKind::Title: return "title";
            case MenuButtonTargetKind::Menu: return "menu";
            case MenuButtonTargetKind::AudioTrack: return "audio-track";
            case MenuButtonTargetKind::SubtitleTrack: return "subtitle-track";
            case MenuButtonTargetKind::SubtitleOff: return "subtitle-off";
        }
        return "title";
    }();
    return {{"label", QString::fromStdString(b.label)},
            {"bounds", rect_json(b.bounds)},
            {"targetKind", target_kind},
            {"targetTitle", static_cast<int>(b.target_title)},
            {"targetChapter", static_cast<int>(b.target_chapter)},
            {"targetStream", static_cast<int>(b.target_stream)},
            {"targetMenuId", QString::fromStdString(b.target_menu_id)},
            {"actions", actions_json(button_action_sequence(b))},
            {"kind", b.kind == MenuButtonKind::Image ? "image" : "text"},
            {"useCustomStyle", b.use_custom_style}, {"style", style_json(b.style)},
            {"normalImage", media_json(b.normal_image, cache)},
            {"selectedImage", media_json(b.selected_image, cache)},
            {"imageHighlightColor", rgba_json(b.image_highlight_color)},
            {"autoSubmenuLink", b.auto_submenu_link}};
}
MenuButton button_from_json(QWidget* parent, const QJsonValue& v, bool* cancelled, const QString& context) {
    MenuButton b;
    if (!v.isObject()) return b;
    const auto o = v.toObject();
    b.label = o.value("label").toString().toStdString();
    b.bounds = rect_from_json(o.value("bounds"), b.bounds);
    const auto target_kind = o.value("targetKind").toString("title");
    if (target_kind == "menu") b.target_kind = MenuButtonTargetKind::Menu;
    else if (target_kind == "audio-track") b.target_kind = MenuButtonTargetKind::AudioTrack;
    else if (target_kind == "subtitle-track") b.target_kind = MenuButtonTargetKind::SubtitleTrack;
    else if (target_kind == "subtitle-off") b.target_kind = MenuButtonTargetKind::SubtitleOff;
    else b.target_kind = MenuButtonTargetKind::Title;
    b.target_title = static_cast<std::uint16_t>(std::clamp(o.value("targetTitle").toInt(1), 0, 65535));
    b.target_chapter = static_cast<std::uint16_t>(std::clamp(o.value("targetChapter").toInt(0), 0, 65535));
    b.target_stream = static_cast<std::uint16_t>(std::clamp(o.value("targetStream").toInt(1), 0, 65535));
    b.target_menu_id = o.value("targetMenuId").toString().toStdString();
    if (o.contains("actions")) b.actions = actions_from_json(o.value("actions"));
    else b.actions = {legacy_button_action(b)};
    mirror_first_action_to_legacy_target(b);
    b.kind = o.value("kind").toString("text") == "image" ? MenuButtonKind::Image : MenuButtonKind::Text;
    b.use_custom_style = o.value("useCustomStyle").toBool(false);
    b.style = style_from_json(o.value("style"));
    const QString normal = resolve_media(parent, o.value("normalImage"), context + " unselected button image", cancelled);
    if (*cancelled) return b;
    const QString selected = resolve_media(parent, o.value("selectedImage"), context + " selected button image", cancelled);
    if (*cancelled) return b;
    b.normal_image = fs_path(normal); b.selected_image = fs_path(selected);
    b.image_highlight_color = rgba_from_json(o.value("imageHighlightColor"), b.image_highlight_color);
    b.auto_submenu_link = o.value("autoSubmenuLink").toBool(false);
    return b;
}
QJsonObject overlay_json(const MenuOverlay& o, FingerprintCache& cache) {
    return {{"kind", o.kind == MenuOverlayKind::Image ? "image" : "text"},
            {"bounds", rect_json(o.bounds)}, {"text", QString::fromStdString(o.text)},
            {"fontFamily", QString::fromStdString(o.font_family)}, {"fontSizePx", o.font_size_px},
            {"bold", o.bold}, {"italic", o.italic}, {"textColor", rgba_json(o.text_color)},
            {"image", media_json(o.image, cache)}};
}
MenuOverlay overlay_from_json(QWidget* parent, const QJsonValue& v, bool* cancelled, const QString& context) {
    MenuOverlay x;
    if (!v.isObject()) return x;
    const auto o = v.toObject();
    x.kind = o.value("kind").toString("text") == "image" ? MenuOverlayKind::Image : MenuOverlayKind::Text;
    x.bounds = rect_from_json(o.value("bounds"), x.bounds);
    x.text = o.value("text").toString(QString::fromStdString(x.text)).toStdString();
    x.font_family = o.value("fontFamily").toString(QString::fromStdString(x.font_family)).toStdString();
    x.font_size_px = o.value("fontSizePx").toInt(x.font_size_px);
    x.bold = o.value("bold").toBool(x.bold); x.italic = o.value("italic").toBool(x.italic);
    x.text_color = rgba_from_json(o.value("textColor"), x.text_color);
    const QString image = resolve_media(parent, o.value("image"), context + " image label", cancelled);
    if (!*cancelled) x.image = fs_path(image);
    return x;
}

QJsonObject menu_json(const Menu& m, FingerprintCache& cache) {
    QJsonArray buttons; for (const auto& b : m.buttons) buttons.append(button_json(b, cache));
    QJsonArray overlays; for (const auto& o : m.overlays) overlays.append(overlay_json(o, cache));
    QJsonArray children; for (const auto& c : m.submenus) children.append(menu_json(c, cache));
    return {{"id", QString::fromStdString(m.id)}, {"name", QString::fromStdString(m.name)},
            {"inheritBackgroundImage", m.inherit_background_image}, {"inheritBackgroundColor", m.inherit_background_color},
            {"inheritButtonStyle", m.inherit_button_style}, {"inheritEncoding", m.inherit_encoding},
            {"inheritAudio", m.inherit_audio}, {"inheritDuration", m.inherit_duration},
            {"backgroundImage", media_json(m.background_image, cache)}, {"backgroundVideo", media_json(m.background_video, cache)}, {"backgroundColor", rgba_json(m.background_color)},
            {"audioSource", media_json(m.audio_source, cache)}, {"buttons", buttons}, {"overlays", overlays},
            {"defaultButtonStyle", style_json(m.default_button_style)}, {"width", m.width}, {"height", m.height},
            {"durationSeconds", m.duration_seconds}, {"loopMedia", m.loop_media}, {"blurayEncoding", encoding_json(m.encoding)},
            {"uhdEncoding", encoding_json(m.uhd_encoding)}, {"dvdEncoding", encoding_json(m.dvd_encoding)},
            {"autoBackButton", m.auto_back_button}, {"backButton", button_json(m.back_button, cache)},
            {"submenus", children}};
}
Menu menu_from_json(QWidget* parent, const QJsonValue& v, bool* cancelled, const QString& inherited_context = {}) {
    Menu m;
    if (!v.isObject()) return m;
    const auto o = v.toObject();
    m.id = o.value("id").toString(QString::fromStdString(m.id)).toStdString();
    m.name = o.value("name").toString(QString::fromStdString(m.name)).toStdString();
    const QString context = inherited_context.isEmpty() ? QString("menu '%1'").arg(QString::fromStdString(m.name))
                                                        : inherited_context + " / " + QString::fromStdString(m.name);
    m.inherit_background_image = o.value("inheritBackgroundImage").toBool(m.inherit_background_image);
    m.inherit_background_color = o.value("inheritBackgroundColor").toBool(m.inherit_background_color);
    m.inherit_button_style = o.value("inheritButtonStyle").toBool(m.inherit_button_style);
    m.inherit_encoding = o.value("inheritEncoding").toBool(m.inherit_encoding);
    m.inherit_audio = o.value("inheritAudio").toBool(m.inherit_audio);
    m.inherit_duration = o.value("inheritDuration").toBool(m.inherit_duration);
    m.background_image = fs_path(resolve_media(parent, o.value("backgroundImage"), context + " background image", cancelled));
    if (*cancelled) return m;
    m.background_video = fs_path(resolve_media(parent, o.value("backgroundVideo"), context + " background video", cancelled));
    if (*cancelled) return m;
    m.background_color = rgba_from_json(o.value("backgroundColor"), m.background_color);
    m.audio_source = fs_path(resolve_media(parent, o.value("audioSource"), context + " audio", cancelled));
    if (*cancelled) return m;
    m.default_button_style = style_from_json(o.value("defaultButtonStyle"));
    m.width = o.value("width").toInt(m.width); m.height = o.value("height").toInt(m.height);
    m.duration_seconds = o.value("durationSeconds").toDouble(m.duration_seconds);
    m.loop_media = o.value("loopMedia").toBool(true);
    m.encoding = encoding_from_json(o.contains("blurayEncoding") ? o.value("blurayEncoding") : o.value("encoding"), EncodingProfile{});
    m.uhd_encoding = o.contains("uhdEncoding") ? encoding_from_json(o.value("uhdEncoding"), default_uhd_encoding_profile()) : default_uhd_encoding_profile();
    m.dvd_encoding = o.contains("dvdEncoding") ? encoding_from_json(o.value("dvdEncoding"), default_dvd_encoding_profile()) : default_dvd_encoding_profile();
    m.auto_back_button = o.value("autoBackButton").toBool(m.auto_back_button);
    m.back_button = button_from_json(parent, o.value("backButton"), cancelled, context + " Back button");
    if (*cancelled) return m;
    for (const auto& bv : o.value("buttons").toArray()) {
        m.buttons.push_back(button_from_json(parent, bv, cancelled, context));
        if (*cancelled) return m;
    }
    for (const auto& ov : o.value("overlays").toArray()) {
        m.overlays.push_back(overlay_from_json(parent, ov, cancelled, context));
        if (*cancelled) return m;
    }
    for (const auto& cv : o.value("submenus").toArray()) {
        m.submenus.push_back(menu_from_json(parent, cv, cancelled, context));
        if (*cancelled) return m;
    }
    return m;
}

void scale_style_for_4k(ButtonStyle& s) {
    s.font_size_px *= 2;
    s.corner_radius *= 2;
    s.normal.border_width *= 2;
    s.active.border_width *= 2;
    s.selected.border_width *= 2;
    s.activated.border_width *= 2;
}
void scale_rect_for_4k(Rect& r) {
    r.x *= 2; r.y *= 2; r.width *= 2; r.height *= 2;
}
void migrate_menu_design_to_4k(Menu& m) {
    if (m.width == kProjectDesignWidth && m.height == kProjectDesignHeight) return;
    if (m.width != 1920 || m.height != 1080) return;
    m.width = kProjectDesignWidth; m.height = kProjectDesignHeight;
    scale_style_for_4k(m.default_button_style);
    scale_rect_for_4k(m.back_button.bounds);
    if (m.back_button.use_custom_style) scale_style_for_4k(m.back_button.style);
    for (auto& b : m.buttons) {
        scale_rect_for_4k(b.bounds);
        if (b.use_custom_style) scale_style_for_4k(b.style);
    }
    for (auto& o : m.overlays) {
        scale_rect_for_4k(o.bounds);
        o.font_size_px *= 2;
    }
    for (auto& c : m.submenus) migrate_menu_design_to_4k(c);
}

QString chapter_mode_name(ChapterMode mode) {
    switch (mode) {
        case ChapterMode::SourceOrFiveMinute: return "source-or-5min";
        case ChapterMode::Manual: return "manual";
        case ChapterMode::Interval: return "interval";
        case ChapterMode::None: return "none";
    }
    return "source-or-5min";
}
ChapterMode chapter_mode_from_name(const QString& name) {
    if (name == "manual") return ChapterMode::Manual;
    if (name == "interval") return ChapterMode::Interval;
    if (name == "none") return ChapterMode::None;
    return ChapterMode::SourceOrFiveMinute;
}


struct UserOperationName { UserOperation op; const char* name; };
constexpr std::array<UserOperationName,40> kUserOperationNames{{
    {UserOperation::MenuCall,"menu-call"},{UserOperation::TitleSearch,"title-search"},
    {UserOperation::ChapterSearch,"chapter-search"},{UserOperation::TimeSearch,"time-search"},
    {UserOperation::SkipNext,"skip-next"},{UserOperation::SkipPrevious,"skip-previous"},
    {UserOperation::Stop,"stop"},{UserOperation::Pause,"pause"},{UserOperation::StillOff,"still-off"},
    {UserOperation::ForwardPlay,"forward-play"},{UserOperation::BackwardPlay,"backward-play"},
    {UserOperation::Resume,"resume"},{UserOperation::MoveUp,"move-up"},{UserOperation::MoveDown,"move-down"},
    {UserOperation::MoveLeft,"move-left"},{UserOperation::MoveRight,"move-right"},
    {UserOperation::SelectButton,"select-button"},{UserOperation::ActivateButton,"activate-button"},
    {UserOperation::SelectAndActivate,"select-and-activate"},{UserOperation::PrimaryAudioChange,"primary-audio-change"},
    {UserOperation::AngleChange,"angle-change"},{UserOperation::PopupOn,"popup-on"},{UserOperation::PopupOff,"popup-off"},
    {UserOperation::SubtitleEnableDisable,"subtitle-enable-disable"},{UserOperation::SubtitleChange,"subtitle-change"},
    {UserOperation::SecondaryVideoEnableDisable,"secondary-video-enable-disable"},
    {UserOperation::SecondaryVideoChange,"secondary-video-change"},
    {UserOperation::SecondaryAudioEnableDisable,"secondary-audio-enable-disable"},
    {UserOperation::SecondaryAudioChange,"secondary-audio-change"},{UserOperation::PipSubtitleChange,"pip-subtitle-change"},
    {UserOperation::GoUp,"go-up"},{UserOperation::TitleMenuCall,"title-menu-call"},
    {UserOperation::SubtitleMenuCall,"subtitle-menu-call"},{UserOperation::AudioMenuCall,"audio-menu-call"},
    {UserOperation::AngleMenuCall,"angle-menu-call"},{UserOperation::ChapterMenuCall,"chapter-menu-call"},
    {UserOperation::KaraokeAudioMixChange,"karaoke-audio-mix-change"},
    {UserOperation::VideoPresentationModeChange,"video-presentation-mode-change"},
    {UserOperation::TitleOrTimePlay,"title-or-time-play"},{UserOperation::ChapterSearchOrPlay,"chapter-search-or-play"}
}};
QJsonArray user_operations_json(std::uint64_t mask) {
    QJsonArray out;
    for (const auto& item : kUserOperationNames) if (user_operation_prohibited(mask,item.op)) out.append(item.name);
    return out;
}
std::uint64_t user_operations_from_json(const QJsonValue& value) {
    std::uint64_t mask=0;
    for (const auto& v : value.toArray()) {
        const auto name=v.toString();
        for (const auto& item : kUserOperationNames) if (name==item.name) { mask|=user_operation_bit(item.op); break; }
    }
    return mask;
}

QJsonObject audio_stream_settings_json(const AudioStreamSettings& stream) {
    return {{"sourceOrdinal", stream.source_ordinal},
            {"language", QString::fromStdString(stream.language)},
            {"overrideEncoding", stream.override_encoding},
            {"outputChannels", stream.output_channels},
            {"blurayEncoding", encoding_json(stream.encoding)},
            {"uhdEncoding", encoding_json(stream.uhd_encoding)},
            {"dvdEncoding", encoding_json(stream.dvd_encoding)}};
}
AudioStreamSettings audio_stream_settings_from_json(const QJsonValue& value) {
    AudioStreamSettings stream;
    if (!value.isObject()) return stream;
    const auto o = value.toObject();
    stream.source_ordinal = o.value("sourceOrdinal").toInt(0);
    stream.language = o.value("language").toString().toStdString();
    stream.override_encoding = o.value("overrideEncoding").toBool(false);
    stream.output_channels = o.value("outputChannels").toInt(0);
    stream.encoding = encoding_from_json(o.value("blurayEncoding"), EncodingProfile{});
    stream.uhd_encoding = o.contains("uhdEncoding") ? encoding_from_json(o.value("uhdEncoding"), default_uhd_encoding_profile()) : default_uhd_encoding_profile();
    stream.dvd_encoding = o.contains("dvdEncoding") ? encoding_from_json(o.value("dvdEncoding"), default_dvd_encoding_profile()) : default_dvd_encoding_profile();
    return stream;
}
QJsonObject subtitle_style_json(const SubtitleStyle& style) {
    return {{"overrideStyle", style.override_style},
            {"fontFamily", QString::fromStdString(style.font_family)},
            {"fontSizeUnits", style.font_size_px}, {"fontColor", rgba_json(style.font_color)},
            {"bold", style.bold}, {"italic", style.italic}, {"underline", style.underline},
            {"strikeout", style.strikeout}, {"lineSpacing", style.line_spacing},
            {"borderWidth", style.border_width}, {"bottomOffsetUnits", style.bottom_offset_px},
            {"fadeInMs", style.fade_in_ms}, {"fadeOutMs", style.fade_out_ms},
            {"dvdOutlineColor", rgba_json(style.dvd_outline_color)},
            {"dvdShadowColor", rgba_json(style.dvd_shadow_color)},
            {"dvdShadowOffsetX", style.dvd_shadow_offset_x}, {"dvdShadowOffsetY", style.dvd_shadow_offset_y},
            {"dvdHorizontalAlignment", QString::fromStdString(style.dvd_horizontal_alignment)},
            {"dvdVerticalAlignment", QString::fromStdString(style.dvd_vertical_alignment)},
            {"dvdLeftMarginPx", style.dvd_left_margin_px}, {"dvdRightMarginPx", style.dvd_right_margin_px},
            {"dvdTopMarginPx", style.dvd_top_margin_px}, {"dvdBottomMarginPx", style.dvd_bottom_margin_px},
            {"dvdForceDisplay", style.dvd_force_display}};
}
SubtitleStyle subtitle_style_from_json(const QJsonValue& value) {
    SubtitleStyle style;
    if (!value.isObject()) return style;
    const auto o = value.toObject();
    style.override_style = o.value("overrideStyle").toBool(false);
    style.font_family = o.value("fontFamily").toString().toStdString();
    style.font_size_px = o.contains("fontSizeUnits") ? o.value("fontSizeUnits").toInt(80) : o.value("fontSizePx").toInt(80);
    style.font_color = rgba_from_json(o.value("fontColor"), {255,255,255,255});
    style.bold = o.value("bold").toBool(false); style.italic = o.value("italic").toBool(false);
    style.underline = o.value("underline").toBool(false); style.strikeout = o.value("strikeout").toBool(false);
    style.line_spacing = o.value("lineSpacing").toDouble(1.0);
    style.border_width = o.value("borderWidth").toDouble(2.0);
    style.bottom_offset_px = o.contains("bottomOffsetUnits") ? o.value("bottomOffsetUnits").toInt(80) : o.value("bottomOffsetPx").toInt(80);
    style.fade_in_ms = o.value("fadeInMs").toDouble(0.0); style.fade_out_ms = o.value("fadeOutMs").toDouble(0.0);
    style.dvd_outline_color = rgba_from_json(o.value("dvdOutlineColor"), {0,0,0,255});
    style.dvd_shadow_color = rgba_from_json(o.value("dvdShadowColor"), {0,0,0,255});
    style.dvd_shadow_offset_x = o.value("dvdShadowOffsetX").toInt(0); style.dvd_shadow_offset_y = o.value("dvdShadowOffsetY").toInt(0);
    style.dvd_horizontal_alignment = o.value("dvdHorizontalAlignment").toString("center").toStdString();
    style.dvd_vertical_alignment = o.value("dvdVerticalAlignment").toString("bottom").toStdString();
    style.dvd_left_margin_px = o.value("dvdLeftMarginPx").toInt(40); style.dvd_right_margin_px = o.value("dvdRightMarginPx").toInt(40);
    style.dvd_top_margin_px = o.value("dvdTopMarginPx").toInt(20); style.dvd_bottom_margin_px = o.value("dvdBottomMarginPx").toInt(36);
    style.dvd_force_display = o.value("dvdForceDisplay").toBool(false);
    return style;
}
QJsonObject subtitle_stream_settings_json(const SubtitleStreamSettings& stream) {
    return {{"sourceOrdinal", stream.source_ordinal}, {"language", QString::fromStdString(stream.language)},
            {"style", subtitle_style_json(stream.style)}};
}
SubtitleStreamSettings subtitle_stream_settings_from_json(const QJsonValue& value) {
    SubtitleStreamSettings stream;
    if (!value.isObject()) return stream;
    const auto o = value.toObject();
    stream.source_ordinal = o.value("sourceOrdinal").toInt(0);
    stream.language = o.value("language").toString().toStdString();
    if (o.contains("style")) stream.style = subtitle_style_from_json(o.value("style"));
    return stream;
}

QJsonObject title_json(const Title& t, FingerprintCache& cache) {
    QJsonArray chapters; for (double c : t.chapters_seconds) chapters.append(c);
    QJsonArray audio_streams; for (const auto& stream : t.audio_stream_settings) audio_streams.append(audio_stream_settings_json(stream));
    QJsonArray subtitle_streams; for (const auto& stream : t.subtitle_stream_settings) subtitle_streams.append(subtitle_stream_settings_json(stream));
    QJsonArray external_subtitles; for (const auto& sub : t.external_subtitles) external_subtitles.append(QJsonObject{{"source", media_json(sub.source, cache)}, {"language", QString::fromStdString(sub.language)}, {"style", subtitle_style_json(sub.style)}});
    return {{"name", QString::fromStdString(t.name)}, {"source", media_json(t.source, cache)},
            {"audioLanguage", QString::fromStdString(t.audio_language)},
            {"defaultAudioStream", t.default_audio_stream}, {"defaultSubtitleStream", t.default_subtitle_stream},
            {"audioStreams", audio_streams}, {"subtitleStreams", subtitle_streams}, {"externalSubtitles", external_subtitles},
            {"subtitleDefaults", subtitle_style_json(t.subtitle_default_style)},
            {"chapterMode", chapter_mode_name(t.chapter_mode)},
            {"chapterIntervalSeconds", t.chapter_interval_seconds},
            {"chaptersSeconds", chapters},
            {"blurayFrameRate", QString::fromStdString(t.frame_rate)},
            {"uhdFrameRate", QString::fromStdString(t.uhd_frame_rate)},
            {"dvdFrameRate", QString::fromStdString(t.dvd_frame_rate)},
            {"blurayResolution", QString::fromStdString(t.resolution)},
            {"uhdResolution", QString::fromStdString(t.uhd_resolution)},
            {"dvdResolution", QString::fromStdString(t.dvd_resolution)},
            {"blurayAspectRatio", QString::fromStdString(t.aspect_ratio)},
            {"uhdAspectRatio", QString::fromStdString(t.uhd_aspect_ratio)},
            {"dvdAspectRatio", QString::fromStdString(t.dvd_aspect_ratio)},
            {"blurayEncoding", encoding_json(t.encoding)}, {"uhdEncoding", encoding_json(t.uhd_encoding)},
            {"dvdEncoding", encoding_json(t.dvd_encoding)}, {"forceReencode", t.force_reencode},
            {"menuButtonActions", actions_json(t.menu_button_actions)},
            {"prohibitedUserOperations", user_operations_json(t.prohibited_user_operations)}};
}
Title title_from_json(QWidget* parent, const QJsonValue& v, bool* cancelled, int index) {
    Title t;
    if (!v.isObject()) return t;
    const auto o = v.toObject();
    t.name = o.value("name").toString().toStdString();
    const QString role = QString("title %1 (%2) video").arg(index + 1).arg(QString::fromStdString(t.name));
    t.source = fs_path(resolve_media(parent, o.value("source"), role, cancelled));
    if (*cancelled) return t;
    t.audio_language = o.value("audioLanguage").toString("eng").toStdString();
    t.default_audio_stream = o.value("defaultAudioStream").toInt(t.default_audio_stream);
    t.default_subtitle_stream = o.value("defaultSubtitleStream").toInt(t.default_subtitle_stream);
    for (const auto& stream : o.value("audioStreams").toArray()) t.audio_stream_settings.push_back(audio_stream_settings_from_json(stream));
    for (const auto& stream : o.value("subtitleStreams").toArray()) t.subtitle_stream_settings.push_back(subtitle_stream_settings_from_json(stream));
    if (o.contains("subtitleDefaults")) t.subtitle_default_style = subtitle_style_from_json(o.value("subtitleDefaults"));
    for (const auto& item : o.value("externalSubtitles").toArray()) {
        if (!item.isObject()) continue;
        const auto so = item.toObject();
        ExternalSubtitle sub;
        sub.source = fs_path(resolve_media(parent, so.value("source"), role + " external subtitle", cancelled));
        if (*cancelled) return t;
        sub.language = so.value("language").toString("und").toStdString();
        if (so.contains("style")) sub.style = subtitle_style_from_json(so.value("style"));
        t.external_subtitles.push_back(std::move(sub));
    }
    for (const auto& c : o.value("chaptersSeconds").toArray()) t.chapters_seconds.push_back(c.toDouble());
    if (o.contains("chapterMode")) {
        t.chapter_mode = chapter_mode_from_name(o.value("chapterMode").toString());
        t.chapter_interval_seconds = o.value("chapterIntervalSeconds").toDouble(300.0);
    } else {
        // v1-v4 only had an explicit chapter list. Preserve non-empty lists as
        // manual authoring; empty legacy lists adopt the new source/fallback default.
        t.chapter_mode = t.chapters_seconds.empty() ? ChapterMode::SourceOrFiveMinute : ChapterMode::Manual;
        t.chapter_interval_seconds = 300.0;
    }
    t.frame_rate = o.value("blurayFrameRate").toString("inherit").toStdString();
    t.uhd_frame_rate = o.value("uhdFrameRate").toString("inherit").toStdString();
    t.dvd_frame_rate = o.value("dvdFrameRate").toString("inherit").toStdString();
    t.resolution = o.value("blurayResolution").toString("auto").toStdString();
    t.uhd_resolution = o.value("uhdResolution").toString("auto").toStdString();
    t.dvd_resolution = o.value("dvdResolution").toString("auto").toStdString();
    t.aspect_ratio = o.value("blurayAspectRatio").toString("auto").toStdString();
    t.uhd_aspect_ratio = o.value("uhdAspectRatio").toString("auto").toStdString();
    t.dvd_aspect_ratio = o.value("dvdAspectRatio").toString("auto").toStdString();
    t.encoding = encoding_from_json(o.contains("blurayEncoding") ? o.value("blurayEncoding") : o.value("encoding"), EncodingProfile{});
    t.uhd_encoding = o.contains("uhdEncoding") ? encoding_from_json(o.value("uhdEncoding"), default_uhd_encoding_profile()) : default_uhd_encoding_profile();
    t.dvd_encoding = o.contains("dvdEncoding") ? encoding_from_json(o.value("dvdEncoding"), default_dvd_encoding_profile()) : default_dvd_encoding_profile();
    t.force_reencode = o.value("forceReencode").toBool(false);
    if (o.contains("menuButtonActions")) t.menu_button_actions = actions_from_json(o.value("menuButtonActions"));
    if (o.contains("prohibitedUserOperations")) t.prohibited_user_operations = user_operations_from_json(o.value("prohibitedUserOperations"));
    return t;
}

} // namespace

bool save_project_file(QWidget*, const Project& project, const QString& file_name, QString* error) {
    try {
        FingerprintCache cache;
        QJsonArray titles;
        for (const auto& t : project.titles) titles.append(title_json(t, cache));
        QJsonObject root{{"format", "BDMV Author Project"}, {"version", kProjectFormatVersion},
                         {"volumeLabel", QString::fromStdString(project.volume_label)},
                         {"outputImage", path_qstring(project.output_image)},
                         {"workDirectory", path_qstring(project.work_directory)},
                         {"target", target_name(project.target)},
                         {"discCapacityBytes", QString::number(static_cast<qulonglong>(project.disc_capacity_bytes))},
                         {"automaticVideoBitrate", project.automatic_video_bitrate},
                         {"frameRate", QString::fromStdString(project.frame_rate)},
                         {"blurayMenuResolution", QString::fromStdString(project.menu_resolution)},
                         {"uhdMenuResolution", QString::fromStdString(project.menu_uhd_resolution)},
                         {"dvdMenuResolution", QString::fromStdString(project.menu_dvd_resolution)},
                         {"blurayMenuAspectRatio", QString::fromStdString(project.menu_aspect_ratio)},
                         {"uhdMenuAspectRatio", QString::fromStdString(project.menu_uhd_aspect_ratio)},
                         {"dvdMenuAspectRatio", QString::fromStdString(project.menu_dvd_aspect_ratio)},
                         {"keepWorkDirectory", project.keep_work_directory},
                         {"useEncodeCache", project.use_encode_cache},
                         {"useComplianceCache", project.use_compliance_cache},
                         {"hasMenu", project_has_menu(project)},
                         {"firstPlayActions", actions_json(project.first_play_actions)},
                         {"titles", titles}, {"menu", menu_json(project.menu, cache)}};
        QSaveFile f(file_name);
        if (!f.open(QIODevice::WriteOnly)) fail(QString("Cannot save project:\n%1\n\n%2").arg(file_name, f.errorString()));
        const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
        if (f.write(bytes) != bytes.size()) fail(QString("Failed while writing project:\n%1").arg(file_name));
        if (!f.commit()) fail(QString("Failed to commit project file:\n%1\n\n%2").arg(file_name, f.errorString()));
        return true;
    } catch (const std::exception& e) {
        if (error) *error = QString::fromUtf8(e.what());
        return false;
    }
}

std::optional<Project> load_project_file(QWidget* parent, const QString& file_name, QString* error,
                                         bool* font_replacements_applied) {
    if (font_replacements_applied) *font_replacements_applied = false;
    try {
        QFile f(file_name);
        if (!f.open(QIODevice::ReadOnly)) fail(QString("Cannot open project:\n%1\n\n%2").arg(file_name, f.errorString()));
        QJsonParseError parse{};
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &parse);
        if (parse.error != QJsonParseError::NoError || !doc.isObject())
            fail(QString("Invalid BDMV Author project JSON at offset %1: %2").arg(parse.offset).arg(parse.errorString()));
        const auto o = doc.object();
        if (o.value("format").toString() != "BDMV Author Project") fail("Not a BDMV Author project file");
        const int version = o.value("version").toInt();
        if (version < 1 || version > kProjectFormatVersion) fail(QString("Unsupported BDMV Author project format version %1").arg(version));

        Project p;
        p.volume_label = o.value("volumeLabel").toString("BLURAY").toStdString();
        p.output_image = fs_path(o.value("outputImage").toString());
        p.work_directory = fs_path(o.value("workDirectory").toString());
        p.target = target_from_name(o.value("target").toString("bluray-1080p"));
        p.disc_capacity_bytes = default_disc_capacity_bytes(p.target);
        p.automatic_video_bitrate = version >= 22 ? o.value("automaticVideoBitrate").toBool(true) : false;
        if (version >= 22) {
            bool capacity_ok = false;
            const auto parsed_capacity = o.value("discCapacityBytes").toString().toULongLong(&capacity_ok);
            if (capacity_ok) p.disc_capacity_bytes = static_cast<std::uint64_t>(parsed_capacity);
        }
        p.frame_rate = o.value("frameRate").toString("auto").toStdString();
        if (version >= 11) {
            p.menu_resolution = o.value("blurayMenuResolution").toString("1920x1080").toStdString();
            p.menu_uhd_resolution = o.value("uhdMenuResolution").toString("1920x1080").toStdString();
            p.menu_dvd_resolution = o.value("dvdMenuResolution").toString("720x480").toStdString();
            p.menu_aspect_ratio = o.value("blurayMenuAspectRatio").toString("16:9").toStdString();
            p.menu_uhd_aspect_ratio = o.value("uhdMenuAspectRatio").toString("16:9").toStdString();
            p.menu_dvd_aspect_ratio = o.value("dvdMenuAspectRatio").toString("16:9").toStdString();
        } else {
            std::string legacy_resolution = o.value("menuResolution").toString("highest").toStdString();
            if (legacy_resolution == "highest") {
                if (p.target == DiscTarget::UltraHdBluRay2160) legacy_resolution = "3840x2160";
                else if (p.target == DiscTarget::DvdVideo480p) legacy_resolution = "720x480";
                else legacy_resolution = "1920x1080";
            }
            menu_resolution_for_target(p,p.target) = legacy_resolution;
            menu_aspect_ratio_for_target(p,p.target) = o.value("menuAspectRatio").toString("16:9").toStdString();
        }
        if (version <= 4 && p.frame_rate == "24000/1001") p.frame_rate = "auto";
        p.keep_work_directory = o.value("keepWorkDirectory").toBool(false);
        p.use_encode_cache = o.value("useEncodeCache").toBool(true);
        p.use_compliance_cache = version >= 19 ? o.value("useComplianceCache").toBool(true) : p.use_encode_cache;
        p.force_reencode = false;
        if (o.contains("firstPlayActions")) p.first_play_actions = actions_from_json(o.value("firstPlayActions"));
        bool cancelled = false;
        const auto ta = o.value("titles").toArray();
        for (int i = 0; i < ta.size(); ++i) {
            p.titles.push_back(title_from_json(parent, ta.at(i), &cancelled, i));
            if (cancelled) return std::nullopt;
        }
        p.menu = menu_from_json(parent, o.value("menu"), &cancelled);
        if (cancelled) return std::nullopt;
        if (version >= 12 && !o.value("hasMenu").toBool(true)) p.menu.id.clear();
        if (version == 1) migrate_menu_design_to_4k(p.menu);
        // v1-v22 wrote generic aliases such as sans-serif as historical defaults.
        // Their non-overridden subtitle size/position fields were also dormant,
        // so migrate those to the new automatic 80-su baseline rather than
        // treating the old implementation defaults as explicit choices.
        if (version <= 22) migrate_legacy_subtitle_defaults(p);
        bool replacements = false;
        if (!concretize_project_fonts(parent, p, version <= 22, &replacements)) return std::nullopt;
        if (font_replacements_applied) *font_replacements_applied = replacements;
        return p;
    } catch (const std::exception& e) {
        if (error) *error = QString::fromUtf8(e.what());
        return std::nullopt;
    }
}

} // namespace bdmvauthor
