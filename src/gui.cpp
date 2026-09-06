// SPDX-License-Identifier: Apache-2.0
#include "bdmvauthor/author.hpp"
#include "bdmvauthor/font_renderer.hpp"
#include "bdmvauthor/project_file.hpp"
#include <QAction>
#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QBrush>
#include <QColor>
#include <QColorDialog>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QDir>
#include <QInputDialog>
#include <QIcon>
#include <QListWidget>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QScrollArea>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFontComboBox>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFrame>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsView>
#include <QGroupBox>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMetaObject>
#include <QPainter>
#include <QPen>
#include <QPainterPath>
#include <QPixmap>
#include <QProgressBar>
#include <QPlainTextEdit>
#include <QProcess>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QResizeEvent>
#include <QSettings>
#include <QSet>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QTableWidget>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <filesystem>
#include <fstream>
#include <ctime>
#include <iterator>
#include <limits>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace bdmvauthor;

#include "app_icon_data.inc"

namespace {
QIcon bdmvauthor_app_icon(){QPixmap p;p.loadFromData(kBdmvAuthorIconPng,kBdmvAuthorIconPngSize,"PNG");return QIcon(p);}

namespace fs=std::filesystem;

fs::path gui_cache_root(){
    if(const char* override_dir=std::getenv("BDMVAUTHOR_CACHE_DIR");override_dir&&*override_dir)return fs::path(override_dir);
    const char* home=std::getenv("HOME");
#ifdef _WIN32
    if(!home||!*home)home=std::getenv("USERPROFILE");
#endif
    if(!home||!*home)return {};
    return fs::path(home)/".cache"/"bdmvauthor"/"encoded-clips";
}

struct GuiCacheEntry{
    fs::path path;
    QString category,source,target,codec,resolution,frame_rate,settings;
    qint64 bitrate_kbps=0,created_unix=0,last_used_unix=0;
    quint64 size_bytes=0;
};

QString cache_category_label(const QString& category){
    if(category=="title-video")return "Title video";
    if(category=="title-audio")return "Title audio";
    if(category=="menu-video")return "Menu video";
    if(category=="menu-audio")return "Menu audio";
    if(category=="compliance-analysis")return "Compliance analysis";
    if(category=="video")return "Title video";
    if(category=="audio")return "Title audio";
    if(category=="compliance")return "Compliance analysis";
    return category.isEmpty()?QStringLiteral("Unknown"):category;
}

qint64 cache_file_mtime_unix(const fs::path& path){
    const QFileInfo info(QString::fromStdString(path.string()));
    return info.exists()?info.lastModified().toSecsSinceEpoch():0;
}

GuiCacheEntry read_cache_entry(const fs::path& data){
    GuiCacheEntry e;e.path=data;
    std::error_code ec;e.size_bytes=fs::file_size(data,ec);if(ec)e.size_bytes=0;
    e.category=QString::fromStdString(data.parent_path().filename().string());
    e.last_used_unix=cache_file_mtime_unix(data);e.created_unix=e.last_used_unix;
    std::ifstream f(data.string()+".meta");
    std::string magic;int version=0;
    if(f&&(f>>magic>>version)&&magic=="BDMVAUTHOR_CACHE_META"&&version==1){
        std::string key;
        while(f>>key){
            if(key=="category"){std::string v;if(f>>std::quoted(v))e.category=QString::fromStdString(v);}
            else if(key=="sourceName"){std::string v;if(f>>std::quoted(v))e.source=QString::fromStdString(v);}
            else if(key=="target"){std::string v;if(f>>std::quoted(v))e.target=QString::fromStdString(v);}
            else if(key=="codec"){std::string v;if(f>>std::quoted(v))e.codec=QString::fromStdString(v);}
            else if(key=="resolution"){std::string v;if(f>>std::quoted(v))e.resolution=QString::fromStdString(v);}
            else if(key=="frameRate"){std::string v;if(f>>std::quoted(v))e.frame_rate=QString::fromStdString(v);}
            else if(key=="bitrateKbps"){long long v=0;if(f>>v)e.bitrate_kbps=v;}
            else if(key=="settings"){std::string v;if(f>>std::quoted(v))e.settings=QString::fromStdString(v);}
            else if(key=="createdUnix"){long long v=0;if(f>>v)e.created_unix=v;}
            else if(key=="lastUsedUnix"){long long v=0;if(f>>v)e.last_used_unix=v;}
            else{std::string rest;std::getline(f,rest);}
        }
    }
    return e;
}

std::vector<GuiCacheEntry> scan_cache_entries(){
    std::vector<GuiCacheEntry> out;const auto root=gui_cache_root();if(root.empty())return out;
    std::error_code ec;if(!fs::is_directory(root,ec))return out;
    for(fs::recursive_directory_iterator it(root,fs::directory_options::skip_permission_denied,ec),end;it!=end;it.increment(ec)){
        if(ec){ec.clear();continue;}
        const auto p=it->path();const auto name=p.filename().string();
        if(it->is_directory(ec)&&!ec){if(name.find(".tmp-")!=std::string::npos)it.disable_recursion_pending();continue;}
        if(ec){ec.clear();continue;}if(!it->is_regular_file(ec)||ec)continue;
        if(name.size()>=5&&name.substr(name.size()-5)==".meta")continue;
        if(name.find(".tmp")!=std::string::npos)continue;
        out.push_back(read_cache_entry(p));
    }
    std::sort(out.begin(),out.end(),[](const GuiCacheEntry&a,const GuiCacheEntry&b){return a.last_used_unix>b.last_used_unix;});
    return out;
}

void remove_cache_entry_files(const fs::path& data){std::error_code ec;fs::remove(data,ec);ec.clear();fs::remove(fs::path(data.string()+".meta"),ec);}

quint64 cleanup_cache_unused_for_days(int days){
    if(days<=0)return 0;const qint64 cutoff=QDateTime::currentSecsSinceEpoch()-static_cast<qint64>(days)*86400;quint64 freed=0;
    for(const auto&e:scan_cache_entries())if(e.last_used_unix>0&&e.last_used_unix<cutoff){freed+=e.size_bytes;remove_cache_entry_files(e.path);}return freed;
}

QString human_cache_size(quint64 bytes){
    static const char* units[]={"B","KiB","MiB","GiB","TiB"};double value=static_cast<double>(bytes);int unit=0;while(value>=1024.0&&unit<4){value/=1024.0;++unit;}
    return QString::number(value,'f',unit==0?0:(value<10.0?2:1))+" "+units[unit];
}

class CacheManagerDialog final:public QDialog{
    QTableWidget* table_=nullptr;QLabel* total_=nullptr;QCheckBox* auto_cleanup_=nullptr;QSpinBox* days_=nullptr;
    std::vector<GuiCacheEntry> entries_;
    void refresh(){
        entries_=scan_cache_entries();table_->setRowCount(0);quint64 total=0;
        for(const auto&e:entries_){const int r=table_->rowCount();table_->insertRow(r);total+=e.size_bytes;
            const QString values[]={cache_category_label(e.category),e.source.isEmpty()?QStringLiteral("Unknown"):e.source,e.target,e.codec,e.resolution,e.frame_rate,
                e.bitrate_kbps>0?QString::number(e.bitrate_kbps)+" kb/s":QString(),e.settings,human_cache_size(e.size_bytes),
                e.created_unix>0?QDateTime::fromSecsSinceEpoch(e.created_unix).toString("yyyy-MM-dd HH:mm"):QStringLiteral("Unknown"),
                e.last_used_unix>0?QDateTime::fromSecsSinceEpoch(e.last_used_unix).toString("yyyy-MM-dd HH:mm"):QStringLiteral("Unknown")};
            for(int c=0;c<11;++c){auto* item=new QTableWidgetItem(values[c]);if(c==0)item->setData(Qt::UserRole,QString::fromStdString(e.path.string()));table_->setItem(r,c,item);}
        }
        total_->setText(QString("%1 entries — %2").arg(static_cast<qulonglong>(entries_.size())).arg(human_cache_size(total)));table_->resizeColumnsToContents();
    }
public:
    explicit CacheManagerDialog(QWidget*parent=nullptr):QDialog(parent){setWindowTitle("Encoded-media cache manager");resize(1250,650);auto*root=new QVBoxLayout(this);
        auto*note=new QLabel("Cached encodes and compliance analyses are content-addressed. Last-used time is refreshed whenever an entry is reused. Older entries created before metadata support may show Unknown details until they are reused.",this);note->setWordWrap(true);root->addWidget(note);
        table_=new QTableWidget(0,11,this);table_->setHorizontalHeaderLabels({"Type","Original file","Target","Codec","Resolution","Frame rate","Bitrate","Important settings","Size","Created","Last used"});table_->setSelectionBehavior(QAbstractItemView::SelectRows);table_->setSelectionMode(QAbstractItemView::ExtendedSelection);table_->horizontalHeader()->setSectionResizeMode(7,QHeaderView::Stretch);root->addWidget(table_,1);
        auto*policy=new QHBoxLayout;auto_cleanup_=new QCheckBox("Automatically delete entries unused for",this);days_=new QSpinBox(this);days_->setRange(1,3650);days_->setSuffix(" days");QSettings settings;auto_cleanup_->setChecked(settings.value("cache/automaticCleanupEnabled",true).toBool());days_->setValue(settings.value("cache/cleanupUnusedDays",30).toInt());days_->setEnabled(auto_cleanup_->isChecked());connect(auto_cleanup_,&QCheckBox::toggled,days_,&QWidget::setEnabled);policy->addWidget(auto_cleanup_);policy->addWidget(days_);policy->addStretch(1);total_=new QLabel(this);policy->addWidget(total_);root->addLayout(policy);
        auto*buttons=new QHBoxLayout;auto*delete_selected=new QPushButton("Delete selected",this);auto*delete_all=new QPushButton("Delete all",this);auto*refresh_button=new QPushButton("Refresh",this);auto*close=new QPushButton("Close",this);buttons->addWidget(delete_selected);buttons->addWidget(delete_all);buttons->addWidget(refresh_button);buttons->addStretch(1);buttons->addWidget(close);root->addLayout(buttons);
        connect(delete_selected,&QPushButton::clicked,this,[this]{const auto rows=table_->selectionModel()->selectedRows();if(rows.empty())return;if(QMessageBox::question(this,"Delete cached entries",QString("Delete %1 selected cache entries?").arg(rows.size()))!=QMessageBox::Yes)return;for(const auto&idx:rows){const auto path=table_->item(idx.row(),0)->data(Qt::UserRole).toString();remove_cache_entry_files(fs::path(path.toStdString()));}refresh();});
        connect(delete_all,&QPushButton::clicked,this,[this]{if(QMessageBox::warning(this,"Delete all cached entries","Delete all encoded-media and compliance cache entries?",QMessageBox::Yes|QMessageBox::No,QMessageBox::No)!=QMessageBox::Yes)return;const auto rootp=gui_cache_root();if(!rootp.empty()){std::error_code ec;fs::remove_all(rootp,ec);}refresh();});
        connect(refresh_button,&QPushButton::clicked,this,[this]{refresh();});connect(close,&QPushButton::clicked,this,&QDialog::accept);refresh();}
    ~CacheManagerDialog()override{QSettings settings;settings.setValue("cache/automaticCleanupEnabled",auto_cleanup_->isChecked());settings.setValue("cache/cleanupUnusedDays",days_->value());settings.sync();}
};

void maybe_run_scheduled_cache_cleanup(){QSettings settings;if(!settings.value("cache/automaticCleanupEnabled",true).toBool())return;const qint64 now=QDateTime::currentSecsSinceEpoch();const qint64 last=settings.value("cache/lastCleanupUnix",0).toLongLong();if(last>0&&now-last<20*60*60)return;cleanup_cache_unused_for_days(settings.value("cache/cleanupUnusedDays",30).toInt());settings.setValue("cache/lastCleanupUnix",now);}

struct ToolConfiguration {
    QString ffmpeg,ffprobe,x264,x265,tsmuxer,dvdauthor,spumux,mplex,mkisofs;
    QString h264_provider=QStringLiteral("ffmpeg");
    QString hevc_provider=QStringLiteral("ffmpeg");
};
struct ToolCapabilities {
    ToolConfiguration config;
    QString ffmpeg,ffprobe,x264,x265,tsmuxer,dvdauthor,spumux,mplex,mkisofs;
    bool ffmpeg_ok=false,ffprobe_ok=false,x264_ok=false,x265_ok=false,tsmuxer_ok=false;
    bool dvdauthor_ok=false,spumux_ok=false,mplex_ok=false,mkisofs_ok=false;
    bool ffmpeg_libx264=false,ffmpeg_libx265=false,ffmpeg_mpeg2=false;
    bool ffmpeg_ac3=false,ffmpeg_dca=false,ffmpeg_truehd=false,ffmpeg_pcm_s16le=false,ffmpeg_pcm_s16be=false,ffmpeg_pcm_s24le=false,ffmpeg_pcm_s24be=false;
    QStringList warnings;
};
ToolCapabilities g_tool_capabilities;

ToolConfiguration load_tool_configuration(){
    QSettings s;ToolConfiguration c;
    c.ffmpeg=s.value("tools/ffmpeg").toString();c.ffprobe=s.value("tools/ffprobe").toString();
    c.x264=s.value("tools/x264").toString();c.x265=s.value("tools/x265").toString();c.tsmuxer=s.value("tools/tsmuxer").toString();
    c.dvdauthor=s.value("tools/dvdauthor").toString();c.spumux=s.value("tools/spumux").toString();c.mplex=s.value("tools/mplex").toString();c.mkisofs=s.value("tools/mkisofs").toString();
    c.h264_provider=s.value("tools/h264Provider",QStringLiteral("ffmpeg")).toString();
    c.hevc_provider=s.value("tools/hevcProvider",QStringLiteral("ffmpeg")).toString();
    if(c.h264_provider!="standalone")c.h264_provider="ffmpeg";
    if(c.hevc_provider!="standalone")c.hevc_provider="ffmpeg";
    return c;
}
void save_tool_configuration(const ToolConfiguration& c){
    QSettings s;s.setValue("tools/ffmpeg",c.ffmpeg);s.setValue("tools/ffprobe",c.ffprobe);s.setValue("tools/x264",c.x264);s.setValue("tools/x265",c.x265);s.setValue("tools/tsmuxer",c.tsmuxer);
    s.setValue("tools/dvdauthor",c.dvdauthor);s.setValue("tools/spumux",c.spumux);s.setValue("tools/mplex",c.mplex);s.setValue("tools/mkisofs",c.mkisofs);
    s.setValue("tools/h264Provider",c.h264_provider);s.setValue("tools/hevcProvider",c.hevc_provider);
}
QString resolve_tool(const QString& override_path,const QString& command,bool sibling=false){
    const QString specified=override_path.trimmed();
    bool use_sibling=sibling;
    if(!use_sibling && !specified.isEmpty() && !specified.contains('/') && !specified.contains('\\')){
        QString bare=specified;
#ifdef _WIN32
        if(bare.endsWith(QStringLiteral(".exe"),Qt::CaseInsensitive))bare.chop(4);
#endif
        use_sibling=(bare.compare(command,Qt::CaseInsensitive)==0);
    }
    if(use_sibling){
        QString sibling_name=command;
#ifdef _WIN32
        if(!sibling_name.endsWith(QStringLiteral(".exe"),Qt::CaseInsensitive))sibling_name+=QStringLiteral(".exe");
#endif
        const QString candidate=QCoreApplication::applicationDirPath()+QDir::separator()+sibling_name;
        QFileInfo fi(candidate);if(fi.isFile()&&fi.isExecutable())return fi.absoluteFilePath();
    }
    if(!specified.isEmpty()){
        QFileInfo fi(specified);if(fi.isFile()&&fi.isExecutable())return fi.absoluteFilePath();
        const auto found=QStandardPaths::findExecutable(specified);if(!found.isEmpty())return found;
        return {};
    }
    return QStandardPaths::findExecutable(command);
}
void configure_hidden_tool_process(QProcess& process){
#if defined(_WIN32)
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args){
        args->flags|=CREATE_NO_WINDOW;
        args->startupInfo->dwFlags|=STARTF_USESHOWWINDOW;
        args->startupInfo->wShowWindow=SW_HIDE;
    });
#else
    (void)process;
#endif
}
void start_tool_process(QProcess& process,const QString& program,const QStringList& args){
    configure_hidden_tool_process(process);
    process.start(program,args);
}
QByteArray run_tool_capture(const QString& program,const QStringList& args,bool* ok=nullptr,int timeout_ms=6000){
    if(ok)*ok=false;
    if(program.isEmpty())return {};
    QProcess p;p.setProcessChannelMode(QProcess::MergedChannels);start_tool_process(p,program,args);
    if(!p.waitForStarted(2000))return {};
    if(!p.waitForFinished(timeout_ms)){p.kill();p.waitForFinished(1000);return {};}
    if(ok)*ok=(p.exitStatus()==QProcess::NormalExit&&p.exitCode()==0);
    return p.readAll();
}
bool output_has_token(const QByteArray& out,const QByteArray& token){
    const auto lines=out.split('\n');for(const auto& line:lines){const auto fields=line.simplified().split(' ');for(const auto& field:fields)if(field==token)return true;}return false;
}
ToolCapabilities probe_tool_capabilities(const ToolConfiguration& config){
    ToolCapabilities c;c.config=config;
#if defined(_WIN32)
    constexpr bool sibling_external_tools=true;
#else
    constexpr bool sibling_external_tools=false;
#endif
    c.ffmpeg=resolve_tool(config.ffmpeg,"ffmpeg",sibling_external_tools);c.ffprobe=resolve_tool(config.ffprobe,"ffprobe",sibling_external_tools);c.x264=resolve_tool(config.x264,"x264",sibling_external_tools);c.x265=resolve_tool(config.x265,"x265",sibling_external_tools);
    // These helpers are bundled as private siblings by Windows and portable Linux.
    c.tsmuxer=resolve_tool(config.tsmuxer,"tsmuxer",true);c.dvdauthor=resolve_tool(config.dvdauthor,"dvdauthor",true);c.spumux=resolve_tool(config.spumux,"spumux",true);c.mplex=resolve_tool(config.mplex,"mplex",true);c.mkisofs=resolve_tool(config.mkisofs,"mkisofs",true);
    run_tool_capture(c.ffmpeg,{"-hide_banner","-version"},&c.ffmpeg_ok);
    run_tool_capture(c.ffprobe,{"-hide_banner","-version"},&c.ffprobe_ok);
    if(c.ffmpeg_ok){bool enc_ok=false;const auto enc=run_tool_capture(c.ffmpeg,{"-hide_banner","-encoders"},&enc_ok,10000);if(enc_ok){c.ffmpeg_libx264=output_has_token(enc,"libx264");c.ffmpeg_libx265=output_has_token(enc,"libx265");c.ffmpeg_mpeg2=output_has_token(enc,"mpeg2video");c.ffmpeg_ac3=output_has_token(enc,"ac3");c.ffmpeg_dca=output_has_token(enc,"dca");c.ffmpeg_truehd=output_has_token(enc,"truehd");c.ffmpeg_pcm_s16le=output_has_token(enc,"pcm_s16le");c.ffmpeg_pcm_s16be=output_has_token(enc,"pcm_s16be");c.ffmpeg_pcm_s24le=output_has_token(enc,"pcm_s24le");c.ffmpeg_pcm_s24be=output_has_token(enc,"pcm_s24be");}}
    bool x264_run=false;const auto x264_help=run_tool_capture(c.x264,{"--fullhelp"},&x264_run,6000);c.x264_ok=x264_run&&x264_help.contains("--bluray-compat");
    bool x265_run=false;const auto x265_help=run_tool_capture(c.x265,{"--help"},&x265_run,6000);c.x265_ok=x265_run&&x265_help.contains("--uhd-bd")&&x265_help.contains("--output-depth");
    // The remaining helpers use differing/non-zero --help conventions, so an executable path is sufficient here; build-time failures still report the exact command.
    c.tsmuxer_ok=!c.tsmuxer.isEmpty();c.dvdauthor_ok=!c.dvdauthor.isEmpty();c.spumux_ok=!c.spumux.isEmpty();c.mplex_ok=!c.mplex.isEmpty();c.mkisofs_ok=!c.mkisofs.isEmpty();
    if(!c.ffmpeg_ok)c.warnings<<"FFmpeg was not found or could not run; encoding and media conversion are unavailable.";
    else {
        QStringList missing;if(!c.ffmpeg_libx264)missing<<"libx264";if(!c.ffmpeg_libx265)missing<<"libx265";if(!c.ffmpeg_mpeg2)missing<<"MPEG-2 video";if(!c.ffmpeg_ac3)missing<<"AC-3";if(!c.ffmpeg_dca)missing<<"DTS/DCA";if(!c.ffmpeg_truehd)missing<<"TrueHD";
        if(!missing.isEmpty())c.warnings<<QStringLiteral("The selected FFmpeg has partial encoder support; missing FFmpeg encoders: %1. Standalone x264/x265 may still provide H.264/H.265.").arg(missing.join(", "));
    }
    if(!c.ffprobe_ok)c.warnings<<"ffprobe was not found; source inspection is unavailable.";
    if(!c.tsmuxer_ok)c.warnings<<"tsMuxer was not found; Blu-ray and Ultra HD Blu-ray builds are unavailable.";
    if(!c.ffmpeg_mpeg2)c.warnings<<"FFmpeg MPEG-2 encoding is unavailable, so DVD-Video authoring is disabled.";
    if(!c.dvdauthor_ok||!c.spumux_ok||!c.mplex_ok||!c.mkisofs_ok)c.warnings<<"One or more DVD helpers (dvdauthor, spumux, mplex, mkisofs) are missing; DVD-Video authoring is disabled.";
    if(!c.ffmpeg_libx265&&!c.x265_ok)c.warnings<<"HEVC/H.265 encoding is unavailable. Ultra HD Blu-ray remains available only for legal 1080p AVC/H.264 projects when H.264 is available.";
    if(config.h264_provider=="ffmpeg"&&!c.ffmpeg_libx264&&c.x264_ok)c.warnings<<"FFmpeg was preferred for H.264 but lacks libx264; standalone x264 will be used instead.";
    if(config.h264_provider=="standalone"&&!c.x264_ok&&c.ffmpeg_libx264)c.warnings<<"Standalone x264 was preferred but is unavailable; FFmpeg/libx264 will be used instead.";
    if(config.hevc_provider=="ffmpeg"&&!c.ffmpeg_libx265&&c.x265_ok)c.warnings<<"FFmpeg was preferred for H.265 but lacks libx265; standalone x265 will be used instead.";
    if(config.hevc_provider=="standalone"&&!c.x265_ok&&c.ffmpeg_libx265)c.warnings<<"Standalone x265 was preferred but is unavailable; FFmpeg/libx265 will be used instead.";
    return c;
}
bool h264_available(){return g_tool_capabilities.ffmpeg_libx264||g_tool_capabilities.x264_ok;}
bool hevc_available(){return g_tool_capabilities.ffmpeg_libx265||g_tool_capabilities.x265_ok;}
bool video_codec_available(DiscTarget target,VideoCodec codec){
    if(!g_tool_capabilities.ffmpeg_ok||!g_tool_capabilities.ffprobe_ok)return false;
    if(codec==VideoCodec::X264)return h264_available();
    if(codec==VideoCodec::Hevc)return hevc_available();
    return g_tool_capabilities.ffmpeg_mpeg2 && (target!=DiscTarget::UltraHdBluRay2160);
}
bool audio_codec_available(DiscTarget target,AudioCodec codec){
    if(!g_tool_capabilities.ffmpeg_ok)return false;
    switch(codec){case AudioCodec::Ac3:return g_tool_capabilities.ffmpeg_ac3;case AudioCodec::Dca:return g_tool_capabilities.ffmpeg_dca;case AudioCodec::TrueHdAc3:return g_tool_capabilities.ffmpeg_truehd&&g_tool_capabilities.ffmpeg_ac3;case AudioCodec::Lpcm:return target==DiscTarget::DvdVideo480p?g_tool_capabilities.ffmpeg_pcm_s16be:g_tool_capabilities.ffmpeg_pcm_s16le;}return false;
}
bool target_available(DiscTarget target){
    if(!g_tool_capabilities.ffmpeg_ok||!g_tool_capabilities.ffprobe_ok)return false;
    if(target==DiscTarget::DvdVideo480p)return g_tool_capabilities.ffmpeg_mpeg2&&g_tool_capabilities.dvdauthor_ok&&g_tool_capabilities.spumux_ok&&g_tool_capabilities.mplex_ok&&g_tool_capabilities.mkisofs_ok;
    if(!g_tool_capabilities.tsmuxer_ok)return false;
    if(target==DiscTarget::UltraHdBluRay2160)return hevc_available()||h264_available();
    return h264_available()||g_tool_capabilities.ffmpeg_mpeg2;
}
VideoEncoderProvider resolved_h264_provider(){
    const bool standalone_first=g_tool_capabilities.config.h264_provider=="standalone";
    if(standalone_first&&g_tool_capabilities.x264_ok)return VideoEncoderProvider::Standalone;
    if(g_tool_capabilities.ffmpeg_libx264)return VideoEncoderProvider::Ffmpeg;
    return VideoEncoderProvider::Standalone;
}
VideoEncoderProvider resolved_hevc_provider(){
    const bool standalone_first=g_tool_capabilities.config.hevc_provider=="standalone";
    if(standalone_first&&g_tool_capabilities.x265_ok)return VideoEncoderProvider::Standalone;
    if(g_tool_capabilities.ffmpeg_libx265)return VideoEncoderProvider::Ffmpeg;
    return VideoEncoderProvider::Standalone;
}
ToolPaths author_tool_paths(){
    ToolPaths t;t.ffmpeg=g_tool_capabilities.ffmpeg.toStdString();t.ffprobe=g_tool_capabilities.ffprobe.toStdString();t.x264=g_tool_capabilities.x264.toStdString();t.x265=g_tool_capabilities.x265.toStdString();t.tsmuxer=g_tool_capabilities.tsmuxer.toStdString();t.dvdauthor=g_tool_capabilities.dvdauthor.toStdString();t.spumux=g_tool_capabilities.spumux.toStdString();t.mplex=g_tool_capabilities.mplex.toStdString();t.mkisofs=g_tool_capabilities.mkisofs.toStdString();t.h264_provider=resolved_h264_provider();t.hevc_provider=resolved_hevc_provider();return t;
}
void set_combo_item_enabled(QComboBox* c,int row,bool enabled,const QString& unavailable_reason={}){
    if(auto* model=qobject_cast<QStandardItemModel*>(c->model()))if(auto* item=model->item(row)){item->setEnabled(enabled);if(!enabled&&!unavailable_reason.isEmpty())item->setToolTip(unavailable_reason);}
}
void populate_video_combo(QComboBox* c, DiscTarget target, VideoCodec selected) {
    const QSignalBlocker blocker(c);
    c->clear();
    if (target == DiscTarget::BluRay1080) {
        c->addItem("x264 / H.264", static_cast<int>(VideoCodec::X264));
        c->addItem("FFmpeg MPEG-2", static_cast<int>(VideoCodec::Mpeg2));
    } else if (target == DiscTarget::UltraHdBluRay2160) {
        c->addItem("HEVC / H.265 (UHD)", static_cast<int>(VideoCodec::Hevc));
        c->addItem("AVC / H.264 (1080p23.976/24 only)", static_cast<int>(VideoCodec::X264));
    } else {
        c->addItem("FFmpeg MPEG-2 (DVD-Video)", static_cast<int>(VideoCodec::Mpeg2));
    }
    for(int row=0;row<c->count();++row){const auto codec=static_cast<VideoCodec>(c->itemData(row).toInt());set_combo_item_enabled(c,row,video_codec_available(target,codec),QStringLiteral("This encoder is unavailable with the currently configured external tools."));}
    int index=c->findData(static_cast<int>(selected));
    if(index<0)index=0;
    c->setCurrentIndex(index);
}
QComboBox* video_combo(QWidget* parent, DiscTarget target, VideoCodec selected) {
    auto* c=new QComboBox(parent);populate_video_combo(c,target,selected);return c;
}
void populate_audio_combo(QComboBox* c, DiscTarget target, AudioCodec selected) {
    const QSignalBlocker blocker(c);
    c->clear();
    c->addItem("AC-3 (default)", static_cast<int>(AudioCodec::Ac3));
    c->addItem("LPCM", static_cast<int>(AudioCodec::Lpcm));
    if(target != DiscTarget::DvdVideo480p){
        c->addItem("DTS / DCA", static_cast<int>(AudioCodec::Dca));
        c->addItem("TrueHD + AC-3 core (experimental)", static_cast<int>(AudioCodec::TrueHdAc3));
    }
    for(int row=0;row<c->count();++row){const auto codec=static_cast<AudioCodec>(c->itemData(row).toInt());set_combo_item_enabled(c,row,audio_codec_available(target,codec),QStringLiteral("This FFmpeg audio encoder is unavailable."));}
    int index=c->findData(static_cast<int>(selected));
    if(index<0)index=0;
    c->setCurrentIndex(index);
}
QComboBox* audio_combo(QWidget* parent, DiscTarget target, AudioCodec selected) {
    auto* c=new QComboBox(parent);populate_audio_combo(c,target,selected);return c;
}
QComboBox* preset_combo(QWidget* parent = nullptr) {
    auto* c = new QComboBox(parent);
    for (const char* p : {"ultrafast", "superfast", "veryfast", "faster", "fast", "medium", "slow", "slower", "veryslow", "placebo"})
        c->addItem(QString::fromLatin1(p));
    c->setCurrentText("medium");
    return c;
}
QSpinBox* bitrate_spin(QWidget* parent = nullptr) {
    auto* s = new QSpinBox(parent); s->setRange(1000,100000); s->setSingleStep(1000); s->setSuffix(" kb/s"); s->setValue(24000); return s;
}
QSpinBox* audio_bitrate_spin(QWidget* parent = nullptr) {
    auto* s = new QSpinBox(parent); s->setAlignment(Qt::AlignRight); return s;
}
int gui_keyframe_maximum(DiscTarget target,VideoCodec codec,const std::string& rate_token) {
    if(target==DiscTarget::DvdVideo480p){
        if(rate_token=="pal-dvd")return 15;
        if(rate_token=="film-dvd"||rate_token=="ntsc-dvd")return 18;
        return 15; // Auto/inherit must remain legal even if the disc resolves PAL.
    }
    double fps=0.0;
    if(rate_token=="23.976p")fps=24000.0/1001.0;else if(rate_token=="24p")fps=24.0;
    else if(rate_token=="25p"||rate_token=="50i")fps=25.0;else if(rate_token=="59.94i")fps=30000.0/1001.0;
    else if(rate_token=="50p")fps=50.0;else if(rate_token=="59.94p")fps=60000.0/1001.0;else if(rate_token=="60p")fps=60.0;
    if(fps<=0.0){
        // Auto/inherit can resolve to the slowest legal cadence. Use the most
        // restrictive maximum so an entered value is never accepted now and
        // rejected later merely because source timing was resolved.
        return codec==VideoCodec::Mpeg2?24:25;
    }
    return maximum_keyframe_interval_for_rate(target,codec,fps,false);
}
QSpinBox* keyframe_interval_spin(QWidget* parent = nullptr, int value = 0) {
    auto* s = new QSpinBox(parent); s->setRange(0,25); s->setSuffix(" frames");
    s->setSpecialValueText("Default"); s->setValue(std::max(0,value));
    s->setToolTip("0 keeps BDMV Author's historical codec/timing-specific GOP interval. The maximum updates immediately when target, codec, or frame rate changes.");
    return s;
}
void configure_keyframe_interval_spin(QSpinBox* spin,DiscTarget target,VideoCodec codec,const std::string& rate_token,int requested=-1){
    const QSignalBlocker blocker(spin);const int old=requested>=0?requested:spin->value();const int maximum=gui_keyframe_maximum(target,codec,rate_token);spin->setRange(0,maximum);spin->setValue(std::clamp(old,0,maximum));spin->setToolTip(QString("0 = Default. Legal explicit range for the current selection: 1–%1 frames.").arg(maximum));
}
void configure_audio_bitrate_spin(QSpinBox* spin,DiscTarget target,AudioCodec codec,int bitrate,bool allow_exceeding_format_limits=false) {
    const QSignalBlocker blocker(spin);
    spin->setSpecialValueText(QString());
    spin->setSuffix(" kb/s");
    if(codec==AudioCodec::Lpcm){spin->setRange(0,0);spin->setValue(0);spin->setSpecialValueText("Fixed");spin->setEnabled(false);return;}
    spin->setEnabled(true);
    if(allow_exceeding_format_limits){spin->setRange(1,1000000);spin->setSingleStep(32);}
    else if(codec==AudioCodec::Dca){spin->setRange(384,1536);spin->setSingleStep(32);}
    else{spin->setRange(192,target_max_ac3_bitrate_kbps(target));spin->setSingleStep(32);}
    spin->setValue(std::clamp(bitrate,spin->minimum(),spin->maximum()));
}
void configure_video_bitrate_spin(QSpinBox* spin, DiscTarget target, VideoCodec codec, int bitrate,bool allow_exceeding_format_limits=false) {
    const QSignalBlocker blocker(spin);
    spin->setRange(allow_exceeding_format_limits?1:1000,allow_exceeding_format_limits?1000000:video_codec_max_bitrate_kbps(target,codec));
    spin->setValue(std::clamp(bitrate,spin->minimum(),spin->maximum()));
}
QSpinBox* video_rate_limit_spin(QWidget* parent=nullptr){auto* s=new QSpinBox(parent);s->setSuffix(" kb/s");s->setSingleStep(1000);return s;}
void configure_video_minrate_spin(QSpinBox* spin,DiscTarget target,VideoCodec codec,int bitrate,bool allow_exceeding_format_limits=false){const QSignalBlocker blocker(spin);spin->setRange(0,allow_exceeding_format_limits?1000000:video_codec_max_bitrate_kbps(target,codec));spin->setValue(std::clamp(bitrate,spin->minimum(),spin->maximum()));}
void configure_video_maxrate_spin(QSpinBox* spin,DiscTarget target,VideoCodec codec,int bitrate,bool allow_exceeding_format_limits=false){const QSignalBlocker blocker(spin);spin->setRange(1,allow_exceeding_format_limits?1000000:video_codec_max_bitrate_kbps(target,codec));spin->setValue(std::clamp(bitrate,spin->minimum(),spin->maximum()));}
EncodingProfile profile_from_widgets(QComboBox* video,QSpinBox* bitrate,QComboBox* preset,QCheckBox* two_pass,QComboBox* audio,QSpinBox* audio_bitrate,const EncodingProfile* base=nullptr,QSpinBox* keyframe=nullptr,QSpinBox* minrate=nullptr,QSpinBox* maxrate=nullptr) {
    EncodingProfile e = base ? *base : EncodingProfile{}; e.video_codec=static_cast<VideoCodec>(video->currentData().toInt()); e.video_bitrate_kbps=bitrate->value();if(minrate)e.video_min_bitrate_kbps=minrate->value();if(maxrate)e.video_max_bitrate_kbps=maxrate->value(); if(e.video_codec==VideoCodec::Hevc)e.x265_preset=preset->currentText().toStdString();else e.x264_preset=preset->currentText().toStdString(); e.two_pass=two_pass->isChecked(); if(keyframe)e.keyframe_interval_frames=keyframe->value(); e.audio_codec=static_cast<AudioCodec>(audio->currentData().toInt()); if(audio_bitrate&&audio_codec_has_configurable_bitrate(e.audio_codec))set_audio_bitrate_kbps(e,audio_bitrate->value()); return e;
}
QVariantMap encoding_map(const EncodingProfile& e){QVariantMap m;m["videoCodec"]=static_cast<int>(e.video_codec);m["videoBitrate"]=e.video_bitrate_kbps;m["videoMinrate"]=e.video_min_bitrate_kbps;m["videoMaxrate"]=e.video_max_bitrate_kbps;m["x264Preset"]=QString::fromStdString(e.x264_preset);m["x265Preset"]=QString::fromStdString(e.x265_preset);m["twoPass"]=e.two_pass;m["keyframeIntervalFrames"]=e.keyframe_interval_frames;m["audioCodec"]=static_cast<int>(e.audio_codec);m["ac3Bitrate"]=e.ac3_bitrate_kbps;m["dcaBitrate"]=e.dca_bitrate_kbps;m["lpcmSampleRateHz"]=e.lpcm_sample_rate_hz;m["lpcmBitDepth"]=e.lpcm_bit_depth;m["x264AdvancedOptions"]=QString::fromStdString(e.x264_advanced_options);m["x265AdvancedOptions"]=QString::fromStdString(e.x265_advanced_options);m["mpeg2AdvancedOptions"]=QString::fromStdString(e.mpeg2_advanced_options);m["ac3AdvancedOptions"]=QString::fromStdString(e.ac3_advanced_options);m["dcaAdvancedOptions"]=QString::fromStdString(e.dca_advanced_options);m["lpcmAdvancedOptions"]=QString::fromStdString(e.lpcm_advanced_options);m["truehdAdvancedOptions"]=QString::fromStdString(e.truehd_advanced_options);return m;}
EncodingProfile encoding_from_map(const QVariant& value,const EncodingProfile& fallback){EncodingProfile e=fallback;const auto m=value.toMap();if(m.isEmpty())return e;if(m.contains("videoCodec"))e.video_codec=static_cast<VideoCodec>(m.value("videoCodec").toInt());if(m.contains("videoBitrate"))e.video_bitrate_kbps=m.value("videoBitrate").toInt();if(m.contains("videoMinrate"))e.video_min_bitrate_kbps=m.value("videoMinrate").toInt();if(m.contains("videoMaxrate"))e.video_max_bitrate_kbps=m.value("videoMaxrate").toInt();if(m.contains("x264Preset"))e.x264_preset=m.value("x264Preset").toString().toStdString();if(m.contains("x265Preset"))e.x265_preset=m.value("x265Preset").toString().toStdString();if(m.contains("twoPass"))e.two_pass=m.value("twoPass").toBool();if(m.contains("keyframeIntervalFrames"))e.keyframe_interval_frames=m.value("keyframeIntervalFrames").toInt();if(m.contains("audioCodec"))e.audio_codec=static_cast<AudioCodec>(m.value("audioCodec").toInt());if(m.contains("ac3Bitrate"))e.ac3_bitrate_kbps=m.value("ac3Bitrate").toInt();if(m.contains("dcaBitrate"))e.dca_bitrate_kbps=m.value("dcaBitrate").toInt();if(m.contains("lpcmSampleRateHz"))e.lpcm_sample_rate_hz=m.value("lpcmSampleRateHz").toInt();if(m.contains("lpcmBitDepth"))e.lpcm_bit_depth=m.value("lpcmBitDepth").toInt();if(m.contains("x264AdvancedOptions"))e.x264_advanced_options=m.value("x264AdvancedOptions").toString().toStdString();if(m.contains("x265AdvancedOptions"))e.x265_advanced_options=m.value("x265AdvancedOptions").toString().toStdString();if(m.contains("mpeg2AdvancedOptions"))e.mpeg2_advanced_options=m.value("mpeg2AdvancedOptions").toString().toStdString();if(m.contains("ac3AdvancedOptions"))e.ac3_advanced_options=m.value("ac3AdvancedOptions").toString().toStdString();if(m.contains("dcaAdvancedOptions"))e.dca_advanced_options=m.value("dcaAdvancedOptions").toString().toStdString();if(m.contains("lpcmAdvancedOptions"))e.lpcm_advanced_options=m.value("lpcmAdvancedOptions").toString().toStdString();if(m.contains("truehdAdvancedOptions"))e.truehd_advanced_options=m.value("truehdAdvancedOptions").toString().toStdString();return e;}

QString video_codec_display_name(VideoCodec codec){switch(codec){case VideoCodec::X264:return "x264 / H.264";case VideoCodec::Hevc:return "x265 / HEVC";case VideoCodec::Mpeg2:return "FFmpeg MPEG-2";}return "Video";}
QString audio_codec_display_name(AudioCodec codec){switch(codec){case AudioCodec::Ac3:return "FFmpeg AC-3";case AudioCodec::Dca:return "FFmpeg DTS/DCA";case AudioCodec::Lpcm:return "FFmpeg LPCM";case AudioCodec::TrueHdAc3:return "FFmpeg TrueHD";}return "Audio";}
QString tool_report(const ToolCapabilities& c){
    auto line=[](const QString& name,const QString& path,bool ok){return QStringLiteral("%1: %2").arg(name,ok?(path+"  [OK]"):QStringLiteral("not found / unusable"));};
    QStringList out;out<<line("FFmpeg",c.ffmpeg,c.ffmpeg_ok)<<line("ffprobe",c.ffprobe,c.ffprobe_ok)<<line("x264",c.x264,c.x264_ok)<<line("x265",c.x265,c.x265_ok)<<line("tsMuxer",c.tsmuxer,c.tsmuxer_ok)<<line("dvdauthor",c.dvdauthor,c.dvdauthor_ok)<<line("spumux",c.spumux,c.spumux_ok)<<line("mplex",c.mplex,c.mplex_ok)<<line("mkisofs",c.mkisofs,c.mkisofs_ok);
    if(c.ffmpeg_ok)out<<QStringLiteral("FFmpeg encoders: libx264=%1, libx265=%2, mpeg2video=%3, ac3=%4, dca=%5, truehd=%6, pcm_s16le=%7, pcm_s16be=%8, pcm_s24le=%9, pcm_s24be=%10").arg(c.ffmpeg_libx264?"yes":"no",c.ffmpeg_libx265?"yes":"no",c.ffmpeg_mpeg2?"yes":"no",c.ffmpeg_ac3?"yes":"no",c.ffmpeg_dca?"yes":"no",c.ffmpeg_truehd?"yes":"no",c.ffmpeg_pcm_s16le?"yes":"no",c.ffmpeg_pcm_s16be?"yes":"no",c.ffmpeg_pcm_s24le?"yes":"no",c.ffmpeg_pcm_s24be?"yes":"no");
    const auto hp=(c.ffmpeg_libx264||c.x264_ok)?(resolved_h264_provider()==VideoEncoderProvider::Ffmpeg?"FFmpeg/libx264":"standalone x264"):"unavailable";
    const auto xp=(c.ffmpeg_libx265||c.x265_ok)?(resolved_hevc_provider()==VideoEncoderProvider::Ffmpeg?"FFmpeg/libx265":"standalone x265"):"unavailable";
    out<<QStringLiteral("Active H.264 provider: %1").arg(hp)<<QStringLiteral("Active H.265 provider: %1").arg(xp);
    out<<QStringLiteral("Targets: Blu-ray=%1, UHD Blu-ray=%2, DVD-Video=%3").arg(target_available(DiscTarget::BluRay1080)?"enabled":"disabled",target_available(DiscTarget::UltraHdBluRay2160)?"enabled":"disabled",target_available(DiscTarget::DvdVideo480p)?"enabled":"disabled");
    if(!c.warnings.isEmpty()){out<<""<<"Warnings:";for(const auto& w:c.warnings)out<<QStringLiteral("• %1").arg(w);}return out.join('\n');
}
class ToolSettingsDialog final : public QDialog {
    ToolConfiguration value_;QPlainTextEdit* report_=nullptr;QComboBox *h264_provider_=nullptr,*hevc_provider_=nullptr;
    QLineEdit *ffmpeg_=nullptr,*ffprobe_=nullptr,*x264_=nullptr,*x265_=nullptr,*tsmuxer_=nullptr,*dvdauthor_=nullptr,*spumux_=nullptr,*mplex_=nullptr,*mkisofs_=nullptr;
    static QLineEdit* tool_row(QWidget* parent,QFormLayout* form,const QString& label,const QString& initial){
        auto* row=new QWidget(parent);auto* l=new QHBoxLayout(row);l->setContentsMargins(0,0,0,0);auto* edit=new QLineEdit(initial,row);edit->setPlaceholderText("Auto-detect from PATH");auto* browse=new QPushButton("…",row);browse->setFixedWidth(34);auto* clear=new QPushButton("Auto",row);clear->setFixedWidth(52);l->addWidget(edit,1);l->addWidget(browse);l->addWidget(clear);form->addRow(label,row);
        QObject::connect(browse,&QPushButton::clicked,row,[edit,row]{const auto p=QFileDialog::getOpenFileName(row,"Select executable",edit->text());if(!p.isEmpty())edit->setText(p);});QObject::connect(clear,&QPushButton::clicked,row,[edit]{edit->clear();});return edit;
    }
    ToolConfiguration current()const{ToolConfiguration c;c.ffmpeg=ffmpeg_->text();c.ffprobe=ffprobe_->text();c.x264=x264_->text();c.x265=x265_->text();c.tsmuxer=tsmuxer_->text();c.dvdauthor=dvdauthor_->text();c.spumux=spumux_->text();c.mplex=mplex_->text();c.mkisofs=mkisofs_->text();c.h264_provider=h264_provider_->currentData().toString();c.hevc_provider=hevc_provider_->currentData().toString();return c;}
    void rescan(){const auto caps=probe_tool_capabilities(current());const auto old=g_tool_capabilities;g_tool_capabilities=caps;report_->setPlainText(tool_report(caps));g_tool_capabilities=old;}
public:
    ToolSettingsDialog(QWidget* parent,const ToolConfiguration& initial):QDialog(parent),value_(initial){setWindowTitle("External tools and encoders");resize(820,720);auto* root=new QVBoxLayout(this);auto* note=new QLabel("Leave a path blank to search PATH automatically. BDMV Author checks FFmpeg encoder support and disables only unavailable codecs. FFmpeg is preferred for H.264/H.265 by default; standalone x264/x265 can be selected or used as fallback.",this);note->setWordWrap(true);root->addWidget(note);auto* form=new QFormLayout;root->addLayout(form);
        ffmpeg_=tool_row(this,form,"FFmpeg",initial.ffmpeg);ffprobe_=tool_row(this,form,"ffprobe",initial.ffprobe);x264_=tool_row(this,form,"x264",initial.x264);x265_=tool_row(this,form,"x265",initial.x265);tsmuxer_=tool_row(this,form,"tsMuxer",initial.tsmuxer);dvdauthor_=tool_row(this,form,"dvdauthor",initial.dvdauthor);spumux_=tool_row(this,form,"spumux",initial.spumux);mplex_=tool_row(this,form,"mplex",initial.mplex);mkisofs_=tool_row(this,form,"mkisofs",initial.mkisofs);
        h264_provider_=new QComboBox(this);h264_provider_->addItem("FFmpeg/libx264 (preferred)","ffmpeg");h264_provider_->addItem("Standalone x264 (preferred)","standalone");h264_provider_->setCurrentIndex(h264_provider_->findData(initial.h264_provider));form->addRow("H.264 provider",h264_provider_);
        hevc_provider_=new QComboBox(this);hevc_provider_->addItem("FFmpeg/libx265 (preferred)","ffmpeg");hevc_provider_->addItem("Standalone x265 (preferred)","standalone");hevc_provider_->setCurrentIndex(hevc_provider_->findData(initial.hevc_provider));form->addRow("H.265 provider",hevc_provider_);
        auto* scan=new QPushButton("Rescan with these settings",this);root->addWidget(scan);report_=new QPlainTextEdit(this);report_->setReadOnly(true);root->addWidget(report_,1);connect(scan,&QPushButton::clicked,this,[this]{rescan();});auto* buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,this);connect(buttons,&QDialogButtonBox::accepted,this,[this]{value_=current();accept();});connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject);root->addWidget(buttons);rescan();}
    ToolConfiguration value()const{return value_;}
};
class AdvancedCodecOptionsDialog final : public QDialog {
    DiscTarget target_;EncodingProfile value_;QPlainTextEdit *video_=nullptr,*audio_=nullptr;QComboBox *lossless_rate_=nullptr,*lossless_depth_=nullptr;
public:
    AdvancedCodecOptionsDialog(QWidget* parent,DiscTarget target,const EncodingProfile& initial):QDialog(parent),target_(target),value_(initial){
        setWindowTitle("Advanced codec options");resize(720,560);auto* root=new QVBoxLayout(this);
        auto* note=new QLabel("Enter one codec-private option per line as name=value (or name for a Boolean flag). Options that control disc-spec-critical codec/profile/level, rate/VBV, GOP/timing, pixel format/color signaling, sample rate, channel layout, or output format are intentionally rejected; use the normal BDMV Author controls for those.",this);note->setWordWrap(true);root->addWidget(note);
        root->addWidget(new QLabel(video_codec_display_name(value_.video_codec)+" options",this));video_=new QPlainTextEdit(QString::fromStdString(advanced_video_options_for_codec(value_)),this);video_->setPlaceholderText("aq-mode=2\npsy-rd=1.0\ntrellis=2");root->addWidget(video_,1);
        if(lossless_audio_codec(value_.audio_codec)){auto* format=new QFormLayout;lossless_rate_=new QComboBox(this);if(target_!=DiscTarget::DvdVideo480p)lossless_rate_->addItem("Auto — match source / round up",0);for(int rate:{48000,96000,192000})if(lpcm_sample_rate_allowed_for_target(target_,rate))lossless_rate_->addItem(QString("%1 kHz").arg(rate/1000),rate);int ri=lossless_rate_->findData(value_.lpcm_sample_rate_hz);lossless_rate_->setCurrentIndex(ri>=0?ri:0);lossless_depth_=new QComboBox(this);if(target_!=DiscTarget::DvdVideo480p)lossless_depth_->addItem("Auto — match source / round up",0);lossless_depth_->addItem("16-bit",16);lossless_depth_->addItem("24-bit",24);int di=lossless_depth_->findData(value_.lpcm_bit_depth);lossless_depth_->setCurrentIndex(di>=0?di:0);format->addRow("Lossless sample rate",lossless_rate_);format->addRow("Lossless bit depth",lossless_depth_);root->addLayout(format);}
        root->addWidget(new QLabel(audio_codec_display_name(value_.audio_codec)+" options",this));audio_=new QPlainTextEdit(QString::fromStdString(advanced_audio_options_for_codec(value_)),this);audio_->setPlaceholderText("dialnorm=31");root->addWidget(audio_,1);
        auto* buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,this);root->addWidget(buttons);connect(buttons,&QDialogButtonBox::accepted,this,[this]{auto candidate=value_;advanced_video_options_for_codec(candidate)=video_->toPlainText().toStdString();advanced_audio_options_for_codec(candidate)=audio_->toPlainText().toStdString();if(lossless_rate_)candidate.lpcm_sample_rate_hz=lossless_rate_->currentData().toInt();if(lossless_depth_)candidate.lpcm_bit_depth=lossless_depth_->currentData().toInt();try{validate_advanced_codec_options(candidate,target_);}catch(const std::exception& e){QMessageBox::warning(this,"Invalid advanced codec option",QString::fromStdString(e.what()));return;}value_=std::move(candidate);QDialog::accept();});connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject);
    }
    const EncodingProfile& value()const{return value_;}
};
bool edit_advanced_codec_options(QWidget* parent,DiscTarget target,EncodingProfile& profile){AdvancedCodecOptionsDialog d(parent,target,profile);if(d.exec()!=QDialog::Accepted)return false;profile=d.value();return true;}

QVariant audio_stream_settings_variant(const std::vector<AudioStreamSettings>& settings){
    QVariantList out;for(const auto& stream:settings){QVariantMap m;m["sourceOrdinal"]=stream.source_ordinal;m["language"]=QString::fromStdString(stream.language);m["overrideEncoding"]=stream.override_encoding;m["outputChannels"]=stream.output_channels;m["blurayEncoding"]=encoding_map(stream.encoding);m["uhdEncoding"]=encoding_map(stream.uhd_encoding);m["dvdEncoding"]=encoding_map(stream.dvd_encoding);out.push_back(m);}return out;
}
std::vector<AudioStreamSettings> audio_stream_settings_from_variant(const QVariant& value){
    std::vector<AudioStreamSettings> out;for(const auto& v:value.toList()){const auto m=v.toMap();AudioStreamSettings stream;stream.source_ordinal=m.value("sourceOrdinal").toInt();stream.language=m.value("language").toString().toStdString();stream.override_encoding=m.value("overrideEncoding").toBool();stream.output_channels=m.value("outputChannels").toInt();stream.encoding=encoding_from_map(m.value("blurayEncoding"),EncodingProfile{});stream.uhd_encoding=encoding_from_map(m.value("uhdEncoding"),default_uhd_encoding_profile());stream.dvd_encoding=encoding_from_map(m.value("dvdEncoding"),default_dvd_encoding_profile());out.push_back(std::move(stream));}return out;
}
QVariantMap subtitle_style_map(const SubtitleStyle& st){QVariantMap m;m["overrideStyle"]=st.override_style;m["fontFamily"]=QString::fromStdString(st.font_family);m["fontSizeUnits"]=st.font_size_px;m["fontColorR"]=static_cast<int>(st.font_color.r);m["fontColorG"]=static_cast<int>(st.font_color.g);m["fontColorB"]=static_cast<int>(st.font_color.b);m["fontColorA"]=static_cast<int>(st.font_color.a);m["bold"]=st.bold;m["italic"]=st.italic;m["underline"]=st.underline;m["strikeout"]=st.strikeout;m["lineSpacing"]=st.line_spacing;m["borderWidth"]=st.border_width;m["bottomOffsetUnits"]=st.bottom_offset_px;m["fadeInMs"]=st.fade_in_ms;m["fadeOutMs"]=st.fade_out_ms;m["dvdOutlineR"]=static_cast<int>(st.dvd_outline_color.r);m["dvdOutlineG"]=static_cast<int>(st.dvd_outline_color.g);m["dvdOutlineB"]=static_cast<int>(st.dvd_outline_color.b);m["dvdOutlineA"]=static_cast<int>(st.dvd_outline_color.a);m["dvdShadowR"]=static_cast<int>(st.dvd_shadow_color.r);m["dvdShadowG"]=static_cast<int>(st.dvd_shadow_color.g);m["dvdShadowB"]=static_cast<int>(st.dvd_shadow_color.b);m["dvdShadowA"]=static_cast<int>(st.dvd_shadow_color.a);m["dvdShadowOffsetX"]=st.dvd_shadow_offset_x;m["dvdShadowOffsetY"]=st.dvd_shadow_offset_y;m["dvdHorizontalAlignment"]=QString::fromStdString(st.dvd_horizontal_alignment);m["dvdVerticalAlignment"]=QString::fromStdString(st.dvd_vertical_alignment);m["dvdLeftMarginPx"]=st.dvd_left_margin_px;m["dvdRightMarginPx"]=st.dvd_right_margin_px;m["dvdTopMarginPx"]=st.dvd_top_margin_px;m["dvdBottomMarginPx"]=st.dvd_bottom_margin_px;m["dvdForceDisplay"]=st.dvd_force_display;return m;}
SubtitleStyle subtitle_style_from_map(const QVariant& v){const auto m=v.toMap();SubtitleStyle st;st.override_style=m.value("overrideStyle",false).toBool();st.font_family=m.value("fontFamily",QString()).toString().toStdString();st.font_size_px=m.contains("fontSizeUnits")?m.value("fontSizeUnits",80).toInt():m.value("fontSizePx",80).toInt();st.font_color={static_cast<std::uint8_t>(m.value("fontColorR",255).toInt()),static_cast<std::uint8_t>(m.value("fontColorG",255).toInt()),static_cast<std::uint8_t>(m.value("fontColorB",255).toInt()),static_cast<std::uint8_t>(m.value("fontColorA",255).toInt())};st.bold=m.value("bold",false).toBool();st.italic=m.value("italic",false).toBool();st.underline=m.value("underline",false).toBool();st.strikeout=m.value("strikeout",false).toBool();st.line_spacing=m.value("lineSpacing",1.0).toDouble();st.border_width=m.value("borderWidth",2.0).toDouble();st.bottom_offset_px=m.contains("bottomOffsetUnits")?m.value("bottomOffsetUnits",80).toInt():m.value("bottomOffsetPx",80).toInt();st.fade_in_ms=m.value("fadeInMs",0.0).toDouble();st.fade_out_ms=m.value("fadeOutMs",0.0).toDouble();st.dvd_outline_color={static_cast<std::uint8_t>(m.value("dvdOutlineR",0).toInt()),static_cast<std::uint8_t>(m.value("dvdOutlineG",0).toInt()),static_cast<std::uint8_t>(m.value("dvdOutlineB",0).toInt()),static_cast<std::uint8_t>(m.value("dvdOutlineA",255).toInt())};st.dvd_shadow_color={static_cast<std::uint8_t>(m.value("dvdShadowR",0).toInt()),static_cast<std::uint8_t>(m.value("dvdShadowG",0).toInt()),static_cast<std::uint8_t>(m.value("dvdShadowB",0).toInt()),static_cast<std::uint8_t>(m.value("dvdShadowA",255).toInt())};st.dvd_shadow_offset_x=m.value("dvdShadowOffsetX",0).toInt();st.dvd_shadow_offset_y=m.value("dvdShadowOffsetY",0).toInt();st.dvd_horizontal_alignment=m.value("dvdHorizontalAlignment","center").toString().toStdString();st.dvd_vertical_alignment=m.value("dvdVerticalAlignment","bottom").toString().toStdString();st.dvd_left_margin_px=m.value("dvdLeftMarginPx",40).toInt();st.dvd_right_margin_px=m.value("dvdRightMarginPx",40).toInt();st.dvd_top_margin_px=m.value("dvdTopMarginPx",20).toInt();st.dvd_bottom_margin_px=m.value("dvdBottomMarginPx",36).toInt();st.dvd_force_display=m.value("dvdForceDisplay",false).toBool();return st;}
QVariant subtitle_stream_settings_variant(const std::vector<SubtitleStreamSettings>& settings){QVariantList out;for(const auto& stream:settings){QVariantMap m;m["sourceOrdinal"]=stream.source_ordinal;m["language"]=QString::fromStdString(stream.language);m["style"]=subtitle_style_map(stream.style);out.push_back(m);}return out;}
std::vector<SubtitleStreamSettings> subtitle_stream_settings_from_variant(const QVariant& value){std::vector<SubtitleStreamSettings> out;for(const auto& v:value.toList()){const auto m=v.toMap();SubtitleStreamSettings stream;stream.source_ordinal=m.value("sourceOrdinal").toInt();stream.language=m.value("language").toString().toStdString();if(m.contains("style"))stream.style=subtitle_style_from_map(m.value("style"));out.push_back(std::move(stream));}return out;}
QVariant external_subtitles_variant(const std::vector<ExternalSubtitle>& subtitles){QVariantList out;for(const auto& sub:subtitles){QVariantMap m;m["source"]=QString::fromStdString(sub.source.string());m["language"]=QString::fromStdString(sub.language);m["style"]=subtitle_style_map(sub.style);out.push_back(m);}return out;}
std::vector<ExternalSubtitle> external_subtitles_from_variant(const QVariant& value){std::vector<ExternalSubtitle> out;for(const auto& v:value.toList()){const auto m=v.toMap();ExternalSubtitle sub;sub.source=m.value("source").toString().toStdString();sub.language=m.value("language","und").toString().toStdString();if(m.contains("style"))sub.style=subtitle_style_from_map(m.value("style"));if(!sub.source.empty())out.push_back(std::move(sub));}return out;}

struct GuiAudioTrack{int ordinal=0;QString codec;QString language;int channels=2;};
struct GuiSubtitleTrack{int ordinal=0;QString codec;QString language;};
bool gui_supported_subtitle_codec(const QString& codec){static const QStringList supported={"hdmv_pgs_subtitle","subrip","srt","ass","ssa","webvtt","text","mov_text"};return supported.contains(codec);}
bool probe_gui_title_streams(const QString& source,std::vector<GuiAudioTrack>& audio,std::vector<GuiSubtitleTrack>& subtitles,QString* error=nullptr){
    if(!g_tool_capabilities.ffprobe_ok){if(error)*error="ffprobe is unavailable.";return false;}QProcess p;start_tool_process(p,g_tool_capabilities.ffprobe,{"-v","error","-show_entries","stream=codec_type,codec_name,channels:stream_tags=language","-of","json",source});if(!p.waitForStarted(2000)||!p.waitForFinished(8000)||p.exitStatus()!=QProcess::NormalExit||p.exitCode()!=0){if(error)*error="ffprobe could not inspect the title streams.";return false;}QJsonParseError pe;const auto doc=QJsonDocument::fromJson(p.readAllStandardOutput(),&pe);if(pe.error!=QJsonParseError::NoError||!doc.isObject()){if(error)*error="ffprobe returned invalid stream metadata.";return false;}int ao=0,so=0;for(const auto& v:doc.object().value("streams").toArray()){const auto o=v.toObject();const auto type=o.value("codec_type").toString();const auto codec=o.value("codec_name").toString();const auto lang=o.value("tags").toObject().value("language").toString("und").toLower();if(type=="audio")audio.push_back({ao++,codec,lang,o.value("channels").toInt(2)});else if(type=="subtitle"){const int ordinal=so++;if(gui_supported_subtitle_codec(codec))subtitles.push_back({ordinal,codec,lang});}}return true;
}
std::optional<double> probe_gui_stream_duration(const QString& source,const QString& selector){
    if(source.isEmpty()||!g_tool_capabilities.ffprobe_ok)return std::nullopt;
    QProcess p;start_tool_process(p,g_tool_capabilities.ffprobe,{"-v","error","-select_streams",selector,"-show_entries","stream=duration:format=duration","-of","json",source});
    if(!p.waitForStarted(2000)||!p.waitForFinished(8000)||p.exitStatus()!=QProcess::NormalExit||p.exitCode()!=0)return std::nullopt;
    QJsonParseError pe;const auto doc=QJsonDocument::fromJson(p.readAllStandardOutput(),&pe);if(pe.error!=QJsonParseError::NoError||!doc.isObject())return std::nullopt;
    const auto streams=doc.object().value("streams").toArray();if(streams.isEmpty())return std::nullopt;
    const auto duration_value=streams.first().toObject().value("duration");bool ok=false;double seconds=duration_value.isDouble()?duration_value.toDouble():duration_value.toString().toDouble(&ok);if(duration_value.isDouble())ok=true;
    if(ok&&std::isfinite(seconds)&&seconds>0.0)return seconds;
    const auto format_value=doc.object().value("format").toObject().value("duration");ok=false;seconds=format_value.isDouble()?format_value.toDouble():format_value.toString().toDouble(&ok);if(format_value.isDouble())ok=true;
    if(ok&&std::isfinite(seconds)&&seconds>0.0)return seconds;return std::nullopt;
}

QColor qcolor(const Rgba& c){ return QColor(c.r,c.g,c.b,c.a); }
Rgba rgba(const QColor& c){ return {static_cast<std::uint8_t>(c.red()),static_cast<std::uint8_t>(c.green()),static_cast<std::uint8_t>(c.blue()),static_cast<std::uint8_t>(c.alpha())}; }
QString rgba_name(const QColor& c){
    return QString("#%1%2%3%4").arg(c.red(),2,16,QChar('0')).arg(c.green(),2,16,QChar('0')).arg(c.blue(),2,16,QChar('0')).arg(c.alpha(),2,16,QChar('0')).toLower();
}

class ColorButton final : public QPushButton {
    QColor color_;
    void update_view(){
        setText(rgba_name(color_));
        const int yiq=(color_.red()*299+color_.green()*587+color_.blue()*114)/1000;
        setStyleSheet(QString("QPushButton { background-color: rgba(%1,%2,%3,%4); color: %5; }")
            .arg(color_.red()).arg(color_.green()).arg(color_.blue()).arg(color_.alpha())
            .arg(yiq>=128?"black":"white"));
    }
public:
    explicit ColorButton(const Rgba& c,QWidget* parent=nullptr):QPushButton(parent),color_(qcolor(c)){
        update_view();
        connect(this,&QPushButton::clicked,this,[this]{
            QColor chosen=QColorDialog::getColor(color_,this,"Choose color",QColorDialog::ShowAlphaChannel);
            if(chosen.isValid()){color_=chosen;update_view();}
        });
    }
    Rgba value() const { return rgba(color_); }
    void setValue(const Rgba& c) { color_=qcolor(c); update_view(); }
};



class SubtitleStyleDialog final : public QDialog {
    DiscTarget target_;
    bool text_subtitle_;
    bool all_targets_;
    QCheckBox* override_=nullptr;
    QTabWidget* tabs_=nullptr;
    QWidget *common_page_=nullptr,*bluray_page_=nullptr,*dvd_page_=nullptr;
    QFontComboBox* font_=nullptr;
    QSpinBox* size_=nullptr;
    ColorButton* color_=nullptr;
    QCheckBox *bold_=nullptr,*italic_=nullptr,*underline_=nullptr,*strikeout_=nullptr;
    QDoubleSpinBox *line_spacing_=nullptr,*border_=nullptr,*fade_in_=nullptr,*fade_out_=nullptr;
    QSpinBox* bottom_offset_=nullptr;
    ColorButton *dvd_outline_=nullptr,*dvd_shadow_=nullptr;
    QSpinBox *shadow_x_=nullptr,*shadow_y_=nullptr,*left_=nullptr,*right_=nullptr,*top_=nullptr,*bottom_=nullptr;
    QComboBox *halign_=nullptr,*valign_=nullptr;
    QCheckBox* force_=nullptr;
    std::vector<QWidget*> common_text_only_;
    std::vector<QWidget*> bluray_text_only_;
    std::vector<QWidget*> dvd_only_;
    void refresh_enabled(){
        const bool on=override_->isChecked();
        for(auto* w:common_text_only_)w->setEnabled(on&&text_subtitle_);
        border_->setEnabled(on);
        const bool bluray=on&&(all_targets_||target_!=DiscTarget::DvdVideo480p);
        for(auto* w:bluray_text_only_)w->setEnabled(bluray&&text_subtitle_);
        bottom_offset_->setEnabled(bluray);
        const bool dvd=on&&text_subtitle_&&(all_targets_||target_==DiscTarget::DvdVideo480p);
        for(auto* w:dvd_only_)w->setEnabled(dvd);
    }
public:
    SubtitleStyleDialog(QWidget* parent,DiscTarget target,bool text_subtitle,const SubtitleStyle& initial,bool all_targets=false):QDialog(parent),target_(target),text_subtitle_(text_subtitle),all_targets_(all_targets){
        setWindowTitle(text_subtitle?"Text subtitle properties":"Bitmap subtitle properties");resize(640,660);auto* root=new QVBoxLayout(this);
        override_=new QCheckBox("Override backend subtitle rendering properties",this);override_->setChecked(initial.override_style);root->addWidget(override_);
        auto* note=new QLabel(text_subtitle?"Common properties are supported by both text renderers. Blu-ray/UHD contains tsMuxer-specific controls; DVD contains spumux-specific controls.":"PGS/SUP subtitles already contain rasterized glyphs, so font/color/size cannot be changed without OCR. Applicable Blu-ray/UHD bitmap positioning and border controls remain available.",this);note->setWordWrap(true);root->addWidget(note);
        tabs_=new QTabWidget(this);root->addWidget(tabs_,1);

        common_page_=new QWidget;auto* cf=new QFormLayout(common_page_);
        font_=new QFontComboBox(common_page_);font_->setCurrentFont(QFont(QString::fromStdString(initial.font_family)));
        size_=new QSpinBox(common_page_);size_->setRange(4,256);size_->setSuffix(" su");size_->setToolTip("Resolution-independent subtitle units: 1 su equals 1 pixel on a 1080-line subtitle plane; 720p/DVD scale automatically.");size_->setValue(initial.font_size_px);
        color_=new ColorButton(initial.font_color,common_page_);
        bold_=new QCheckBox(common_page_);bold_->setChecked(initial.bold);italic_=new QCheckBox(common_page_);italic_->setChecked(initial.italic);
        border_=new QDoubleSpinBox(common_page_);border_->setRange(0.0,32.0);border_->setDecimals(1);border_->setSuffix(" px");border_->setValue(initial.border_width);
        cf->addRow("Font family",font_);cf->addRow("Font size (subtitle units)",size_);cf->addRow("Font color",color_);cf->addRow("Bold",bold_);cf->addRow("Italic",italic_);cf->addRow("Outline / border width",border_);
        tabs_->addTab(common_page_,"Common");common_text_only_={font_,size_,color_,bold_,italic_};

        bluray_page_=new QWidget;auto* bf=new QFormLayout(bluray_page_);
        underline_=new QCheckBox(bluray_page_);underline_->setChecked(initial.underline);strikeout_=new QCheckBox(bluray_page_);strikeout_->setChecked(initial.strikeout);
        line_spacing_=new QDoubleSpinBox(bluray_page_);line_spacing_->setRange(0.1,10.0);line_spacing_->setDecimals(2);line_spacing_->setSingleStep(0.05);line_spacing_->setValue(initial.line_spacing);
        bottom_offset_=new QSpinBox(bluray_page_);bottom_offset_->setRange(0,2160);bottom_offset_->setSuffix(" su");bottom_offset_->setToolTip("Resolution-independent subtitle units; 80 su is 80 px at 1080p/UHD and scales for lower-resolution subtitle planes.");bottom_offset_->setValue(initial.bottom_offset_px);
        fade_in_=new QDoubleSpinBox(bluray_page_);fade_in_->setRange(0.0,10000.0);fade_in_->setDecimals(0);fade_in_->setSuffix(" ms");fade_in_->setValue(initial.fade_in_ms);
        fade_out_=new QDoubleSpinBox(bluray_page_);fade_out_->setRange(0.0,10000.0);fade_out_->setDecimals(0);fade_out_->setSuffix(" ms");fade_out_->setValue(initial.fade_out_ms);
        bf->addRow("Underline",underline_);bf->addRow("Strikeout",strikeout_);bf->addRow("Line spacing",line_spacing_);bf->addRow("Bottom offset (subtitle units)",bottom_offset_);bf->addRow("Fade in",fade_in_);bf->addRow("Fade out",fade_out_);
        tabs_->addTab(bluray_page_,"Blu-ray / UHD");bluray_text_only_={underline_,strikeout_,line_spacing_,fade_in_,fade_out_};

        dvd_page_=new QWidget;auto* df=new QFormLayout(dvd_page_);dvd_outline_=new ColorButton(initial.dvd_outline_color,dvd_page_);dvd_shadow_=new ColorButton(initial.dvd_shadow_color,dvd_page_);shadow_x_=new QSpinBox(dvd_page_);shadow_x_->setRange(-100,100);shadow_x_->setValue(initial.dvd_shadow_offset_x);shadow_y_=new QSpinBox(dvd_page_);shadow_y_->setRange(-100,100);shadow_y_->setValue(initial.dvd_shadow_offset_y);halign_=new QComboBox(dvd_page_);halign_->addItem("Use subtitle-file alignment","default");halign_->addItem("Left","left");halign_->addItem("Center","center");halign_->addItem("Right","right");halign_->setCurrentIndex(std::max(0,halign_->findData(QString::fromStdString(initial.dvd_horizontal_alignment))));valign_=new QComboBox(dvd_page_);valign_->addItem("Top","top");valign_->addItem("Center","center");valign_->addItem("Bottom","bottom");valign_->setCurrentIndex(std::max(0,valign_->findData(QString::fromStdString(initial.dvd_vertical_alignment))));
        auto margin=[&](int v){auto* x=new QSpinBox(dvd_page_);x->setRange(0,720);x->setSuffix(" px");x->setValue(v);return x;};left_=margin(initial.dvd_left_margin_px);right_=margin(initial.dvd_right_margin_px);top_=margin(initial.dvd_top_margin_px);bottom_=margin(initial.dvd_bottom_margin_px);force_=new QCheckBox("Force display even when subtitles are disabled",dvd_page_);force_->setChecked(initial.dvd_force_display);
        df->addRow("Outline color",dvd_outline_);df->addRow("Shadow color",dvd_shadow_);df->addRow("Shadow X offset",shadow_x_);df->addRow("Shadow Y offset",shadow_y_);df->addRow("Horizontal alignment",halign_);df->addRow("Vertical alignment",valign_);df->addRow("Left margin",left_);df->addRow("Right margin",right_);df->addRow("Top margin",top_);df->addRow("Bottom margin",bottom_);df->addRow(force_);tabs_->addTab(dvd_page_,"DVD");dvd_only_={dvd_outline_,dvd_shadow_,shadow_x_,shadow_y_,halign_,valign_,left_,right_,top_,bottom_,force_};

        if(!all_targets_){
            const int bluray_index=tabs_->indexOf(bluray_page_),dvd_index=tabs_->indexOf(dvd_page_);
            const bool dvd_target=target_==DiscTarget::DvdVideo480p;
            tabs_->setTabEnabled(bluray_index,!dvd_target);tabs_->setTabEnabled(dvd_index,dvd_target);
            if(dvd_target){bluray_page_->setToolTip("These properties are only used by Blu-ray/UHD tsMuxer output.");}
            else{dvd_page_->setToolTip("These properties are only used by DVD-Video/spumux output.");}
        }
        auto* buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,this);root->addWidget(buttons);connect(buttons,&QDialogButtonBox::accepted,this,&QDialog::accept);connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject);connect(override_,&QCheckBox::toggled,this,[this]{refresh_enabled();});refresh_enabled();
    }
    SubtitleStyle value()const{SubtitleStyle st;st.override_style=override_->isChecked();st.font_family=font_->currentFont().family().toStdString();st.font_size_px=size_->value();st.font_color=color_->value();st.bold=bold_->isChecked();st.italic=italic_->isChecked();st.underline=underline_->isChecked();st.strikeout=strikeout_->isChecked();st.line_spacing=line_spacing_->value();st.border_width=border_->value();st.bottom_offset_px=bottom_offset_->value();st.fade_in_ms=fade_in_->value();st.fade_out_ms=fade_out_->value();st.dvd_outline_color=dvd_outline_->value();st.dvd_shadow_color=dvd_shadow_->value();st.dvd_shadow_offset_x=shadow_x_->value();st.dvd_shadow_offset_y=shadow_y_->value();st.dvd_horizontal_alignment=halign_->currentData().toString().toStdString();st.dvd_vertical_alignment=valign_->currentData().toString().toStdString();st.dvd_left_margin_px=left_->value();st.dvd_right_margin_px=right_->value();st.dvd_top_margin_px=top_->value();st.dvd_bottom_margin_px=bottom_->value();st.dvd_force_display=force_->isChecked();return st;}
};

class AudioAdvancedOptionsOnlyDialog final:public QDialog{
    DiscTarget target_;EncodingProfile value_;QPlainTextEdit* edit_=nullptr;
public:AudioAdvancedOptionsOnlyDialog(QWidget* parent,DiscTarget target,const EncodingProfile& initial):QDialog(parent),target_(target),value_(initial){setWindowTitle("Audio stream advanced options");resize(650,420);auto* root=new QVBoxLayout(this);auto* note=new QLabel("Enter one codec-private option per line. Codec, bitrate, LPCM sample rate/bit depth, channel layout, and output-format parameters are controlled by the stream settings and cannot be overridden here.",this);note->setWordWrap(true);root->addWidget(note);root->addWidget(new QLabel(audio_codec_display_name(value_.audio_codec)+" options",this));edit_=new QPlainTextEdit(QString::fromStdString(advanced_audio_options_for_codec(value_)),this);root->addWidget(edit_,1);auto* buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,this);root->addWidget(buttons);connect(buttons,&QDialogButtonBox::accepted,this,[this]{auto candidate=value_;advanced_audio_options_for_codec(candidate)=edit_->toPlainText().toStdString();try{validate_advanced_audio_options(candidate,target_);}catch(const std::exception&e){QMessageBox::warning(this,"Invalid advanced audio option",QString::fromStdString(e.what()));return;}value_=std::move(candidate);accept();});connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject);}const EncodingProfile& value()const{return value_;}
};

class TitleStreamsDialog final : public QDialog {
    struct AudioRow {
        GuiAudioTrack source;
        AudioStreamSettings value;
        QLineEdit* language=nullptr;
        QCheckBox* custom=nullptr;
        QComboBox* codec=nullptr;
        QComboBox* channels=nullptr;
        QSpinBox* bitrate=nullptr;
        QComboBox* rate=nullptr;
        QComboBox* depth=nullptr;
        QPushButton* advanced=nullptr;
    };
    struct SubtitleRow { GuiSubtitleTrack source; SubtitleStreamSettings value; QLineEdit* language=nullptr; QPushButton* style=nullptr; };
    DiscTarget target_;
    int video_bitrate_kbps_;
    bool allow_exceeding_format_limits_=false;
    EncodingProfile hd_default_,uhd_default_,dvd_default_;
    SubtitleStyle default_subtitle_style_;
    std::vector<AudioRow> audio_;
    std::vector<SubtitleRow> subtitles_;
    QTableWidget* external_table_=nullptr;
    QComboBox* default_audio_=nullptr;
    QComboBox* default_subtitle_=nullptr;
    QLabel* total_=nullptr;
    QDialogButtonBox* buttons_=nullptr;

    const EncodingProfile& fallback() const {
        switch(target_){case DiscTarget::UltraHdBluRay2160:return uhd_default_;case DiscTarget::DvdVideo480p:return dvd_default_;case DiscTarget::BluRay1080:return hd_default_;}
        return hd_default_;
    }
    EncodingProfile profile_from_row(const AudioRow& row) const {
        EncodingProfile e=row.custom->isChecked()?encoding_for_target(row.value,target_):fallback();
        if(row.custom->isChecked()){
            e.audio_codec=static_cast<AudioCodec>(row.codec->currentData().toInt());
            if(lossless_audio_codec(e.audio_codec)){e.lpcm_sample_rate_hz=row.rate->currentData().toInt();e.lpcm_bit_depth=row.depth->currentData().toInt();}
            else if(audio_codec_has_configurable_bitrate(e.audio_codec))set_audio_bitrate_kbps(e,row.bitrate->value());
        }
        return e;
    }
    int max_channels(const EncodingProfile& e) const {
        return lossless_audio_codec(e.audio_codec)?lpcm_max_channels_for_target(target_,e.lpcm_sample_rate_hz>0?e.lpcm_sample_rate_hz:48000):6;
    }
    int output_channels(const AudioRow& row,const EncodingProfile& e) const {
        const int requested=row.channels->currentData().toInt();
        return requested>0?requested:std::min(std::max(1,row.source.channels),max_channels(e));
    }
    bool row_peak_measured_during_authoring(const AudioRow& row) const {
        return profile_from_row(row).audio_codec==AudioCodec::TrueHdAc3;
    }
    int row_fixed_bitrate(const AudioRow& row) const {
        const auto e=profile_from_row(row);const int channels=output_channels(row,e);
        if(e.audio_codec==AudioCodec::Lpcm)return lpcm_bitrate_kbps(e.lpcm_sample_rate_hz,e.lpcm_bit_depth,channels);
        if(e.audio_codec==AudioCodec::Dca)return e.dca_bitrate_kbps;
        if(e.audio_codec==AudioCodec::TrueHdAc3)return 0;
        return e.ac3_bitrate_kbps;
    }
    void refresh_row(AudioRow& row){
        const bool custom=row.custom->isChecked();row.codec->setEnabled(custom);const auto e=profile_from_row(row);const bool lossless=lossless_audio_codec(e.audio_codec);
        row.rate->setEnabled(custom&&lossless);row.depth->setEnabled(custom&&lossless);row.advanced->setEnabled(custom);
        const int maximum=max_channels(e);
        for(int i=0;i<row.channels->count();++i){const int ch=row.channels->itemData(i).toInt();set_combo_item_enabled(row.channels,i,ch==0||(ch<=row.source.channels&&ch<=maximum),ch>row.source.channels?"Upmixing is not supported.":"The selected codec/target does not support this many channels.");}
        if(e.audio_codec==AudioCodec::Lpcm){const QSignalBlocker blocker(row.bitrate);row.bitrate->setEnabled(true);row.bitrate->setReadOnly(true);row.bitrate->setButtonSymbols(QAbstractSpinBox::NoButtons);row.bitrate->setRange(0,40000);row.bitrate->setSuffix(" kb/s");row.bitrate->setValue(row_fixed_bitrate(row));}
        else{row.bitrate->setReadOnly(false);row.bitrate->setButtonSymbols(QAbstractSpinBox::UpDownArrows);configure_audio_bitrate_spin(row.bitrate,target_,e.audio_codec,audio_bitrate_kbps(e),allow_exceeding_format_limits_);row.bitrate->setEnabled(custom&&audio_codec_has_configurable_bitrate(e.audio_codec));}
        if(row.depth->findData(24)>=0){const bool depth24ok=target_==DiscTarget::DvdVideo480p?g_tool_capabilities.ffmpeg_pcm_s24be:g_tool_capabilities.ffmpeg_pcm_s24le;set_combo_item_enabled(row.depth,row.depth->findData(24),depth24ok,"The selected FFmpeg lacks a 24-bit PCM encoder.");}
    }
    void refresh_total(){
        int fixed_audio=0;int measured_tracks=0;for(auto& row:audio_){refresh_row(row);if(row_peak_measured_during_authoring(row))++measured_tracks;else fixed_audio+=row_fixed_bitrate(row);}
        const int fixed_combined=video_bitrate_kbps_+fixed_audio;const int maximum=target_max_combined_av_bitrate_kbps(target_);
        if(measured_tracks>0){
            total_->setText(QString("Configured fixed-rate ceiling: video %1 + fixed audio %2 = %3 kb/s (target maximum %4 kb/s). %5 TrueHD track(s): peak bitrate is measured during authoring before video encoding, and the video VBV/maxrate is reduced to fit the remaining transport budget.").arg(video_bitrate_kbps_).arg(fixed_audio).arg(fixed_combined).arg(maximum).arg(measured_tracks));
        }else{
            total_->setText(QString("Configured encode ceiling: video %1 + audio %2 = %3 kb/s (target maximum %4 kb/s)").arg(video_bitrate_kbps_).arg(fixed_audio).arg(fixed_combined).arg(maximum));
        }
        total_->setWordWrap(true);
        const bool over=!allow_exceeding_format_limits_&&fixed_combined>maximum;total_->setStyleSheet(over?"color: #c62828; font-weight: bold;":"");if(buttons_)buttons_->button(QDialogButtonBox::Ok)->setEnabled(!over);
    }
    static bool valid_language(const QString& s){if(s.trimmed().isEmpty())return true;const auto t=s.trimmed();if(t.size()!=3)return false;for(const auto ch:t)if(!ch.isLetter())return false;return true;}
    static bool external_is_text(const QString& path){const auto ext=QFileInfo(path).suffix().toLower();return ext!="sup"&&ext!="pgs";}
    SubtitleStyle style_for_edit(const SubtitleStyle& current) const {if(current.override_style)return current;auto inherited=default_subtitle_style_;inherited.override_style=false;return inherited;}
    void refresh_default_subtitle_choices(int preferred=std::numeric_limits<int>::min()){
        if(!default_subtitle_)return;
        if(preferred==std::numeric_limits<int>::min())preferred=default_subtitle_->currentData().toInt();
        const QSignalBlocker blocker(default_subtitle_);default_subtitle_->clear();
        default_subtitle_->addItem("Player default",-1);default_subtitle_->addItem("Subtitles off",0);
        int authored=1;for(const auto& row:subtitles_){const auto lang=row.language?row.language->text().trimmed():QString{};const auto shown=lang.isEmpty()?row.source.language:lang;default_subtitle_->addItem(QString("Subtitle %1 — %2 (%3)").arg(authored).arg(shown.isEmpty()?QStringLiteral("und"):shown).arg(row.source.codec),authored);++authored;}
        if(external_table_)for(int r=0;r<external_table_->rowCount();++r){const auto* path=external_table_->item(r,0);const auto* lang=external_table_->item(r,1);default_subtitle_->addItem(QString("Subtitle %1 — %2 (attached: %3)").arg(authored).arg(lang?lang->text():QStringLiteral("und")).arg(path?QFileInfo(path->text()).fileName():QStringLiteral("file")),authored);++authored;}
        int index=default_subtitle_->findData(preferred);if(index<0)index=default_subtitle_->findData(-1);default_subtitle_->setCurrentIndex(std::max(0,index));
    }
    void add_external_row(const QString& path,const QString& language,const SubtitleStyle& style={}){
        const int row=external_table_->rowCount();external_table_->insertRow(row);auto* path_item=new QTableWidgetItem(path);path_item->setFlags(path_item->flags()&~Qt::ItemIsEditable);path_item->setData(Qt::UserRole+17,subtitle_style_map(style));external_table_->setItem(row,0,path_item);external_table_->setItem(row,1,new QTableWidgetItem(language.isEmpty()?QStringLiteral("und"):language.toLower()));auto* button=new QPushButton(style.override_style?"Properties…":"Defaults…",external_table_);external_table_->setCellWidget(row,2,button);connect(button,&QPushButton::clicked,this,[this,button]{int r=-1;for(int i=0;i<external_table_->rowCount();++i)if(external_table_->cellWidget(i,2)==button){r=i;break;}if(r<0)return;auto* item=external_table_->item(r,0);if(!item)return;const auto current=subtitle_style_from_map(item->data(Qt::UserRole+17));SubtitleStyleDialog d(this,target_,external_is_text(item->text()),style_for_edit(current));if(d.exec()!=QDialog::Accepted)return;const auto updated=d.value();item->setData(Qt::UserRole+17,subtitle_style_map(updated));button->setText(updated.override_style?"Properties…":"Defaults…");});
    }
public:
    TitleStreamsDialog(QWidget* parent,const QString& source,DiscTarget target,int video_bitrate,const EncodingProfile& hd,const EncodingProfile& uhd,const EncodingProfile& dvd,const SubtitleStyle& default_subtitle_style,const std::vector<AudioStreamSettings>& current_audio,const std::vector<SubtitleStreamSettings>& current_subtitle,const std::vector<ExternalSubtitle>& current_external,int current_default_audio=0,int current_default_subtitle=-1,bool allow_exceeding_format_limits=false)
        :QDialog(parent),target_(target),video_bitrate_kbps_(video_bitrate),allow_exceeding_format_limits_(allow_exceeding_format_limits),hd_default_(hd),uhd_default_(uhd),dvd_default_(dvd),default_subtitle_style_(default_subtitle_style){
        setWindowTitle("Title audio and subtitle streams");resize(1260,700);auto* root=new QVBoxLayout(this);
        auto* note=new QLabel("Language overrides preserve the source tag when blank. Custom encoding controls codec/bitrate and LPCM/TrueHD lossless format settings. Channel layout can be reduced independently; selecting fewer channels forces an encode with the title fallback codec unless Custom encoding is also enabled. Upmixing is rejected.",this);note->setWordWrap(true);root->addWidget(note);
        std::vector<GuiAudioTrack> audio_sources;std::vector<GuiSubtitleTrack> subtitle_sources;QString error;if(!probe_gui_title_streams(source,audio_sources,subtitle_sources,&error))root->addWidget(new QLabel(error,this));
        auto* defaults_box=new QGroupBox("Playback defaults",this);auto* defaults_form=new QFormLayout(defaults_box);default_audio_=new QComboBox(defaults_box);default_audio_->addItem("Player default",0);for(std::size_t i=0;i<audio_sources.size();++i){const auto& src=audio_sources[i];default_audio_->addItem(QString("Audio %1 — %2 (%3, %4 ch)").arg(i+1).arg(src.language.isEmpty()?QStringLiteral("und"):src.language).arg(src.codec).arg(src.channels),static_cast<int>(i+1));}int dai=default_audio_->findData(current_default_audio);default_audio_->setCurrentIndex(dai>=0?dai:0);default_subtitle_=new QComboBox(defaults_box);defaults_form->addRow("Default audio",default_audio_);defaults_form->addRow("Default subtitles",default_subtitle_);auto* defaults_note=new QLabel("Player default preserves the player's normal language selection. Subtitles off explicitly starts this title with subtitle display disabled. Explicit choices use the authored one-based stream order.",defaults_box);defaults_note->setWordWrap(true);defaults_form->addRow(defaults_note);root->addWidget(defaults_box);
        auto* tabs=new QTabWidget(this);root->addWidget(tabs,1);
        auto* audio_page=new QWidget;auto* ag=new QGridLayout(audio_page);audio_.reserve(audio_sources.size());const QStringList ah={"#","Source","Ch","Language override","Custom encoding","Codec","Output channels","Bitrate","Lossless rate","Lossless depth","Advanced"};for(qsizetype c=0;c<ah.size();++c)ag->addWidget(new QLabel(ah[c],audio_page),0,static_cast<int>(c));
        int ar=1;for(const auto& src:audio_sources){
            AudioStreamSettings value;value.source_ordinal=src.ordinal;value.encoding=hd;value.uhd_encoding=uhd;value.dvd_encoding=dvd;if(const auto it=std::find_if(current_audio.begin(),current_audio.end(),[&](const AudioStreamSettings& x){return x.source_ordinal==src.ordinal;});it!=current_audio.end())value=*it;
            AudioRow row;row.source=src;row.value=value;ag->addWidget(new QLabel(QString::number(src.ordinal+1),audio_page),ar,0);ag->addWidget(new QLabel(src.codec,audio_page),ar,1);ag->addWidget(new QLabel(QString::number(src.channels),audio_page),ar,2);
            row.language=new QLineEdit(QString::fromStdString(value.language),audio_page);row.language->setPlaceholderText(src.language+" (source)");ag->addWidget(row.language,ar,3);
            row.custom=new QCheckBox(audio_page);row.custom->setChecked(value.override_encoding);ag->addWidget(row.custom,ar,4);const auto active=value.override_encoding?encoding_for_target(value,target):fallback();row.codec=audio_combo(audio_page,target,active.audio_codec);ag->addWidget(row.codec,ar,5);
            row.channels=new QComboBox(audio_page);row.channels->addItem(QString("Source (%1 ch)").arg(src.channels),0);if(src.channels>=8)row.channels->addItem("7.1 (8 ch)",8);if(src.channels>=6)row.channels->addItem("5.1 (6 ch)",6);if(src.channels>=4)row.channels->addItem("Quad (4 ch)",4);if(src.channels>=2)row.channels->addItem("Stereo (2 ch)",2);row.channels->addItem("Mono (1 ch)",1);row.channels->setCurrentIndex(std::max(0,row.channels->findData(value.output_channels)));ag->addWidget(row.channels,ar,6);
            row.bitrate=audio_bitrate_spin(audio_page);ag->addWidget(row.bitrate,ar,7);row.rate=new QComboBox(audio_page);if(target!=DiscTarget::DvdVideo480p)row.rate->addItem("Auto",0);for(int rate:{48000,96000,192000})if(lpcm_sample_rate_allowed_for_target(target,rate))row.rate->addItem(QString("%1 kHz").arg(rate/1000),rate);row.rate->setCurrentIndex(std::max(0,row.rate->findData(active.lpcm_sample_rate_hz)));ag->addWidget(row.rate,ar,8);row.depth=new QComboBox(audio_page);if(target!=DiscTarget::DvdVideo480p)row.depth->addItem("Auto",0);row.depth->addItem("16-bit",16);row.depth->addItem("24-bit",24);row.depth->setCurrentIndex(std::max(0,row.depth->findData(active.lpcm_bit_depth)));ag->addWidget(row.depth,ar,9);row.advanced=new QPushButton("Advanced…",audio_page);ag->addWidget(row.advanced,ar,10);
            audio_.push_back(row);auto* rp=&audio_.back();connect(rp->custom,&QCheckBox::toggled,this,[this]{refresh_total();});connect(rp->codec,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this]{refresh_total();});connect(rp->channels,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this]{refresh_total();});connect(rp->bitrate,QOverload<int>::of(&QSpinBox::valueChanged),this,[this]{refresh_total();});connect(rp->rate,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this]{refresh_total();});connect(rp->depth,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this]{refresh_total();});connect(rp->advanced,&QPushButton::clicked,this,[this,rp]{auto e=profile_from_row(*rp);AudioAdvancedOptionsOnlyDialog d(this,target_,e);if(d.exec()!=QDialog::Accepted)return;auto& target_profile=encoding_for_target(rp->value,target_);copy_audio_encoding_settings(target_profile,d.value());refresh_total();});++ar;
        }ag->setColumnStretch(3,1);tabs->addTab(audio_page,QString("Audio (%1)").arg(audio_sources.size()));

        auto* sub_page=new QWidget;auto* sub_root=new QVBoxLayout(sub_page);auto* embedded=new QWidget(sub_page);auto* sg=new QGridLayout(embedded);subtitles_.reserve(subtitle_sources.size());sg->addWidget(new QLabel("#",embedded),0,0);sg->addWidget(new QLabel("Source codec",embedded),0,1);sg->addWidget(new QLabel("Source language",embedded),0,2);sg->addWidget(new QLabel("Language override (ISO 639-2, blank = source)",embedded),0,3);sg->addWidget(new QLabel("Properties",embedded),0,4);int sr=1;
        for(const auto& src:subtitle_sources){SubtitleStreamSettings value;value.source_ordinal=src.ordinal;if(const auto it=std::find_if(current_subtitle.begin(),current_subtitle.end(),[&](const SubtitleStreamSettings& x){return x.source_ordinal==src.ordinal;});it!=current_subtitle.end())value=*it;SubtitleRow row;row.source=src;row.value=value;sg->addWidget(new QLabel(QString::number(src.ordinal+1),embedded),sr,0);sg->addWidget(new QLabel(src.codec,embedded),sr,1);sg->addWidget(new QLabel(src.language,embedded),sr,2);row.language=new QLineEdit(QString::fromStdString(value.language),embedded);row.language->setPlaceholderText(src.language+" (source)");sg->addWidget(row.language,sr,3);row.style=new QPushButton(value.style.override_style?"Properties…":"Defaults…",embedded);sg->addWidget(row.style,sr,4);subtitles_.push_back(row);auto* rp=&subtitles_.back();connect(rp->style,&QPushButton::clicked,this,[this,rp]{SubtitleStyleDialog d(this,target_,rp->source.codec!="hdmv_pgs_subtitle",style_for_edit(rp->value.style));if(d.exec()!=QDialog::Accepted)return;rp->value.style=d.value();rp->style->setText(rp->value.style.override_style?"Properties…":"Defaults…");});++sr;}sg->setColumnStretch(3,1);sub_root->addWidget(embedded);
        auto* attached_label=new QLabel("Attached subtitle files",sub_page);attached_label->setStyleSheet("font-weight: bold;");sub_root->addWidget(attached_label);external_table_=new QTableWidget(0,3,sub_page);external_table_->setHorizontalHeaderLabels({"File","Language (ISO 639-2)","Properties"});external_table_->horizontalHeader()->setSectionResizeMode(0,QHeaderView::Stretch);external_table_->horizontalHeader()->setSectionResizeMode(1,QHeaderView::ResizeToContents);external_table_->setSelectionBehavior(QAbstractItemView::SelectRows);sub_root->addWidget(external_table_,1);for(const auto& sub:current_external)add_external_row(QString::fromStdString(sub.source.string()),QString::fromStdString(sub.language),sub.style);refresh_default_subtitle_choices(current_default_subtitle);auto* sub_buttons=new QHBoxLayout;auto* attach=new QPushButton("Attach subtitle file…",sub_page);auto* remove=new QPushButton("Remove selected",sub_page);sub_buttons->addWidget(attach);sub_buttons->addWidget(remove);sub_buttons->addStretch(1);sub_root->addLayout(sub_buttons);connect(attach,&QPushButton::clicked,this,[this]{const int previous=default_subtitle_->currentData().toInt();const auto files=QFileDialog::getOpenFileNames(this,"Attach subtitle files",{},"Subtitle files (*.srt *.ass *.ssa *.vtt *.sup *.pgs);;All files (*)");for(const auto& f:files)if(!f.isEmpty())add_external_row(f,"und",{});refresh_default_subtitle_choices(previous);});connect(remove,&QPushButton::clicked,this,[this]{const int previous=default_subtitle_->currentData().toInt();QSet<int> rows;for(const auto* item:external_table_->selectedItems())rows.insert(item->row());QList<int> ordered=rows.values();std::sort(ordered.begin(),ordered.end(),std::greater<int>());for(int r:ordered)external_table_->removeRow(r);refresh_default_subtitle_choices(previous);});tabs->addTab(sub_page,QString("Subtitles (%1 + %2 attached)").arg(subtitle_sources.size()).arg(current_external.size()));

        total_=new QLabel(this);root->addWidget(total_);buttons_=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,this);root->addWidget(buttons_);connect(buttons_,&QDialogButtonBox::rejected,this,&QDialog::reject);connect(buttons_,&QDialogButtonBox::accepted,this,[this]{
            for(auto& row:audio_){if(!valid_language(row.language->text())){QMessageBox::warning(this,"Invalid language","Audio language overrides must be blank or exactly three letters (ISO 639-2).");return;}row.value.language=row.language->text().trimmed().toLower().toStdString();row.value.override_encoding=row.custom->isChecked();row.value.output_channels=row.channels->currentData().toInt();const auto e=profile_from_row(row);const int maximum=max_channels(e);if(row.value.output_channels>row.source.channels){QMessageBox::warning(this,"Invalid downmix","An audio stream cannot be upmixed to more channels than its source.");return;}if(row.value.output_channels>maximum){QMessageBox::warning(this,"Invalid channel layout","The selected codec/target cannot carry the requested channel count.");return;}if(row.value.override_encoding){copy_audio_encoding_settings(encoding_for_target(row.value,target_),e);try{AuthorEngine::validate_encoding_profile_for_target(encoding_for_target(row.value,target_),target_,"audio stream",allow_exceeding_format_limits_);}catch(const std::exception& ex){QMessageBox::warning(this,"Invalid audio stream settings",QString::fromStdString(ex.what()));return;}}}
            for(auto& row:subtitles_){if(!valid_language(row.language->text())){QMessageBox::warning(this,"Invalid language","Subtitle language overrides must be blank or exactly three letters (ISO 639-2).");return;}row.value.language=row.language->text().trimmed().toLower().toStdString();}
            for(int r=0;r<external_table_->rowCount();++r){const auto* path=external_table_->item(r,0);const auto* lang=external_table_->item(r,1);if(!path||path->text().trimmed().isEmpty()){QMessageBox::warning(this,"Invalid subtitle","Attached subtitle path cannot be empty.");return;}if(!lang||!valid_language(lang->text())||lang->text().trimmed().isEmpty()){QMessageBox::warning(this,"Invalid language","Attached subtitle language must be exactly three letters (ISO 639-2).");return;}}
            int fixed_sum=video_bitrate_kbps_;for(const auto& row:audio_)if(!row_peak_measured_during_authoring(row))fixed_sum+=row_fixed_bitrate(row);if(!allow_exceeding_format_limits_&&fixed_sum>target_max_combined_av_bitrate_kbps(target_)){QMessageBox::warning(this,"Bitrate limit exceeded",QString("Configured video+fixed-rate audio bitrate %1 kb/s exceeds the target maximum of %2 kb/s. TrueHD peak bitrate, when present, is measured during authoring before video encoding.").arg(fixed_sum).arg(target_max_combined_av_bitrate_kbps(target_)));return;}accept();});refresh_total();
    }
    std::vector<AudioStreamSettings> audio_settings() const {std::vector<AudioStreamSettings> out;for(const auto& row:audio_)if(row.value.override_encoding||row.value.output_channels>0||!row.value.language.empty())out.push_back(row.value);return out;}
    std::vector<SubtitleStreamSettings> subtitle_settings() const {std::vector<SubtitleStreamSettings> out;for(const auto& row:subtitles_)if(!row.value.language.empty()||row.value.style.override_style)out.push_back(row.value);return out;}
    std::vector<ExternalSubtitle> external_subtitles() const {std::vector<ExternalSubtitle> out;for(int r=0;r<external_table_->rowCount();++r){ExternalSubtitle sub;sub.source=external_table_->item(r,0)->text().toStdString();sub.language=external_table_->item(r,1)->text().trimmed().toLower().toStdString();sub.style=subtitle_style_from_map(external_table_->item(r,0)->data(Qt::UserRole+17));out.push_back(std::move(sub));}return out;}
    int default_audio_stream() const{return default_audio_?default_audio_->currentData().toInt():0;}
    int default_subtitle_stream() const{return default_subtitle_?default_subtitle_->currentData().toInt():-1;}
};

struct StateWidgets { ColorButton* text=nullptr; ColorButton* background=nullptr; ColorButton* border=nullptr; QSpinBox* border_width=nullptr; };

class ButtonStyleDialog final : public QDialog {
    QFontComboBox* font_=nullptr;
    QSpinBox* size_=nullptr;
    QCheckBox* bold_=nullptr;
    QCheckBox* italic_=nullptr;
    QSpinBox* radius_=nullptr;
    StateWidgets states_[4];
public:
    ButtonStyleDialog(QWidget* parent,const ButtonStyle& initial):QDialog(parent){
        setWindowTitle("Text button style"); resize(520,480);
        auto* root=new QVBoxLayout(this); auto* form=new QFormLayout;
        font_=new QFontComboBox; font_->setCurrentFont(QFont(QString::fromStdString(initial.font_family)));
        size_=new QSpinBox; size_->setRange(6,256); size_->setSuffix(" px"); size_->setValue(initial.font_size_px);
        bold_=new QCheckBox; bold_->setChecked(initial.bold); italic_=new QCheckBox; italic_->setChecked(initial.italic);
        radius_=new QSpinBox; radius_->setRange(0,256); radius_->setSuffix(" px"); radius_->setValue(initial.corner_radius);
        form->addRow("Font family",font_); form->addRow("Font size",size_); form->addRow("Bold",bold_); form->addRow("Italic",italic_); form->addRow("Corner radius",radius_); root->addLayout(form);
        auto* tabs=new QTabWidget; root->addWidget(tabs,1);
        const ButtonStateStyle* src[4]={&initial.normal,&initial.active,&initial.selected,&initial.activated};
        const char* names[4]={"Normal","Active option","Selected","Activated"};
        for(int i=0;i<4;++i){
            auto* page=new QWidget; auto* f=new QFormLayout(page);
            states_[i].text=new ColorButton(src[i]->text_color,page); states_[i].background=new ColorButton(src[i]->background_color,page); states_[i].border=new ColorButton(src[i]->border_color,page);
            states_[i].border_width=new QSpinBox(page); states_[i].border_width->setRange(0,128); states_[i].border_width->setSuffix(" px"); states_[i].border_width->setValue(src[i]->border_width);
            f->addRow("Text color",states_[i].text); if(i!=1)f->addRow("Background",states_[i].background); else { states_[i].background->hide(); auto* note=new QLabel("Active-option styling is a transparent persistent overlay so keyboard/mouse selection remains independently visible.",page); note->setWordWrap(true); f->addRow(note); } f->addRow("Border color",states_[i].border); f->addRow("Border width",states_[i].border_width); tabs->addTab(page,names[i]);
        }
        auto* buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel); root->addWidget(buttons);
        connect(buttons,&QDialogButtonBox::accepted,this,&QDialog::accept); connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject);
    }
    ButtonStyle value() const {
        ButtonStyle s; s.font_family=font_->currentFont().family().toStdString(); s.font_size_px=size_->value(); s.bold=bold_->isChecked(); s.italic=italic_->isChecked(); s.corner_radius=radius_->value();
        ButtonStateStyle* dst[4]={&s.normal,&s.active,&s.selected,&s.activated};
        for(int i=0;i<4;++i){dst[i]->text_color=states_[i].text->value();dst[i]->background_color=states_[i].background->value();dst[i]->border_color=states_[i].border->value();dst[i]->border_width=states_[i].border_width->value();}
        return s;
    }
};


EncodingProfile load_encoding_defaults(QSettings& settings,const QString& prefix,const EncodingProfile& fallback) {
    EncodingProfile e=fallback;
    e.video_codec=static_cast<VideoCodec>(settings.value(prefix+"/videoCodec",static_cast<int>(e.video_codec)).toInt());
    e.video_bitrate_kbps=settings.value(prefix+"/videoBitrateKbps",e.video_bitrate_kbps).toInt();
    e.video_min_bitrate_kbps=settings.value(prefix+"/videoMinrateKbps",e.video_min_bitrate_kbps).toInt();
    e.video_max_bitrate_kbps=settings.value(prefix+"/videoMaxrateKbps",e.video_max_bitrate_kbps).toInt();
    e.x264_preset=settings.value(prefix+"/x264Preset",QString::fromStdString(e.x264_preset)).toString().toStdString();
    e.x265_preset=settings.value(prefix+"/x265Preset",QString::fromStdString(e.x265_preset)).toString().toStdString();
    e.two_pass=settings.value(prefix+"/twoPass",e.two_pass).toBool();
    e.keyframe_interval_frames=settings.value(prefix+"/keyframeIntervalFrames",e.keyframe_interval_frames).toInt();
    e.audio_codec=static_cast<AudioCodec>(settings.value(prefix+"/audioCodec",static_cast<int>(e.audio_codec)).toInt());
    e.ac3_bitrate_kbps=settings.value(prefix+"/ac3BitrateKbps",e.ac3_bitrate_kbps).toInt();
    e.dca_bitrate_kbps=settings.value(prefix+"/dcaBitrateKbps",e.dca_bitrate_kbps).toInt();
    e.lpcm_sample_rate_hz=settings.value(prefix+"/lpcmSampleRateHz",e.lpcm_sample_rate_hz).toInt();
    e.lpcm_bit_depth=settings.value(prefix+"/lpcmBitDepth",e.lpcm_bit_depth).toInt();
    e.x264_advanced_options=settings.value(prefix+"/x264AdvancedOptions",QString::fromStdString(e.x264_advanced_options)).toString().toStdString();
    e.x265_advanced_options=settings.value(prefix+"/x265AdvancedOptions",QString::fromStdString(e.x265_advanced_options)).toString().toStdString();
    e.mpeg2_advanced_options=settings.value(prefix+"/mpeg2AdvancedOptions",QString::fromStdString(e.mpeg2_advanced_options)).toString().toStdString();
    e.ac3_advanced_options=settings.value(prefix+"/ac3AdvancedOptions",QString::fromStdString(e.ac3_advanced_options)).toString().toStdString();
    e.dca_advanced_options=settings.value(prefix+"/dcaAdvancedOptions",QString::fromStdString(e.dca_advanced_options)).toString().toStdString();
    e.lpcm_advanced_options=settings.value(prefix+"/lpcmAdvancedOptions",QString::fromStdString(e.lpcm_advanced_options)).toString().toStdString();
    e.truehd_advanced_options=settings.value(prefix+"/truehdAdvancedOptions",QString::fromStdString(e.truehd_advanced_options)).toString().toStdString();
    return e;
}
void save_encoding_defaults(QSettings& settings,const QString& prefix,const EncodingProfile& e) {
    settings.setValue(prefix+"/videoCodec",static_cast<int>(e.video_codec));
    settings.setValue(prefix+"/videoBitrateKbps",e.video_bitrate_kbps);
    settings.setValue(prefix+"/videoMinrateKbps",e.video_min_bitrate_kbps);
    settings.setValue(prefix+"/videoMaxrateKbps",e.video_max_bitrate_kbps);
    settings.setValue(prefix+"/x264Preset",QString::fromStdString(e.x264_preset));
    settings.setValue(prefix+"/x265Preset",QString::fromStdString(e.x265_preset));
    settings.setValue(prefix+"/twoPass",e.two_pass);settings.setValue(prefix+"/keyframeIntervalFrames",e.keyframe_interval_frames);
    settings.setValue(prefix+"/audioCodec",static_cast<int>(e.audio_codec));
    settings.setValue(prefix+"/ac3BitrateKbps",e.ac3_bitrate_kbps);
    settings.setValue(prefix+"/dcaBitrateKbps",e.dca_bitrate_kbps);
    settings.setValue(prefix+"/lpcmSampleRateHz",e.lpcm_sample_rate_hz);
    settings.setValue(prefix+"/lpcmBitDepth",e.lpcm_bit_depth);
    settings.setValue(prefix+"/x264AdvancedOptions",QString::fromStdString(e.x264_advanced_options));
    settings.setValue(prefix+"/x265AdvancedOptions",QString::fromStdString(e.x265_advanced_options));
    settings.setValue(prefix+"/mpeg2AdvancedOptions",QString::fromStdString(e.mpeg2_advanced_options));
    settings.setValue(prefix+"/ac3AdvancedOptions",QString::fromStdString(e.ac3_advanced_options));
    settings.setValue(prefix+"/dcaAdvancedOptions",QString::fromStdString(e.dca_advanced_options));
    settings.setValue(prefix+"/lpcmAdvancedOptions",QString::fromStdString(e.lpcm_advanced_options));
    settings.setValue(prefix+"/truehdAdvancedOptions",QString::fromStdString(e.truehd_advanced_options));
}
Rgba setting_rgba(QSettings& settings,const QString& key,const Rgba& fallback) {
    const QColor c(settings.value(key,QColor(fallback.r,fallback.g,fallback.b,fallback.a).name(QColor::HexArgb)).toString());
    return c.isValid()?rgba(c):fallback;
}
void save_rgba(QSettings& settings,const QString& key,const Rgba& c) {
    settings.setValue(key,QColor(c.r,c.g,c.b,c.a).name(QColor::HexArgb));
}
std::string concrete_gui_font_family(const std::string& family) {
    const auto families=QFontDatabase::families();
    auto exact=[&](const QString& requested)->QString{for(const auto& candidate:families)if(candidate.compare(requested,Qt::CaseInsensitive)==0)return candidate;return {};};
    if(!family.empty()&&!bdmvauthor::detail::is_generic_font_family(family)){const auto found=exact(QString::fromStdString(family));if(!found.isEmpty())return found.toStdString();}
    static constexpr std::array<const char*,14> fallbacks{{"Arial","Segoe UI","Tahoma","Verdana","Calibri","Liberation Sans","DejaVu Sans","Noto Sans","Carlito","Ubuntu","FreeSans","Nimbus Sans","TeX Gyre Heros","Helvetica"}};
    for(const char* candidate:fallbacks){const auto found=exact(QString::fromUtf8(candidate));if(!found.isEmpty())return found.toStdString();}
    throw std::runtime_error("No suitable sans-serif font is installed. Install Arial or another common sans-serif font before creating a project.");
}

void concretize_gui_menu_fonts(Menu& menu) {
    menu.default_button_style.font_family=concrete_gui_font_family(menu.default_button_style.font_family);
    for(auto& button:menu.buttons)button.style.font_family=concrete_gui_font_family(button.style.font_family);
    menu.back_button.style.font_family=concrete_gui_font_family(menu.back_button.style.font_family);
    for(auto& overlay:menu.overlays)if(overlay.kind==MenuOverlayKind::Text)overlay.font_family=concrete_gui_font_family(overlay.font_family);
    for(auto& child:menu.submenus)concretize_gui_menu_fonts(child);
}

ButtonStyle load_button_style_defaults(QSettings& settings,const ButtonStyle& fallback) {
    ButtonStyle style=fallback;const QString p="defaults/buttonStyle/";
    style.font_family=settings.value(p+"fontFamily",QString::fromStdString(style.font_family)).toString().toStdString();
    style.font_size_px=settings.value(p+"fontSizePx",style.font_size_px).toInt();
    style.bold=settings.value(p+"bold",style.bold).toBool();style.italic=settings.value(p+"italic",style.italic).toBool();
    style.corner_radius=settings.value(p+"cornerRadius",style.corner_radius).toInt();
    auto load_state=[&](const QString& name,ButtonStateStyle& state){const QString q=p+name+"/";state.text_color=setting_rgba(settings,q+"textColor",state.text_color);state.background_color=setting_rgba(settings,q+"backgroundColor",state.background_color);state.border_color=setting_rgba(settings,q+"borderColor",state.border_color);state.border_width=settings.value(q+"borderWidth",state.border_width).toInt();};
    load_state("normal",style.normal);load_state("active",style.active);load_state("selected",style.selected);load_state("activated",style.activated);return style;
}
void save_button_style_defaults(QSettings& settings,const ButtonStyle& style) {
    const QString p="defaults/buttonStyle/";settings.setValue(p+"fontFamily",QString::fromStdString(style.font_family));settings.setValue(p+"fontSizePx",style.font_size_px);settings.setValue(p+"bold",style.bold);settings.setValue(p+"italic",style.italic);settings.setValue(p+"cornerRadius",style.corner_radius);
    auto save_state=[&](const QString& name,const ButtonStateStyle& state){const QString q=p+name+"/";save_rgba(settings,q+"textColor",state.text_color);save_rgba(settings,q+"backgroundColor",state.background_color);save_rgba(settings,q+"borderColor",state.border_color);settings.setValue(q+"borderWidth",state.border_width);};
    save_state("normal",style.normal);save_state("active",style.active);save_state("selected",style.selected);save_state("activated",style.activated);
}
std::vector<std::pair<QString,std::uint64_t>> disc_size_presets(DiscTarget target);
bool disc_size_is_preset(DiscTarget target,std::uint64_t bytes);
void populate_disc_size_combo(QComboBox* combo,DiscTarget target,std::uint64_t selected);

NewProjectDefaults load_new_project_defaults() {
    QSettings settings;NewProjectDefaults d;
    d.volume_label=settings.value("defaults/project/volumeLabel",QString::fromStdString(d.volume_label)).toString().toStdString();
    settings.remove("defaults/project/outputImage"); // output paths are selected per project
    d.target=static_cast<DiscTarget>(settings.value("defaults/project/target",static_cast<int>(d.target)).toInt());
    d.disc_capacity_bytes=settings.value("defaults/project/blurayDiscCapacityBytes",QVariant::fromValue<qulonglong>(d.disc_capacity_bytes)).toULongLong();
    d.disc_uhd_capacity_bytes=settings.value("defaults/project/uhdDiscCapacityBytes",QVariant::fromValue<qulonglong>(d.disc_uhd_capacity_bytes)).toULongLong();
    d.disc_dvd_capacity_bytes=settings.value("defaults/project/dvdDiscCapacityBytes",QVariant::fromValue<qulonglong>(d.disc_dvd_capacity_bytes)).toULongLong();
    if(!disc_size_is_preset(DiscTarget::BluRay1080,d.disc_capacity_bytes))d.disc_capacity_bytes=default_disc_capacity_bytes(DiscTarget::BluRay1080);
    if(!disc_size_is_preset(DiscTarget::UltraHdBluRay2160,d.disc_uhd_capacity_bytes))d.disc_uhd_capacity_bytes=default_disc_capacity_bytes(DiscTarget::UltraHdBluRay2160);
    if(!disc_size_is_preset(DiscTarget::DvdVideo480p,d.disc_dvd_capacity_bytes))d.disc_dvd_capacity_bytes=default_disc_capacity_bytes(DiscTarget::DvdVideo480p);
    d.use_encode_cache=settings.value("defaults/project/useEncodeCache",d.use_encode_cache).toBool();
    d.use_compliance_cache=settings.value("defaults/project/useComplianceCache",d.use_compliance_cache).toBool();
    d.force_reencode_new_titles=settings.value("defaults/title/forceReencode",d.force_reencode_new_titles).toBool();
    d.title_encoding=load_encoding_defaults(settings,"defaults/titleEncoding",d.title_encoding);
    d.title_uhd_encoding=load_encoding_defaults(settings,"defaults/titleUhdEncoding",d.title_uhd_encoding);
    d.title_dvd_encoding=load_encoding_defaults(settings,"defaults/titleDvdEncoding",d.title_dvd_encoding);
    d.menu_encoding=load_encoding_defaults(settings,"defaults/menuEncoding",d.menu_encoding);
    d.menu_uhd_encoding=load_encoding_defaults(settings,"defaults/menuUhdEncoding",d.menu_uhd_encoding);
    d.menu_dvd_encoding=load_encoding_defaults(settings,"defaults/menuDvdEncoding",d.menu_dvd_encoding);
    d.menu_duration_seconds=settings.value("defaults/menu/durationSeconds",d.menu_duration_seconds).toDouble();
    d.menu_resolution=settings.value("defaults/menu/blurayResolution",QString::fromStdString(d.menu_resolution)).toString().toStdString();
    d.menu_uhd_resolution=settings.value("defaults/menu/uhdResolution",QString::fromStdString(d.menu_uhd_resolution)).toString().toStdString();
    d.menu_dvd_resolution=settings.value("defaults/menu/dvdResolution",QString::fromStdString(d.menu_dvd_resolution)).toString().toStdString();
    const auto valid_bd_menu_resolution=[](const std::string& r){return r=="1920x1080"||r=="1440x1080"||r=="1280x720"||r=="720x576"||r=="720x480";};
    const auto valid_dvd_menu_resolution=[](const std::string& r){return r=="720x480"||r=="720x576"||r=="704x480"||r=="704x576"||r=="352x480"||r=="352x576"||r=="352x240"||r=="352x288";};
    if(!valid_bd_menu_resolution(d.menu_resolution))d.menu_resolution="1920x1080";
    if(d.menu_uhd_resolution!="1920x1080"&&d.menu_uhd_resolution!="3840x2160")d.menu_uhd_resolution="1920x1080";
    if(!valid_dvd_menu_resolution(d.menu_dvd_resolution))d.menu_dvd_resolution="720x480";
    d.button_style=load_button_style_defaults(settings,d.button_style);
    d.button_width=settings.value("defaults/button/width",d.button_width).toInt();d.button_height=settings.value("defaults/button/height",d.button_height).toInt();
    d.label_font_family=settings.value("defaults/label/fontFamily",QString::fromStdString(d.label_font_family)).toString().toStdString();
    d.label_font_size_px=settings.value("defaults/label/fontSizePx",d.label_font_size_px).toInt();d.label_bold=settings.value("defaults/label/bold",d.label_bold).toBool();d.label_italic=settings.value("defaults/label/italic",d.label_italic).toBool();
    const QVariant subtitle_defaults_value=settings.value("defaults/subtitle/style",subtitle_style_map(d.subtitle_style));
    const QVariantMap subtitle_defaults_map=subtitle_defaults_value.toMap();
    d.subtitle_style=subtitle_style_from_map(subtitle_defaults_value);
    if(!subtitle_defaults_map.contains("fontSizeUnits")&&!d.subtitle_style.override_style){d.subtitle_style.font_family.clear();d.subtitle_style.font_size_px=80;d.subtitle_style.bottom_offset_px=80;}
    d.button_style.font_family=concrete_gui_font_family(d.button_style.font_family);
    d.label_font_family=concrete_gui_font_family(d.label_font_family);
    d.subtitle_style.font_family=concrete_gui_font_family(d.subtitle_style.font_family);
    const bool legacy_design_settings=!settings.contains("defaults/designCanvasVersion") && (settings.contains("defaults/button/width")||settings.contains("defaults/buttonStyle/fontSizePx")||settings.contains("defaults/label/fontSizePx"));
    if(legacy_design_settings){d.button_width*=2;d.button_height*=2;d.label_font_size_px*=2;d.button_style.font_size_px*=2;d.button_style.corner_radius*=2;d.button_style.normal.border_width*=2;d.button_style.active.border_width*=2;d.button_style.selected.border_width*=2;d.button_style.activated.border_width*=2;}
    return d;
}
void save_new_project_defaults(const NewProjectDefaults& d) {
    QSettings settings;settings.setValue("defaults/project/volumeLabel",QString::fromStdString(d.volume_label));settings.remove("defaults/project/outputImage");settings.setValue("defaults/project/target",static_cast<int>(d.target));settings.setValue("defaults/project/blurayDiscCapacityBytes",QVariant::fromValue<qulonglong>(d.disc_capacity_bytes));settings.setValue("defaults/project/uhdDiscCapacityBytes",QVariant::fromValue<qulonglong>(d.disc_uhd_capacity_bytes));settings.setValue("defaults/project/dvdDiscCapacityBytes",QVariant::fromValue<qulonglong>(d.disc_dvd_capacity_bytes));settings.setValue("defaults/project/useEncodeCache",d.use_encode_cache);settings.setValue("defaults/project/useComplianceCache",d.use_compliance_cache);settings.setValue("defaults/title/forceReencode",d.force_reencode_new_titles);settings.setValue("defaults/designCanvasVersion",2);
    save_encoding_defaults(settings,"defaults/titleEncoding",d.title_encoding);save_encoding_defaults(settings,"defaults/titleUhdEncoding",d.title_uhd_encoding);save_encoding_defaults(settings,"defaults/titleDvdEncoding",d.title_dvd_encoding);save_encoding_defaults(settings,"defaults/menuEncoding",d.menu_encoding);save_encoding_defaults(settings,"defaults/menuUhdEncoding",d.menu_uhd_encoding);save_encoding_defaults(settings,"defaults/menuDvdEncoding",d.menu_dvd_encoding);settings.setValue("defaults/menu/durationSeconds",d.menu_duration_seconds);settings.setValue("defaults/menu/blurayResolution",QString::fromStdString(d.menu_resolution));settings.setValue("defaults/menu/uhdResolution",QString::fromStdString(d.menu_uhd_resolution));settings.setValue("defaults/menu/dvdResolution",QString::fromStdString(d.menu_dvd_resolution));save_button_style_defaults(settings,d.button_style);
    settings.setValue("defaults/button/width",d.button_width);settings.setValue("defaults/button/height",d.button_height);settings.setValue("defaults/label/fontFamily",QString::fromStdString(d.label_font_family));settings.setValue("defaults/label/fontSizePx",d.label_font_size_px);settings.setValue("defaults/label/bold",d.label_bold);settings.setValue("defaults/label/italic",d.label_italic);settings.setValue("defaults/subtitle/style",subtitle_style_map(d.subtitle_style));settings.sync();
}

void warn_uhd_4k_menu_vlc_mouse_bug(QWidget* parent) {
    QMessageBox::warning(parent,"4K UHD menus and VLC mouse navigation",
        "VLC currently has a UHD Blu-ray mouse-coordinate bug: 3840x2160 video is displayed with a 1920x1080 Blu-ray Interactive Graphics plane, but VLC passes the unscaled 4K mouse coordinates to libbluray. As a result, clickable button areas appear up and to the left of the visible buttons.\n\n"
        "Arrow-key navigation still works, and hardware UHD players are expected to handle the graphics plane correctly. BDMV Author therefore defaults UHD menus to 1920x1080 for VLC compatibility. Use 3840x2160 only if you accept broken mouse navigation in affected VLC versions.");
}

void populate_default_menu_resolution_combo(QComboBox* combo, DiscTarget target, const std::string& selected) {
    combo->clear();
    auto add=[combo](const char* text,const char* token){combo->addItem(QString::fromUtf8(text),QString::fromLatin1(token));};
    if(target==DiscTarget::UltraHdBluRay2160){
        add("1920×1080 (recommended for VLC)","1920x1080");
        add("3840×2160","3840x2160");
    }else if(target==DiscTarget::BluRay1080){
        add("1920×1080","1920x1080");add("1440×1080 anamorphic","1440x1080");add("1280×720","1280x720");add("720×576 SD","720x576");add("720×480 SD","720x480");
    }else{
        add("720×480 NTSC","720x480");add("720×576 PAL","720x576");add("704×480 NTSC","704x480");add("704×576 PAL","704x576");add("352×480 NTSC","352x480");add("352×576 PAL","352x576");add("352×240 NTSC","352x240");add("352×288 PAL","352x288");
    }
    int index=combo->findData(QString::fromStdString(selected));if(index<0)index=0;combo->setCurrentIndex(index);
}

std::vector<std::pair<QString,std::uint64_t>> disc_size_presets(DiscTarget target) {
    if(target==DiscTarget::DvdVideo480p)return {
        {"1.46 GB (8 cm single-layer DVD)",1460000000ULL},
        {"2.66 GB (8 cm dual-layer DVD)",2660000000ULL},
        {"4.7 GB (single-layer DVD)",4700000000ULL},
        {"8.5 GB (dual-layer DVD)",8500000000ULL},
        {"Unlimited",0ULL}};
    if(target==DiscTarget::UltraHdBluRay2160)return {
        {"7.8 GB (8 cm, non-standard UHD target)",7800000000ULL},
        {"15.6 GB (8 cm dual-layer, non-standard UHD target)",15600000000ULL},
        {"25 GB (non-standard UHD target)",25000000000ULL},
        {"50 GB",50000000000ULL},
        {"66 GB",66000000000ULL},
        {"100 GB",100000000000ULL},
        {"Unlimited",0ULL}};
    return {
        {"7.8 GB (8 cm single-layer BD)",7800000000ULL},
        {"15.6 GB (8 cm dual-layer BD)",15600000000ULL},
        {"25 GB",25000000000ULL},
        {"50 GB",50000000000ULL},
        {"Unlimited",0ULL}};
}
bool disc_size_is_preset(DiscTarget target,std::uint64_t bytes){for(const auto& entry:disc_size_presets(target))if(entry.second==bytes)return true;return false;}
void populate_disc_size_combo(QComboBox* combo,DiscTarget target,std::uint64_t selected){
    if(!combo)return;combo->clear();for(const auto& entry:disc_size_presets(target))combo->addItem(entry.first,QVariant::fromValue<qulonglong>(entry.second));
    int index=combo->findData(QVariant::fromValue<qulonglong>(selected));if(index<0)index=combo->findData(QVariant::fromValue<qulonglong>(default_disc_capacity_bytes(target)));if(index<0)index=0;combo->setCurrentIndex(index);
}

class DefaultsDialog final : public QDialog {
    NewProjectDefaults value_;
    QLineEdit *volume_=nullptr;
    QComboBox* target_=nullptr;
    QComboBox *bluray_size_default_=nullptr,*uhd_size_default_=nullptr,*dvd_size_default_=nullptr;
    QComboBox *bluray_menu_resolution_default_=nullptr,*uhd_menu_resolution_default_=nullptr,*dvd_menu_resolution_default_=nullptr;
    QCheckBox *cache_=nullptr,*compliance_cache_=nullptr,*force_reencode_titles_=nullptr;
    QComboBox *title_video_=nullptr,*title_preset_=nullptr,*title_audio_=nullptr,*menu_video_=nullptr,*menu_preset_=nullptr,*menu_audio_=nullptr;
    QComboBox *title_uhd_video_=nullptr,*title_uhd_preset_=nullptr,*title_uhd_audio_=nullptr,*menu_uhd_video_=nullptr,*menu_uhd_preset_=nullptr,*menu_uhd_audio_=nullptr;
    QComboBox *title_dvd_video_=nullptr,*title_dvd_preset_=nullptr,*title_dvd_audio_=nullptr,*menu_dvd_video_=nullptr,*menu_dvd_preset_=nullptr,*menu_dvd_audio_=nullptr;
    QSpinBox *title_video_bitrate_=nullptr,*title_video_minrate_=nullptr,*title_video_maxrate_=nullptr,*title_audio_bitrate_=nullptr,*menu_video_bitrate_=nullptr,*menu_video_minrate_=nullptr,*menu_video_maxrate_=nullptr,*menu_audio_bitrate_=nullptr,*button_width_=nullptr,*button_height_=nullptr,*label_size_=nullptr;
    QSpinBox *title_uhd_video_bitrate_=nullptr,*title_uhd_video_minrate_=nullptr,*title_uhd_video_maxrate_=nullptr,*title_uhd_audio_bitrate_=nullptr,*menu_uhd_video_bitrate_=nullptr,*menu_uhd_video_minrate_=nullptr,*menu_uhd_video_maxrate_=nullptr,*menu_uhd_audio_bitrate_=nullptr;
    QSpinBox *title_dvd_video_bitrate_=nullptr,*title_dvd_video_minrate_=nullptr,*title_dvd_video_maxrate_=nullptr,*title_dvd_audio_bitrate_=nullptr,*menu_dvd_video_bitrate_=nullptr,*menu_dvd_video_minrate_=nullptr,*menu_dvd_video_maxrate_=nullptr,*menu_dvd_audio_bitrate_=nullptr;
    QSpinBox *title_keyframe_=nullptr,*menu_keyframe_=nullptr,*title_uhd_keyframe_=nullptr,*menu_uhd_keyframe_=nullptr,*title_dvd_keyframe_=nullptr,*menu_dvd_keyframe_=nullptr;
    QCheckBox *title_two_pass_=nullptr,*menu_two_pass_=nullptr,*title_uhd_two_pass_=nullptr,*menu_uhd_two_pass_=nullptr,*title_dvd_two_pass_=nullptr,*menu_dvd_two_pass_=nullptr,*label_bold_=nullptr,*label_italic_=nullptr;
    QDoubleSpinBox* menu_duration_=nullptr;
    QFontComboBox* label_font_=nullptr;
    QPushButton *button_style_button_=nullptr,*subtitle_style_button_=nullptr;
    EncodingProfile title_base_,menu_base_,title_uhd_base_,menu_uhd_base_,title_dvd_base_,menu_dvd_base_;
    ButtonStyle button_style_;
    SubtitleStyle subtitle_style_;
    bool restore_defaults_=false;
    void bind_audio(QComboBox* codec,QSpinBox* bitrate,EncodingProfile* base,DiscTarget target) {
        connect(bitrate,QOverload<int>::of(&QSpinBox::valueChanged),this,[codec,base](int v){base->audio_codec=static_cast<AudioCodec>(codec->currentData().toInt());if(audio_codec_has_configurable_bitrate(base->audio_codec))set_audio_bitrate_kbps(*base,v);});
        connect(codec,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[codec,bitrate,base,target](int){base->audio_codec=static_cast<AudioCodec>(codec->currentData().toInt());configure_audio_bitrate_spin(bitrate,target,base->audio_codec,audio_bitrate_kbps(*base));});
    }
public:
    DefaultsDialog(QWidget* parent,const NewProjectDefaults& initial):QDialog(parent),value_(initial),title_base_(initial.title_encoding),menu_base_(initial.menu_encoding),title_uhd_base_(initial.title_uhd_encoding),menu_uhd_base_(initial.menu_uhd_encoding),title_dvd_base_(initial.title_dvd_encoding),menu_dvd_base_(initial.menu_dvd_encoding),button_style_(initial.button_style),subtitle_style_(initial.subtitle_style) {
        setWindowTitle("Defaults for new projects");resize(700,620);auto* root=new QVBoxLayout(this);auto* tabs=new QTabWidget;root->addWidget(tabs,1);
        auto* project=new QWidget;auto* pf=new QFormLayout(project);volume_=new QLineEdit(QString::fromStdString(initial.volume_label));target_=new QComboBox(project);target_->addItem("Blu-ray 1080p",static_cast<int>(DiscTarget::BluRay1080));target_->addItem("Ultra HD Blu-ray 2160p",static_cast<int>(DiscTarget::UltraHdBluRay2160));target_->addItem("DVD-Video",static_cast<int>(DiscTarget::DvdVideo480p));target_->setCurrentIndex(target_->findData(static_cast<int>(initial.target)));cache_=new QCheckBox("Use encoded-media cache");cache_->setChecked(initial.use_encode_cache);compliance_cache_=new QCheckBox("Use cached compliance analysis");compliance_cache_->setChecked(initial.use_compliance_cache);bluray_size_default_=new QComboBox(project);uhd_size_default_=new QComboBox(project);dvd_size_default_=new QComboBox(project);populate_disc_size_combo(bluray_size_default_,DiscTarget::BluRay1080,initial.disc_capacity_bytes);populate_disc_size_combo(uhd_size_default_,DiscTarget::UltraHdBluRay2160,initial.disc_uhd_capacity_bytes);populate_disc_size_combo(dvd_size_default_,DiscTarget::DvdVideo480p,initial.disc_dvd_capacity_bytes);for(auto* combo:{bluray_size_default_,uhd_size_default_,dvd_size_default_}){combo->setMinimumContentsLength(34);combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);}pf->addRow("Default output target",target_);pf->addRow("Default UDF Disc label",volume_);pf->addRow("Default Blu-ray size",bluray_size_default_);pf->addRow("Default UHD size",uhd_size_default_);pf->addRow("Default DVD size",dvd_size_default_);auto* size_note=new QLabel("8 cm Blu-ray capacities and the 25 GB/8 cm UHD choices are size targets for compatible players/media; they do not change codec or transport compliance rules.",project);size_note->setWordWrap(true);pf->addRow(size_note);auto* output_note=new QLabel("Disc image files are chosen separately for each project.",project);output_note->setWordWrap(true);pf->addRow("Disc image",output_note);pf->addRow(cache_);pf->addRow(compliance_cache_);tabs->addTab(project,"Project");
        auto make_encoding_page=[&](DiscTarget target,const EncodingProfile& e,EncodingProfile* base,QComboBox*& video,QSpinBox*& vbit,QSpinBox*& vmin,QSpinBox*& vmax,QComboBox*& preset,QCheckBox*& two,QComboBox*& audio,QSpinBox*& abit,QSpinBox*& keyframe){auto* page=new QWidget;auto* f=new QFormLayout(page);video=video_combo(page,target,e.video_codec);vbit=bitrate_spin(page);configure_video_bitrate_spin(vbit,target,e.video_codec,e.video_bitrate_kbps);vmin=video_rate_limit_spin(page);configure_video_minrate_spin(vmin,target,e.video_codec,e.video_min_bitrate_kbps);vmax=video_rate_limit_spin(page);configure_video_maxrate_spin(vmax,target,e.video_codec,e.video_max_bitrate_kbps);preset=preset_combo(page);preset->setCurrentText(QString::fromStdString(e.video_codec==VideoCodec::Hevc?e.x265_preset:e.x264_preset));two=new QCheckBox("Enable two-pass encoding",page);two->setChecked(e.two_pass);keyframe=keyframe_interval_spin(page,e.keyframe_interval_frames);configure_keyframe_interval_spin(keyframe,target,e.video_codec,"auto",e.keyframe_interval_frames);audio=audio_combo(page,target,e.audio_codec);abit=audio_bitrate_spin(page);configure_audio_bitrate_spin(abit,target,e.audio_codec,audio_bitrate_kbps(e));auto* advanced=new QPushButton("Advanced…",page);advanced->setToolTip("Set codec-private video and audio options. Disc-spec-critical options remain controlled by BDMV Author.");f->addRow("Video codec",video);f->addRow("Video bitrate",vbit);f->addRow("Video minrate",vmin);f->addRow("Video maxrate",vmax);f->addRow("Encoder speed preset",preset);f->addRow("Passes",two);f->addRow("GOP / keyframe interval",keyframe);f->addRow("Audio codec",audio);f->addRow("Audio bitrate",abit);f->addRow("Advanced codec options",advanced);connect(video,QOverload<int>::of(&QComboBox::currentIndexChanged),page,[video,vbit,vmin,vmax,keyframe,target](int){const auto codec=static_cast<VideoCodec>(video->currentData().toInt());configure_video_bitrate_spin(vbit,target,codec,vbit->value());configure_video_minrate_spin(vmin,target,codec,vmin->value());configure_video_maxrate_spin(vmax,target,codec,vmax->value());configure_keyframe_interval_spin(keyframe,target,codec,"auto");});connect(advanced,&QPushButton::clicked,page,[page,target,base,video,audio](bool){base->video_codec=static_cast<VideoCodec>(video->currentData().toInt());base->audio_codec=static_cast<AudioCodec>(audio->currentData().toInt());edit_advanced_codec_options(page,target,*base);});return page;};
        auto* title_page=make_encoding_page(DiscTarget::BluRay1080,initial.title_encoding,&title_base_,title_video_,title_video_bitrate_,title_video_minrate_,title_video_maxrate_,title_preset_,title_two_pass_,title_audio_,title_audio_bitrate_,title_keyframe_);auto* title_layout=qobject_cast<QFormLayout*>(title_page->layout());force_reencode_titles_=new QCheckBox("Always re-encode new titles",title_page);force_reencode_titles_->setChecked(initial.force_reencode_new_titles);force_reencode_titles_->setToolTip("When disabled, BDMV Author remuxes already-compliant Blu-ray video/audio streams without re-encoding them.");title_layout->addRow("Passthrough policy",force_reencode_titles_);tabs->addTab(title_page,"New titles — Blu-ray");bind_audio(title_audio_,title_audio_bitrate_,&title_base_,DiscTarget::BluRay1080);
        auto* menu_page=make_encoding_page(DiscTarget::BluRay1080,initial.menu_encoding,&menu_base_,menu_video_,menu_video_bitrate_,menu_video_minrate_,menu_video_maxrate_,menu_preset_,menu_two_pass_,menu_audio_,menu_audio_bitrate_,menu_keyframe_);auto* menu_layout=qobject_cast<QFormLayout*>(menu_page->layout());bluray_menu_resolution_default_=new QComboBox(menu_page);populate_default_menu_resolution_combo(bluray_menu_resolution_default_,DiscTarget::BluRay1080,initial.menu_resolution);bluray_menu_resolution_default_->setToolTip("Default menu raster for newly created standard Blu-ray projects. This setting is independent from the UHD and DVD defaults.");menu_layout->addRow("Default menu resolution",bluray_menu_resolution_default_);menu_duration_=new QDoubleSpinBox(menu_page);menu_duration_->setRange(10.0,36000.0);menu_duration_->setDecimals(1);menu_duration_->setSuffix(" s");menu_duration_->setValue(initial.menu_duration_seconds);menu_layout->addRow("Menu duration",menu_duration_);tabs->addTab(menu_page,"Menus — Blu-ray");bind_audio(menu_audio_,menu_audio_bitrate_,&menu_base_,DiscTarget::BluRay1080);
        auto* title_uhd_page=make_encoding_page(DiscTarget::UltraHdBluRay2160,initial.title_uhd_encoding,&title_uhd_base_,title_uhd_video_,title_uhd_video_bitrate_,title_uhd_video_minrate_,title_uhd_video_maxrate_,title_uhd_preset_,title_uhd_two_pass_,title_uhd_audio_,title_uhd_audio_bitrate_,title_uhd_keyframe_);tabs->addTab(title_uhd_page,"New titles — UHD");bind_audio(title_uhd_audio_,title_uhd_audio_bitrate_,&title_uhd_base_,DiscTarget::UltraHdBluRay2160);
        auto* menu_uhd_page=make_encoding_page(DiscTarget::UltraHdBluRay2160,initial.menu_uhd_encoding,&menu_uhd_base_,menu_uhd_video_,menu_uhd_video_bitrate_,menu_uhd_video_minrate_,menu_uhd_video_maxrate_,menu_uhd_preset_,menu_uhd_two_pass_,menu_uhd_audio_,menu_uhd_audio_bitrate_,menu_uhd_keyframe_);auto* menu_uhd_layout=qobject_cast<QFormLayout*>(menu_uhd_page->layout());uhd_menu_resolution_default_=new QComboBox(menu_uhd_page);populate_default_menu_resolution_combo(uhd_menu_resolution_default_,DiscTarget::UltraHdBluRay2160,initial.menu_uhd_resolution);uhd_menu_resolution_default_->setToolTip("1920×1080 is the default workaround for VLC's UHD Blu-ray mouse-coordinate bug. Hardware players may use 3840×2160 menus correctly.");menu_uhd_layout->addRow("Default menu resolution",uhd_menu_resolution_default_);connect(uhd_menu_resolution_default_,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this](int){if(uhd_menu_resolution_default_->currentData().toString()!=QStringLiteral("1920x1080"))warn_uhd_4k_menu_vlc_mouse_bug(this);});tabs->addTab(menu_uhd_page,"Menus — UHD");bind_audio(menu_uhd_audio_,menu_uhd_audio_bitrate_,&menu_uhd_base_,DiscTarget::UltraHdBluRay2160);
        auto* title_dvd_page=make_encoding_page(DiscTarget::DvdVideo480p,initial.title_dvd_encoding,&title_dvd_base_,title_dvd_video_,title_dvd_video_bitrate_,title_dvd_video_minrate_,title_dvd_video_maxrate_,title_dvd_preset_,title_dvd_two_pass_,title_dvd_audio_,title_dvd_audio_bitrate_,title_dvd_keyframe_);tabs->addTab(title_dvd_page,"New titles — DVD");bind_audio(title_dvd_audio_,title_dvd_audio_bitrate_,&title_dvd_base_,DiscTarget::DvdVideo480p);
        auto* menu_dvd_page=make_encoding_page(DiscTarget::DvdVideo480p,initial.menu_dvd_encoding,&menu_dvd_base_,menu_dvd_video_,menu_dvd_video_bitrate_,menu_dvd_video_minrate_,menu_dvd_video_maxrate_,menu_dvd_preset_,menu_dvd_two_pass_,menu_dvd_audio_,menu_dvd_audio_bitrate_,menu_dvd_keyframe_);auto* menu_dvd_layout=qobject_cast<QFormLayout*>(menu_dvd_page->layout());dvd_menu_resolution_default_=new QComboBox(menu_dvd_page);populate_default_menu_resolution_combo(dvd_menu_resolution_default_,DiscTarget::DvdVideo480p,initial.menu_dvd_resolution);dvd_menu_resolution_default_->setToolTip("Default menu raster for newly created DVD-Video projects. PAL and NTSC families must still obey the project's frame-rate family constraints.");menu_dvd_layout->addRow("Default menu resolution",dvd_menu_resolution_default_);tabs->addTab(menu_dvd_page,"Menus — DVD");bind_audio(menu_dvd_audio_,menu_dvd_audio_bitrate_,&menu_dvd_base_,DiscTarget::DvdVideo480p);
        auto* subtitles_page=new QWidget;auto* subtitles_layout=new QVBoxLayout(subtitles_page);auto* subtitle_note=new QLabel("These defaults are copied into each new title. Individual subtitle streams can override them later. The editor separates Common, Blu-ray/UHD, and DVD properties; leaving the override disabled keeps backend rendering defaults.",subtitles_page);subtitle_note->setWordWrap(true);subtitles_layout->addWidget(subtitle_note);subtitle_style_button_=new QPushButton("Edit default subtitle properties…",subtitles_page);subtitles_layout->addWidget(subtitle_style_button_);subtitles_layout->addStretch(1);tabs->addTab(subtitles_page,"Subtitles");connect(subtitle_style_button_,&QPushButton::clicked,this,[this]{SubtitleStyleDialog d(this,DiscTarget::BluRay1080,true,subtitle_style_,true);if(d.exec()==QDialog::Accepted)subtitle_style_=d.value();});
        auto* appearance=new QWidget;auto* af=new QFormLayout(appearance);button_style_button_=new QPushButton("Edit default text-button style…",appearance);button_width_=new QSpinBox(appearance);button_width_->setRange(160,3600);button_width_->setSuffix(" px");button_width_->setValue(initial.button_width);button_height_=new QSpinBox(appearance);button_height_->setRange(48,600);button_height_->setSuffix(" px");button_height_->setValue(initial.button_height);label_font_=new QFontComboBox(appearance);label_font_->setCurrentFont(QFont(QString::fromStdString(initial.label_font_family)));label_size_=new QSpinBox(appearance);label_size_->setRange(12,1024);label_size_->setSuffix(" px");label_size_->setValue(initial.label_font_size_px);label_bold_=new QCheckBox(appearance);label_bold_->setChecked(initial.label_bold);label_italic_=new QCheckBox(appearance);label_italic_->setChecked(initial.label_italic);af->addRow("Button appearance",button_style_button_);af->addRow("New button width",button_width_);af->addRow("New button height",button_height_);af->addRow("Text-label font",label_font_);af->addRow("Text-label size",label_size_);af->addRow("Text labels bold",label_bold_);af->addRow("Text labels italic",label_italic_);tabs->addTab(appearance,"Buttons & text");
        connect(button_style_button_,&QPushButton::clicked,this,[this]{ButtonStyleDialog d(this,button_style_);if(d.exec()==QDialog::Accepted)button_style_=d.value();});
        auto* buttons=new QDialogButtonBox(QDialogButtonBox::RestoreDefaults|QDialogButtonBox::Ok|QDialogButtonBox::Cancel);root->addWidget(buttons);connect(buttons,&QDialogButtonBox::accepted,this,&QDialog::accept);connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject);connect(buttons->button(QDialogButtonBox::RestoreDefaults),&QPushButton::clicked,this,[this]{restore_defaults_=true;accept();});
    }
    NewProjectDefaults value() const {
        if(restore_defaults_)return NewProjectDefaults{};
        NewProjectDefaults d=value_;d.volume_label=volume_->text().toStdString();d.target=static_cast<DiscTarget>(target_->currentData().toInt());d.disc_capacity_bytes=static_cast<std::uint64_t>(bluray_size_default_->currentData().toULongLong());d.disc_uhd_capacity_bytes=static_cast<std::uint64_t>(uhd_size_default_->currentData().toULongLong());d.disc_dvd_capacity_bytes=static_cast<std::uint64_t>(dvd_size_default_->currentData().toULongLong());d.use_encode_cache=cache_->isChecked();d.use_compliance_cache=compliance_cache_->isChecked();d.force_reencode_new_titles=force_reencode_titles_->isChecked();
        d.title_encoding=profile_from_widgets(title_video_,title_video_bitrate_,title_preset_,title_two_pass_,title_audio_,title_audio_bitrate_,&title_base_,title_keyframe_,title_video_minrate_,title_video_maxrate_);d.title_uhd_encoding=profile_from_widgets(title_uhd_video_,title_uhd_video_bitrate_,title_uhd_preset_,title_uhd_two_pass_,title_uhd_audio_,title_uhd_audio_bitrate_,&title_uhd_base_,title_uhd_keyframe_,title_uhd_video_minrate_,title_uhd_video_maxrate_);d.title_dvd_encoding=profile_from_widgets(title_dvd_video_,title_dvd_video_bitrate_,title_dvd_preset_,title_dvd_two_pass_,title_dvd_audio_,title_dvd_audio_bitrate_,&title_dvd_base_,title_dvd_keyframe_,title_dvd_video_minrate_,title_dvd_video_maxrate_);d.menu_encoding=profile_from_widgets(menu_video_,menu_video_bitrate_,menu_preset_,menu_two_pass_,menu_audio_,menu_audio_bitrate_,&menu_base_,menu_keyframe_,menu_video_minrate_,menu_video_maxrate_);d.menu_uhd_encoding=profile_from_widgets(menu_uhd_video_,menu_uhd_video_bitrate_,menu_uhd_preset_,menu_uhd_two_pass_,menu_uhd_audio_,menu_uhd_audio_bitrate_,&menu_uhd_base_,menu_uhd_keyframe_,menu_uhd_video_minrate_,menu_uhd_video_maxrate_);d.menu_dvd_encoding=profile_from_widgets(menu_dvd_video_,menu_dvd_video_bitrate_,menu_dvd_preset_,menu_dvd_two_pass_,menu_dvd_audio_,menu_dvd_audio_bitrate_,&menu_dvd_base_,menu_dvd_keyframe_,menu_dvd_video_minrate_,menu_dvd_video_maxrate_);d.menu_duration_seconds=menu_duration_->value();d.menu_resolution=bluray_menu_resolution_default_->currentData().toString().toStdString();d.menu_uhd_resolution=uhd_menu_resolution_default_->currentData().toString().toStdString();d.menu_dvd_resolution=dvd_menu_resolution_default_->currentData().toString().toStdString();d.button_style=button_style_;d.button_width=button_width_->value();d.button_height=button_height_->value();d.label_font_family=label_font_->currentFont().family().toStdString();d.label_font_size_px=label_size_->value();d.label_bold=label_bold_->isChecked();d.label_italic=label_italic_->isChecked();d.subtitle_style=subtitle_style_;return d;
    }
};

QWidget* file_row(QWidget* parent,QLineEdit* edit,const QString& caption){
    auto* w=new QWidget(parent);auto* l=new QHBoxLayout(w);l->setContentsMargins(0,0,0,0);l->addWidget(edit);auto* b=new QPushButton("Browse…",w);l->addWidget(b);
    QObject::connect(b,&QPushButton::clicked,w,[w,edit,caption]{const auto f=QFileDialog::getOpenFileName(w,caption);if(!f.isEmpty())edit->setText(f);});return w;
}

struct TargetChoice {
    QString label;
    QString suggested_label;
    MenuButtonTargetKind kind = MenuButtonTargetKind::Title;
    std::uint16_t title = 1;
    std::uint16_t chapter = 0;
    std::uint16_t stream = 1;
    std::string menu_id;
};

NavigationAction action_from_choice(const TargetChoice& t){
    NavigationAction action;
    switch(t.kind){
        case MenuButtonTargetKind::Title:action.kind=NavigationActionKind::PlayTitle;break;
        case MenuButtonTargetKind::Menu:action.kind=NavigationActionKind::Menu;break;
        case MenuButtonTargetKind::AudioTrack:action.kind=NavigationActionKind::AudioTrack;break;
        case MenuButtonTargetKind::SubtitleTrack:action.kind=NavigationActionKind::SubtitleTrack;break;
        case MenuButtonTargetKind::SubtitleOff:action.kind=NavigationActionKind::SubtitleOff;break;
    }
    action.target_title=t.title;action.target_chapter=t.chapter;action.target_stream=t.stream;action.target_menu_id=t.menu_id;return action;
}
bool action_matches_choice(const NavigationAction& action,const TargetChoice& t){
    if(navigation_action_is_repeat_marker(action.kind))return false;
    const auto candidate=action_from_choice(t);if(candidate.kind!=action.kind)return false;
    if(action.kind==NavigationActionKind::Menu)return candidate.target_menu_id==action.target_menu_id;
    if(candidate.target_title!=action.target_title)return false;
    if(action.kind==NavigationActionKind::PlayTitle)return candidate.target_chapter==action.target_chapter;
    if(action.kind==NavigationActionKind::AudioTrack||action.kind==NavigationActionKind::SubtitleTrack)return candidate.target_stream==action.target_stream;
    return true;
}
QString stored_action_label(const NavigationAction& action){
    switch(action.kind){
        case NavigationActionKind::PlayTitle:return action.target_chapter>1U?QString("Play title %1 — chapter %2").arg(action.target_title).arg(action.target_chapter):QString("Play title %1").arg(action.target_title);
        case NavigationActionKind::Menu:return "Open menu "+QString::fromStdString(action.target_menu_id);
        case NavigationActionKind::AudioTrack:return QString("Select title %1 audio %2").arg(action.target_title).arg(action.target_stream);
        case NavigationActionKind::SubtitleTrack:return QString("Select title %1 subtitle %2").arg(action.target_title).arg(action.target_stream);
        case NavigationActionKind::SubtitleOff:return QString("Turn subtitles off for title %1").arg(action.target_title);
        case NavigationActionKind::RepeatBegin:return action.repeat_count==0U?QString("Repeat group forever {"):QString("Repeat group ×%1 {").arg(action.repeat_count);
        case NavigationActionKind::RepeatEnd:return "} End repeat group";
    }
    return "Action";
}
QString action_label(const NavigationAction& action,const std::vector<TargetChoice>& choices){
    QString base=stored_action_label(action);for(const auto& choice:choices)if(action_matches_choice(action,choice)){base=choice.label;break;}
    if(navigation_action_is_repeat_marker(action.kind))return base;
    if(action.repeat_count==0U)return base+"  ↻ forever";
    if(action.repeat_count>1U)return base+QString("  ×%1").arg(action.repeat_count);
    return base;
}
QString action_sequence_summary(const std::vector<NavigationAction>& actions,const std::vector<TargetChoice>& choices){
    if(actions.empty())return "No actions";
    QStringList labels;for(const auto& action:actions)labels<<action_label(action,choices);
    return labels.join("  →  ");
}

class ChapterSettingsDialog final : public QDialog {
    QComboBox* mode_=nullptr;
    QSpinBox* minutes_=nullptr;
    QDoubleSpinBox* seconds_=nullptr;
    QPlainTextEdit* manual_=nullptr;
    std::vector<double> chapters_;
    static QString format_time(double seconds){
        const auto ms=static_cast<long long>(std::llround(std::max(0.0,seconds)*1000.0));
        const auto h=ms/3600000LL,m=(ms/60000LL)%60LL,s=(ms/1000LL)%60LL,rem=ms%1000LL;
        if(h>0)return QString("%1:%2:%3.%4").arg(h).arg(m,2,10,QChar('0')).arg(s,2,10,QChar('0')).arg(rem,3,10,QChar('0'));
        return QString("%1:%2.%3").arg(m,2,10,QChar('0')).arg(s,2,10,QChar('0')).arg(rem,3,10,QChar('0'));
    }
    static std::optional<double> parse_time(QString text){
        text=text.trimmed();if(text.isEmpty())return std::nullopt;
        const auto parts=text.split(':');bool ok=false;
        if(parts.size()==1){const double v=parts[0].toDouble(&ok);if(ok&&v>=0.0)return v;return std::nullopt;}
        if(parts.size()==2){const double m=parts[0].toDouble(&ok);if(!ok)return std::nullopt;bool ok2=false;const double sec=parts[1].toDouble(&ok2);if(ok2&&m>=0.0&&sec>=0.0&&sec<60.0)return m*60.0+sec;return std::nullopt;}
        if(parts.size()==3){const double h=parts[0].toDouble(&ok);if(!ok)return std::nullopt;bool ok2=false,ok3=false;const double m=parts[1].toDouble(&ok2),sec=parts[2].toDouble(&ok3);if(ok2&&ok3&&h>=0.0&&m>=0.0&&m<60.0&&sec>=0.0&&sec<60.0)return h*3600.0+m*60.0+sec;}
        return std::nullopt;
    }
    void update_enabled(){const auto mode=static_cast<ChapterMode>(mode_->currentData().toInt());manual_->setEnabled(mode==ChapterMode::Manual);minutes_->setEnabled(mode==ChapterMode::Interval);seconds_->setEnabled(mode==ChapterMode::Interval);}
public:
    ChapterSettingsDialog(QWidget* parent,ChapterMode mode,std::vector<double> chapters,double interval):QDialog(parent),chapters_(std::move(chapters)){
        setWindowTitle("Chapter settings");resize(620,480);auto* root=new QVBoxLayout(this);auto* form=new QFormLayout;
        mode_=new QComboBox;mode_->addItem("Use source chapters; if none, every 5 minutes",static_cast<int>(ChapterMode::SourceOrFiveMinute));mode_->addItem("Manual chapter points",static_cast<int>(ChapterMode::Manual));mode_->addItem("New chapter every interval",static_cast<int>(ChapterMode::Interval));mode_->addItem("No additional chapters",static_cast<int>(ChapterMode::None));mode_->setCurrentIndex(mode_->findData(static_cast<int>(mode)));form->addRow("Mode",mode_);
        auto* intervalRow=new QWidget;auto* il=new QHBoxLayout(intervalRow);il->setContentsMargins(0,0,0,0);minutes_=new QSpinBox;minutes_->setRange(0,9999);minutes_->setSuffix(" min");seconds_=new QDoubleSpinBox;seconds_->setRange(0.0,59.999);seconds_->setDecimals(3);seconds_->setSuffix(" sec");const double safe=std::max(0.001,interval);minutes_->setValue(static_cast<int>(safe/60.0));seconds_->setValue(std::fmod(safe,60.0));il->addWidget(minutes_);il->addWidget(seconds_);il->addStretch(1);form->addRow("Interval",intervalRow);root->addLayout(form);
        auto* note=new QLabel("Chapter 1 is always the title start (00:00:00). For Manual mode, enter each additional chapter start on its own line as seconds, MM:SS, or HH:MM:SS. Source mode preserves source chapter starts; when the source has no chapter divisions, BDMV Author creates one every five minutes.");note->setWordWrap(true);root->addWidget(note);
        manual_=new QPlainTextEdit;QStringList lines;for(double c:chapters_)if(c>0.001)lines<<format_time(c);manual_->setPlainText(lines.join('\n'));root->addWidget(manual_,1);
        auto* buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);root->addWidget(buttons);connect(buttons,&QDialogButtonBox::accepted,this,&ChapterSettingsDialog::accept);connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject);connect(mode_,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this]{update_enabled();});update_enabled();
    }
    void accept() override{
        if(mode()==ChapterMode::Manual){std::vector<double> parsed;for(const auto& line:manual_->toPlainText().split('\n')){if(line.trimmed().isEmpty())continue;const auto value=parse_time(line);if(!value){QMessageBox::warning(this,"Chapter settings","Invalid manual chapter time: "+line);return;}if(*value>0.001)parsed.push_back(*value);}std::sort(parsed.begin(),parsed.end());parsed.erase(std::unique(parsed.begin(),parsed.end(),[](double a,double b){return std::abs(a-b)<=0.001;}),parsed.end());chapters_=std::move(parsed);}
        if(mode()==ChapterMode::Interval&&interval_seconds()<=0.0){QMessageBox::warning(this,"Chapter settings","The chapter interval must be greater than zero.");return;}QDialog::accept();
    }
    ChapterMode mode()const{return static_cast<ChapterMode>(mode_->currentData().toInt());}
    double interval_seconds()const{return minutes_->value()*60.0+seconds_->value();}
    const std::vector<double>& chapters()const{return chapters_;}
};

QVariantList navigation_actions_variant(const std::vector<NavigationAction>& actions){QVariantList out;for(const auto& a:actions){QVariantMap m;m["kind"]=static_cast<int>(a.kind);m["title"]=a.target_title;m["chapter"]=a.target_chapter;m["stream"]=a.target_stream;m["menu"]=QString::fromStdString(a.target_menu_id);m["repeat"]=a.repeat_count;out.push_back(m);}return out;}
std::vector<NavigationAction> navigation_actions_from_variant(const QVariant& value){std::vector<NavigationAction> out;for(const auto& v:value.toList()){const auto m=v.toMap();NavigationAction a;a.kind=static_cast<NavigationActionKind>(m.value("kind").toInt());a.target_title=static_cast<std::uint16_t>(m.value("title",1).toUInt());a.target_chapter=static_cast<std::uint16_t>(m.value("chapter",0).toUInt());a.target_stream=static_cast<std::uint16_t>(m.value("stream",1).toUInt());a.target_menu_id=m.value("menu").toString().toStdString();a.repeat_count=static_cast<std::uint16_t>(m.value("repeat",1).toUInt());out.push_back(std::move(a));}return out;}

class ActionSequenceDialog final : public QDialog {
    std::vector<NavigationAction> actions_;
    std::vector<TargetChoice> choices_;
    QListWidget* list_=nullptr;
    QComboBox* add_choice_=nullptr;
    void refresh(){const int previous=list_->currentRow();list_->clear();for(const auto& action:actions_)list_->addItem(action_label(action,choices_));if(!actions_.empty())list_->setCurrentRow(std::clamp(previous,0,static_cast<int>(actions_.size()-1)));}
    void remove_group_markers(std::size_t row){
        if(row>=actions_.size())return;
        if(actions_[row].kind==NavigationActionKind::RepeatBegin){
            auto end=std::find_if(actions_.begin()+static_cast<std::ptrdiff_t>(row+1U),actions_.end(),[](const NavigationAction&a){return a.kind==NavigationActionKind::RepeatEnd;});
            if(end!=actions_.end()){actions_.erase(end);actions_.erase(actions_.begin()+static_cast<std::ptrdiff_t>(row));return;}
        }
        if(actions_[row].kind==NavigationActionKind::RepeatEnd){
            for(std::size_t i=row;i>0;--i)if(actions_[i-1U].kind==NavigationActionKind::RepeatBegin){actions_.erase(actions_.begin()+static_cast<std::ptrdiff_t>(row));actions_.erase(actions_.begin()+static_cast<std::ptrdiff_t>(i-1U));return;}
        }
        actions_.erase(actions_.begin()+static_cast<std::ptrdiff_t>(row));
    }
public:
    ActionSequenceDialog(QWidget* parent,QString title,std::vector<NavigationAction> actions,std::vector<TargetChoice> choices)
        :QDialog(parent),actions_(std::move(actions)),choices_(std::move(choices)){
        setWindowTitle(title);resize(760,470);auto* root=new QVBoxLayout(this);
        auto* note=new QLabel("Actions run from top to bottom. Play-title actions return to the sequence when playback ends. Individual actions may repeat, and a contiguous series can be wrapped in a repeat group. A group may repeat 1–1000 times, or forever when it is the final group and contains at least one Play Title action. Nested groups and menu jumps inside groups are not supported.");note->setWordWrap(true);root->addWidget(note);
        list_=new QListWidget;root->addWidget(list_,1);
        auto* addrow=new QHBoxLayout;add_choice_=new QComboBox;for(const auto& choice:choices_)add_choice_->addItem(choice.label);auto* add=new QPushButton("Add action");addrow->addWidget(add_choice_,1);addrow->addWidget(add);root->addLayout(addrow);
        auto* editrow=new QHBoxLayout;auto* remove=new QPushButton("Remove / ungroup");auto* up=new QPushButton("Move up");auto* down=new QPushButton("Move down");auto* repeat=new QPushButton("Set repeat…");auto* group=new QPushButton("Repeat group…");editrow->addWidget(remove);editrow->addWidget(up);editrow->addWidget(down);editrow->addWidget(repeat);editrow->addWidget(group);editrow->addStretch(1);root->addLayout(editrow);
        auto* buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);root->addWidget(buttons);
        connect(add,&QPushButton::clicked,this,[this]{const int i=add_choice_->currentIndex();if(i<0||static_cast<std::size_t>(i)>=choices_.size())return;actions_.push_back(action_from_choice(choices_[static_cast<std::size_t>(i)]));refresh();list_->setCurrentRow(static_cast<int>(actions_.size()-1));});
        connect(remove,&QPushButton::clicked,this,[this]{const int r=list_->currentRow();if(r<0||static_cast<std::size_t>(r)>=actions_.size())return;remove_group_markers(static_cast<std::size_t>(r));refresh();});
        connect(up,&QPushButton::clicked,this,[this]{const int r=list_->currentRow();if(r<=0||static_cast<std::size_t>(r)>=actions_.size())return;if(navigation_action_is_repeat_marker(actions_[static_cast<std::size_t>(r)].kind)||navigation_action_is_repeat_marker(actions_[static_cast<std::size_t>(r-1)].kind)){QMessageBox::information(this,"Action sequence","Ungroup the actions before moving a repeat-group boundary.");return;}std::swap(actions_[static_cast<std::size_t>(r)],actions_[static_cast<std::size_t>(r-1)]);refresh();list_->setCurrentRow(r-1);});
        connect(down,&QPushButton::clicked,this,[this]{const int r=list_->currentRow();if(r<0||static_cast<std::size_t>(r+1)>=actions_.size())return;if(navigation_action_is_repeat_marker(actions_[static_cast<std::size_t>(r)].kind)||navigation_action_is_repeat_marker(actions_[static_cast<std::size_t>(r+1)].kind)){QMessageBox::information(this,"Action sequence","Ungroup the actions before moving a repeat-group boundary.");return;}std::swap(actions_[static_cast<std::size_t>(r)],actions_[static_cast<std::size_t>(r+1)]);refresh();list_->setCurrentRow(r+1);});
        connect(repeat,&QPushButton::clicked,this,[this]{const int r=list_->currentRow();if(r<0||static_cast<std::size_t>(r)>=actions_.size())return;auto& action=actions_[static_cast<std::size_t>(r)];if(action.kind==NavigationActionKind::RepeatEnd){QMessageBox::information(this,"Repeat","Select the beginning of the repeat group to change its count.");return;}bool ok=false;const QString prompt=action.kind==NavigationActionKind::RepeatBegin?"Repeat group count (0 = forever)":"Repeat count (0 = forever)";const int value=QInputDialog::getInt(this,"Repeat",prompt,action.repeat_count,0,1000,1,&ok);if(!ok)return;action.repeat_count=static_cast<std::uint16_t>(value);refresh();list_->setCurrentRow(r);});
        connect(group,&QPushButton::clicked,this,[this]{
            if(actions_.empty())return;
            bool ok=false;const int first=QInputDialog::getInt(this,"Repeat group","First action number",1,1,static_cast<int>(actions_.size()),1,&ok);if(!ok)return;
            const int last=QInputDialog::getInt(this,"Repeat group","Last action number",first,first,static_cast<int>(actions_.size()),1,&ok);if(!ok)return;
            const int count=QInputDialog::getInt(this,"Repeat group","Group repeat count (0 = forever)",2,0,1000,1,&ok);if(!ok)return;
            const std::size_t begin_index=static_cast<std::size_t>(first-1),end_index=static_cast<std::size_t>(last-1);
            for(std::size_t i=begin_index;i<=end_index;++i)if(navigation_action_is_repeat_marker(actions_[i].kind)){QMessageBox::warning(this,"Repeat group","The selected range already contains a repeat-group boundary. Nested groups are not supported.");return;}
            NavigationAction begin;begin.kind=NavigationActionKind::RepeatBegin;begin.repeat_count=static_cast<std::uint16_t>(count);NavigationAction end;end.kind=NavigationActionKind::RepeatEnd;
            actions_.insert(actions_.begin()+static_cast<std::ptrdiff_t>(end_index+1U),end);
            actions_.insert(actions_.begin()+static_cast<std::ptrdiff_t>(begin_index),begin);
            refresh();list_->setCurrentRow(static_cast<int>(begin_index));
        });
        connect(buttons,&QDialogButtonBox::accepted,this,[this]{
            try{const auto expanded=expand_navigation_repeat_groups(actions_);for(std::size_t i=0;i<expanded.actions.size();++i){const auto&a=expanded.actions[i];if(a.repeat_count==0U&&(a.kind!=NavigationActionKind::PlayTitle||i+1U!=expanded.actions.size()))throw std::runtime_error("Repeat forever on an individual action is allowed only for the final Play Title action.");if(a.kind==NavigationActionKind::Menu&&(i+1U!=expanded.actions.size()||a.repeat_count!=1U))throw std::runtime_error("A menu action must be last and cannot be repeated.");}accept();}
            catch(const std::exception&e){QMessageBox::warning(this,"Action sequence",QString::fromStdString(e.what()));}
        });
        connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject);refresh();
    }
    const std::vector<NavigationAction>& value() const{return actions_;}
};

struct UserOperationUiSpec {UserOperation op;const char* label;const char* formats;};
constexpr std::array<UserOperationUiSpec,40> kUserOperationUi{{
{UserOperation::MenuCall,"Menu call / Root Menu","Blu-ray + DVD"},{UserOperation::TitleSearch,"Title search/play","Blu-ray + DVD"},{UserOperation::ChapterSearch,"Chapter search","Blu-ray + DVD"},{UserOperation::TimeSearch,"Time search/play","Blu-ray + DVD"},{UserOperation::SkipNext,"Next chapter/point","Blu-ray + DVD"},{UserOperation::SkipPrevious,"Previous chapter/point","Blu-ray + DVD"},{UserOperation::Stop,"Stop","Blu-ray + DVD"},{UserOperation::Pause,"Pause","Blu-ray + DVD"},{UserOperation::StillOff,"Still off","Blu-ray + DVD"},{UserOperation::ForwardPlay,"Fast/forward play","Blu-ray + DVD"},{UserOperation::BackwardPlay,"Reverse/backward play","Blu-ray + DVD"},{UserOperation::Resume,"Resume","Blu-ray + DVD"},{UserOperation::MoveUp,"Move selection up","Blu-ray + DVD"},{UserOperation::MoveDown,"Move selection down","Blu-ray + DVD"},{UserOperation::MoveLeft,"Move selection left","Blu-ray + DVD"},{UserOperation::MoveRight,"Move selection right","Blu-ray + DVD"},{UserOperation::SelectButton,"Select button","Blu-ray + DVD"},{UserOperation::ActivateButton,"Activate button","Blu-ray + DVD"},{UserOperation::SelectAndActivate,"Select + activate","Blu-ray + DVD"},{UserOperation::PrimaryAudioChange,"Primary audio/stream change","Blu-ray + DVD"},{UserOperation::AngleChange,"Angle change","Blu-ray + DVD"},{UserOperation::PopupOn,"Popup menu on","Blu-ray"},{UserOperation::PopupOff,"Popup menu off","Blu-ray"},{UserOperation::SubtitleEnableDisable,"Subtitle display on/off","Blu-ray + DVD"},{UserOperation::SubtitleChange,"Subtitle stream change","Blu-ray + DVD"},{UserOperation::SecondaryVideoEnableDisable,"Secondary video on/off","Blu-ray"},{UserOperation::SecondaryVideoChange,"Secondary video change","Blu-ray"},{UserOperation::SecondaryAudioEnableDisable,"Secondary audio on/off","Blu-ray"},{UserOperation::SecondaryAudioChange,"Secondary audio change","Blu-ray"},{UserOperation::PipSubtitleChange,"PiP subtitle change","Blu-ray"},{UserOperation::GoUp,"Go Up","DVD"},{UserOperation::TitleMenuCall,"Title menu call","DVD"},{UserOperation::SubtitleMenuCall,"Subtitle menu call","DVD"},{UserOperation::AudioMenuCall,"Audio menu call","DVD"},{UserOperation::AngleMenuCall,"Angle menu call","DVD"},{UserOperation::ChapterMenuCall,"Chapter menu call","DVD"},{UserOperation::KaraokeAudioMixChange,"Karaoke audio mix change","DVD"},{UserOperation::VideoPresentationModeChange,"Video presentation mode change","DVD"},{UserOperation::TitleOrTimePlay,"Title/time play","DVD"},{UserOperation::ChapterSearchOrPlay,"Chapter search/play","DVD"}}};
class TitleNavigationDialog final : public QDialog {std::vector<NavigationAction> actions_;std::uint64_t prohibited_=0;std::vector<TargetChoice> choices_;QLabel* action_summary_=nullptr;std::vector<std::pair<UserOperation,QCheckBox*>> checks_;bool has_main_menu_=true;void refresh_action_summary(){if(actions_.empty())action_summary_->setText(has_main_menu_?"Default — return to Top/Main Menu":"Default — stop (this project has no main menu)");else action_summary_->setText(action_sequence_summary(actions_,choices_));}
public:TitleNavigationDialog(QWidget* parent,std::vector<NavigationAction> actions,std::uint64_t prohibited,std::vector<TargetChoice> choices,bool has_main_menu):QDialog(parent),actions_(std::move(actions)),prohibited_(prohibited),choices_(std::move(choices)),has_main_menu_(has_main_menu){setWindowTitle("Title navigation and user operations");resize(760,650);auto* root=new QVBoxLayout(this);auto* menu_group=new QGroupBox("Player Menu button");auto* menu_layout=new QVBoxLayout(menu_group);auto* note=new QLabel(has_main_menu_?"By default, pressing the player's Menu/Root Menu button while this title is playing returns to the Top/Main Menu. Set an action sequence to override that title's behavior.":"This project has no visible main menu. The default Menu-button behavior is to stop; a custom action sequence can override it.");note->setWordWrap(true);menu_layout->addWidget(note);auto* action_row=new QHBoxLayout;action_summary_=new QLabel;action_summary_->setWordWrap(true);auto* edit=new QPushButton("Edit action sequence…");auto* reset=new QPushButton("Use default");action_row->addWidget(action_summary_,1);action_row->addWidget(edit);action_row->addWidget(reset);menu_layout->addLayout(action_row);root->addWidget(menu_group);auto* uop_group=new QGroupBox("User Operation Prohibitions");auto* uop_outer=new QVBoxLayout(uop_group);auto* uop_note=new QLabel("Checked operations are prohibited while this title is playing. Nothing is prohibited by default. Labels indicate which disc format(s) support each prohibition.");uop_note->setWordWrap(true);uop_outer->addWidget(uop_note);auto* scroll=new QScrollArea;scroll->setWidgetResizable(true);auto* panel=new QWidget;auto* grid=new QGridLayout(panel);int row=0;for(const auto& spec:kUserOperationUi){auto* box=new QCheckBox(QString::fromUtf8(spec.label));box->setChecked(user_operation_prohibited(prohibited_,spec.op));auto* formats=new QLabel(QString::fromUtf8(spec.formats));formats->setStyleSheet("color: palette(mid);");grid->addWidget(box,row,0);grid->addWidget(formats,row,1);checks_.push_back({spec.op,box});++row;}grid->setColumnStretch(0,1);scroll->setWidget(panel);uop_outer->addWidget(scroll,1);root->addWidget(uop_group,1);auto* buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);root->addWidget(buttons);connect(edit,&QPushButton::clicked,this,[this]{ActionSequenceDialog d(this,"Title Menu-button action sequence",actions_,choices_);if(d.exec()!=QDialog::Accepted)return;actions_=d.value();refresh_action_summary();});connect(reset,&QPushButton::clicked,this,[this]{actions_.clear();refresh_action_summary();});connect(buttons,&QDialogButtonBox::accepted,this,[this]{prohibited_=0;for(const auto& [op,box]:checks_)if(box->isChecked())prohibited_|=user_operation_bit(op);accept();});connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject);refresh_action_summary();}const std::vector<NavigationAction>& actions()const{return actions_;}std::uint64_t prohibited_user_operations()const{return prohibited_;}};

class ButtonPropertiesDialog final : public QDialog {
    MenuButton value_;
    ButtonStyle default_style_;
    std::vector<TargetChoice> targets_;
    QLineEdit* label_=nullptr;
    std::vector<NavigationAction> actions_value_;
    QLabel* actions_summary_=nullptr;
    QPushButton* actions_button_=nullptr;
    QComboBox* kind_=nullptr;
    QCheckBox* custom_style_=nullptr;
    QPushButton* style_button_=nullptr;
    QLineEdit* normal_image_=nullptr;
    QLineEdit* selected_image_=nullptr;
    ColorButton* highlight_=nullptr;
    QWidget* text_panel_=nullptr;
    QWidget* image_panel_=nullptr;
    QSpinBox *x_=nullptr,*y_=nullptr,*w_=nullptr,*h_=nullptr;
public:
    ButtonPropertiesDialog(QWidget* parent,const MenuButton& initial,const ButtonStyle& default_style,
                           std::vector<TargetChoice> targets={},bool lock_target=false)
        :QDialog(parent),value_(initial),default_style_(default_style),targets_(std::move(targets)),actions_value_(button_action_sequence(initial)){
        if(!value_.use_custom_style) value_.style=default_style_;
        setWindowTitle("Menu button properties"); resize(620,520); auto* root=new QVBoxLayout(this); auto* form=new QFormLayout;
        label_=new QLineEdit(QString::fromStdString(initial.label));form->addRow("Label",label_);
        auto* actionrow=new QWidget;auto* actionlayout=new QHBoxLayout(actionrow);actionlayout->setContentsMargins(0,0,0,0);actions_summary_=new QLabel;actions_summary_->setWordWrap(true);actions_button_=new QPushButton("Edit actions…");actionlayout->addWidget(actions_summary_,1);actionlayout->addWidget(actions_button_);form->addRow("Actions",actionrow);
        auto refresh_actions=[this]{actions_summary_->setText(action_sequence_summary(actions_value_,targets_));};
        connect(actions_button_,&QPushButton::clicked,this,[this,refresh_actions]{ActionSequenceDialog d(this,"Button actions",actions_value_,targets_);if(d.exec()!=QDialog::Accepted)return;actions_value_=d.value();refresh_actions();});actions_button_->setEnabled(!lock_target&&!targets_.empty());refresh_actions();
        kind_=new QComboBox;kind_->addItem("Text button",static_cast<int>(MenuButtonKind::Text));kind_->addItem("Image-only button",static_cast<int>(MenuButtonKind::Image));kind_->setCurrentIndex(initial.kind==MenuButtonKind::Image?1:0);form->addRow("Button type",kind_);
        x_=new QSpinBox;x_->setRange(0,kProjectDesignWidth-1);x_->setValue(initial.bounds.x);y_=new QSpinBox;y_->setRange(0,kProjectDesignHeight-1);y_->setValue(initial.bounds.y);w_=new QSpinBox;w_->setRange(16,kProjectDesignWidth);w_->setValue(std::max(16,initial.bounds.width));h_=new QSpinBox;h_->setRange(12,kProjectDesignHeight);h_->setValue(std::max(12,initial.bounds.height));
        auto* pos=new QWidget;auto* pl=new QHBoxLayout(pos);pl->setContentsMargins(0,0,0,0);pl->addWidget(new QLabel("X"));pl->addWidget(x_);pl->addWidget(new QLabel("Y"));pl->addWidget(y_);form->addRow("Position",pos);
        auto* size=new QWidget;auto* sl=new QHBoxLayout(size);sl->setContentsMargins(0,0,0,0);sl->addWidget(new QLabel("Width"));sl->addWidget(w_);sl->addWidget(new QLabel("Height"));sl->addWidget(h_);form->addRow("Button size",size);root->addLayout(form);

        text_panel_=new QGroupBox("Text appearance");auto* tf=new QFormLayout(text_panel_);custom_style_=new QCheckBox("Override menu default style");custom_style_->setChecked(initial.use_custom_style);style_button_=new QPushButton("Edit text style…");tf->addRow(custom_style_);tf->addRow("Style",style_button_);root->addWidget(text_panel_);
        connect(style_button_,&QPushButton::clicked,this,[this]{ButtonStyleDialog d(this,value_.use_custom_style?value_.style:default_style_);if(d.exec()==QDialog::Accepted){value_.style=d.value();custom_style_->setChecked(true);}});

        image_panel_=new QGroupBox("Image button");auto* imf=new QFormLayout(image_panel_);normal_image_=new QLineEdit(QString::fromStdString(initial.normal_image.string()));selected_image_=new QLineEdit(QString::fromStdString(initial.selected_image.string()));highlight_=new ColorButton(initial.image_highlight_color,image_panel_);
        imf->addRow("Unselected image",file_row(image_panel_,normal_image_,"Unselected button image"));imf->addRow("Selected image (optional)",file_row(image_panel_,selected_image_,"Selected button image"));imf->addRow("Automatic highlight",highlight_);auto* note=new QLabel("When no selected image is specified, the unselected image is tinted with this color. Alpha controls highlight strength.");note->setWordWrap(true);imf->addRow(note);root->addWidget(image_panel_);
        auto update_panels=[this]{const bool image=kind_->currentData().toInt()==static_cast<int>(MenuButtonKind::Image);text_panel_->setVisible(!image);image_panel_->setVisible(image);};connect(kind_,&QComboBox::currentIndexChanged,this,[update_panels](int){update_panels();});update_panels();
        auto* buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);root->addWidget(buttons);connect(buttons,&QDialogButtonBox::accepted,this,&QDialog::accept);connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject);
    }
    MenuButton value() const {
        auto b=value_;b.label=label_->text().toStdString();b.kind=static_cast<MenuButtonKind>(kind_->currentData().toInt());b.bounds={x_->value(),y_->value(),w_->value(),h_->value()};b.use_custom_style=custom_style_->isChecked();
        b.normal_image=normal_image_->text().toStdString();b.selected_image=selected_image_->text().toStdString();b.image_highlight_color=highlight_->value();
        set_button_action_sequence(b,actions_value_);
        return b;
    }
};

class ButtonPreviewItem final : public QGraphicsItem {
    std::size_t index_=0;
    MenuButton button_;
    ButtonStyle style_;
    QPixmap normal_;
    QPixmap selected_;
    QRectF rect_;
    bool resizing_=false;
    std::function<void(std::size_t,const Rect&)> changed_;
    QRectF resize_handle() const { return QRectF(rect_.right()-18.0,rect_.bottom()-18.0,18.0,18.0); }
public:
    ButtonPreviewItem(std::size_t index,const MenuButton& button,const ButtonStyle& style,QPixmap normal,QPixmap selected,std::function<void(std::size_t,const Rect&)> changed)
        :index_(index),button_(button),style_(style),normal_(std::move(normal)),selected_(std::move(selected)),rect_(0,0,button.bounds.width,button.bounds.height),changed_(std::move(changed)){
        setPos(button.bounds.x,button.bounds.y);setFlags(ItemIsMovable|ItemIsSelectable|ItemSendsGeometryChanges);setCursor(Qt::OpenHandCursor);setZValue(20.0);
    }
    QRectF boundingRect() const override { return rect_.adjusted(-2,-2,2,2); }
    std::size_t index() const { return index_; }
    void paint(QPainter* p,const QStyleOptionGraphicsItem*,QWidget*) override {
        p->setRenderHint(QPainter::Antialiasing);const bool selected_state=isSelected();
        if(button_.kind==MenuButtonKind::Image){
            QPixmap pix=selected_state&&!selected_.isNull()?selected_:normal_;
            if(!pix.isNull()){
                QPixmap scaled=pix.scaled(rect_.size().toSize(),Qt::KeepAspectRatio,Qt::SmoothTransformation);const QPointF top(rect_.center().x()-scaled.width()/2.0,rect_.center().y()-scaled.height()/2.0);
                if(selected_state&&selected_.isNull()){
                    QPainter hp(&scaled);hp.setCompositionMode(QPainter::CompositionMode_SourceAtop);hp.fillRect(scaled.rect(),qcolor(button_.image_highlight_color));
                }
                p->drawPixmap(top,scaled);
            } else {
                p->setPen(QPen(Qt::red,3));p->drawRect(rect_);p->drawLine(rect_.topLeft(),rect_.bottomRight());p->drawLine(rect_.topRight(),rect_.bottomLeft());
            }
        } else {
            const auto& visual=selected_state?style_.selected:style_.normal;QPainterPath path;path.addRoundedRect(rect_,style_.corner_radius,style_.corner_radius);p->fillPath(path,QBrush(qcolor(visual.background_color)));p->setPen(QPen(qcolor(visual.border_color),visual.border_width));p->drawPath(path);
            QFont font(QString::fromStdString(style_.font_family));font.setPixelSize(style_.font_size_px);font.setBold(style_.bold);font.setItalic(style_.italic);p->setFont(font);p->setPen(qcolor(visual.text_color));p->drawText(rect_,Qt::AlignCenter|Qt::TextWordWrap,QString::fromStdString(button_.label));
        }
        if(isSelected()){p->setPen(QPen(Qt::white,2));p->setBrush(Qt::black);p->drawRect(resize_handle());}
    }
protected:
    QVariant itemChange(GraphicsItemChange change,const QVariant& v) override {
        if(change==ItemPositionChange&&scene()){
            const auto bounds=scene()->sceneRect();QPointF pos=v.toPointF();pos.setX(std::clamp(pos.x(),bounds.left(),bounds.right()-rect_.width()));pos.setY(std::clamp(pos.y(),bounds.top(),bounds.bottom()-rect_.height()));return pos;
        }
        if(change==ItemPositionHasChanged&&changed_){const auto p=pos();changed_(index_,{static_cast<int>(std::lround(p.x())),static_cast<int>(std::lround(p.y())),static_cast<int>(std::lround(rect_.width())),static_cast<int>(std::lround(rect_.height()))});}
        if(change==ItemSelectedHasChanged) update();
        return QGraphicsItem::itemChange(change,v);
    }
    void mousePressEvent(QGraphicsSceneMouseEvent* e) override {
        if(e->button()==Qt::LeftButton&&resize_handle().contains(e->pos())){resizing_=true;setCursor(Qt::SizeFDiagCursor);e->accept();return;}setCursor(Qt::ClosedHandCursor);QGraphicsItem::mousePressEvent(e);
    }
    void mouseMoveEvent(QGraphicsSceneMouseEvent* e) override {
        if(!resizing_){QGraphicsItem::mouseMoveEvent(e);return;}
        const double maxw=scene()?scene()->sceneRect().right()-pos().x():static_cast<double>(kProjectDesignWidth);const double maxh=scene()?scene()->sceneRect().bottom()-pos().y():static_cast<double>(kProjectDesignHeight);const double nw=std::clamp(e->pos().x(),16.0,maxw);const double nh=std::clamp(e->pos().y(),12.0,maxh);prepareGeometryChange();rect_.setSize(QSizeF(nw,nh));update();if(changed_)changed_(index_,{static_cast<int>(std::lround(pos().x())),static_cast<int>(std::lround(pos().y())),static_cast<int>(std::lround(nw)),static_cast<int>(std::lround(nh))});e->accept();
    }
    void mouseReleaseEvent(QGraphicsSceneMouseEvent* e) override {if(resizing_){resizing_=false;setCursor(Qt::OpenHandCursor);e->accept();return;}setCursor(Qt::OpenHandCursor);QGraphicsItem::mouseReleaseEvent(e);}
};

class OverlayPropertiesDialog final : public QDialog {
    MenuOverlay value_;
    QComboBox* kind_=nullptr;
    QLineEdit* text_=nullptr;
    QFontComboBox* font_=nullptr;
    QSpinBox* font_size_=nullptr;
    QCheckBox* bold_=nullptr;
    QCheckBox* italic_=nullptr;
    ColorButton* color_=nullptr;
    QLineEdit* image_=nullptr;
    QWidget* text_panel_=nullptr;
    QWidget* image_panel_=nullptr;
    QSpinBox *x_=nullptr,*y_=nullptr,*w_=nullptr,*h_=nullptr;
public:
    OverlayPropertiesDialog(QWidget* parent,const MenuOverlay& initial):QDialog(parent),value_(initial){
        setWindowTitle("Menu label properties");resize(560,430);auto* root=new QVBoxLayout(this);auto* form=new QFormLayout;
        kind_=new QComboBox;kind_->addItem("Text label",static_cast<int>(MenuOverlayKind::Text));kind_->addItem("Image label",static_cast<int>(MenuOverlayKind::Image));kind_->setCurrentIndex(initial.kind==MenuOverlayKind::Image?1:0);form->addRow("Label type",kind_);
        x_=new QSpinBox;x_->setRange(0,kProjectDesignWidth-1);x_->setValue(initial.bounds.x);y_=new QSpinBox;y_->setRange(0,kProjectDesignHeight-1);y_->setValue(initial.bounds.y);w_=new QSpinBox;w_->setRange(1,kProjectDesignWidth);w_->setValue(std::max(1,initial.bounds.width));h_=new QSpinBox;h_->setRange(1,kProjectDesignHeight);h_->setValue(std::max(1,initial.bounds.height));
        auto* pos=new QWidget;auto* pl=new QHBoxLayout(pos);pl->setContentsMargins(0,0,0,0);pl->addWidget(new QLabel("X"));pl->addWidget(x_);pl->addWidget(new QLabel("Y"));pl->addWidget(y_);form->addRow("Position",pos);
        auto* size=new QWidget;auto* sl=new QHBoxLayout(size);sl->setContentsMargins(0,0,0,0);sl->addWidget(new QLabel("Width"));sl->addWidget(w_);sl->addWidget(new QLabel("Height"));sl->addWidget(h_);form->addRow("Object size",size);root->addLayout(form);
        text_panel_=new QGroupBox("Text label");auto* tf=new QFormLayout(text_panel_);text_=new QLineEdit(QString::fromStdString(initial.text));font_=new QFontComboBox;font_->setCurrentFont(QFont(QString::fromStdString(initial.font_family)));font_size_=new QSpinBox;font_size_->setRange(6,512);font_size_->setSuffix(" px");font_size_->setValue(initial.font_size_px);bold_=new QCheckBox;bold_->setChecked(initial.bold);italic_=new QCheckBox;italic_->setChecked(initial.italic);color_=new ColorButton(initial.text_color,text_panel_);tf->addRow("Text",text_);tf->addRow("Font",font_);tf->addRow("Font size",font_size_);tf->addRow("Bold",bold_);tf->addRow("Italic",italic_);tf->addRow("Color",color_);root->addWidget(text_panel_);
        image_panel_=new QGroupBox("Image label");auto* imf=new QFormLayout(image_panel_);image_=new QLineEdit(QString::fromStdString(initial.image.string()));imf->addRow("Image",file_row(image_panel_,image_,"Menu label image"));auto* note=new QLabel("The image is burned into the menu video, preserves aspect ratio, and never receives focus.");note->setWordWrap(true);imf->addRow(note);root->addWidget(image_panel_);
        auto update=[this]{const bool image=kind_->currentData().toInt()==static_cast<int>(MenuOverlayKind::Image);text_panel_->setVisible(!image);image_panel_->setVisible(image);};connect(kind_,&QComboBox::currentIndexChanged,this,[update](int){update();});update();
        auto* buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);root->addWidget(buttons);connect(buttons,&QDialogButtonBox::accepted,this,&QDialog::accept);connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject);
    }
    MenuOverlay value() const {auto o=value_;o.kind=static_cast<MenuOverlayKind>(kind_->currentData().toInt());o.bounds={x_->value(),y_->value(),w_->value(),h_->value()};o.text=text_->text().toStdString();o.font_family=font_->currentFont().family().toStdString();o.font_size_px=font_size_->value();o.bold=bold_->isChecked();o.italic=italic_->isChecked();o.text_color=color_->value();o.image=image_->text().toStdString();return o;}
};

class OverlayPreviewItem final : public QGraphicsItem {
    std::size_t index_=0;MenuOverlay overlay_;QPixmap image_;QRectF rect_;bool resizing_=false;std::function<void(std::size_t,const Rect&)> changed_;
    QRectF resize_handle() const{return QRectF(rect_.right()-18.0,rect_.bottom()-18.0,18.0,18.0);}
public:
    OverlayPreviewItem(std::size_t index,const MenuOverlay& overlay,QPixmap image,std::function<void(std::size_t,const Rect&)> changed):index_(index),overlay_(overlay),image_(std::move(image)),rect_(0,0,overlay.bounds.width,overlay.bounds.height),changed_(std::move(changed)){setPos(overlay.bounds.x,overlay.bounds.y);setFlags(ItemIsMovable|ItemIsSelectable|ItemSendsGeometryChanges);setCursor(Qt::OpenHandCursor);setZValue(5.0);}
    QRectF boundingRect() const override{return rect_.adjusted(-2,-2,2,2);}std::size_t index()const{return index_;}
    void paint(QPainter* p,const QStyleOptionGraphicsItem*,QWidget*)override{p->setRenderHint(QPainter::Antialiasing);if(overlay_.kind==MenuOverlayKind::Image){if(!image_.isNull()){const auto scaled=image_.scaled(rect_.size().toSize(),Qt::KeepAspectRatio,Qt::SmoothTransformation);p->drawPixmap(QPointF(rect_.center().x()-scaled.width()/2.0,rect_.center().y()-scaled.height()/2.0),scaled);}else{p->setPen(QPen(Qt::red,2));p->drawRect(rect_);p->drawLine(rect_.topLeft(),rect_.bottomRight());}}else{QFont font(QString::fromStdString(overlay_.font_family));font.setPixelSize(overlay_.font_size_px);font.setBold(overlay_.bold);font.setItalic(overlay_.italic);p->setFont(font);p->setPen(qcolor(overlay_.text_color));p->drawText(rect_,Qt::AlignCenter|Qt::TextWordWrap,QString::fromStdString(overlay_.text));}if(isSelected()){p->setPen(QPen(Qt::cyan,2,Qt::DashLine));p->setBrush(Qt::NoBrush);p->drawRect(rect_);p->setBrush(Qt::black);p->drawRect(resize_handle());}}
protected:
    QVariant itemChange(GraphicsItemChange change,const QVariant& v)override{if(change==ItemPositionChange&&scene()){const auto bounds=scene()->sceneRect();QPointF pos=v.toPointF();pos.setX(std::clamp(pos.x(),bounds.left(),bounds.right()-rect_.width()));pos.setY(std::clamp(pos.y(),bounds.top(),bounds.bottom()-rect_.height()));return pos;}if(change==ItemPositionHasChanged&&changed_){const auto p=pos();changed_(index_,{static_cast<int>(std::lround(p.x())),static_cast<int>(std::lround(p.y())),static_cast<int>(std::lround(rect_.width())),static_cast<int>(std::lround(rect_.height()))});}if(change==ItemSelectedHasChanged)update();return QGraphicsItem::itemChange(change,v);}
    void mousePressEvent(QGraphicsSceneMouseEvent* e)override{if(e->button()==Qt::LeftButton&&resize_handle().contains(e->pos())){resizing_=true;setCursor(Qt::SizeFDiagCursor);e->accept();return;}setCursor(Qt::ClosedHandCursor);QGraphicsItem::mousePressEvent(e);}
    void mouseMoveEvent(QGraphicsSceneMouseEvent* e)override{if(!resizing_){QGraphicsItem::mouseMoveEvent(e);return;}const double maxw=scene()?scene()->sceneRect().right()-pos().x():static_cast<double>(kProjectDesignWidth),maxh=scene()?scene()->sceneRect().bottom()-pos().y():static_cast<double>(kProjectDesignHeight);const double nw=std::clamp(e->pos().x(),1.0,maxw),nh=std::clamp(e->pos().y(),1.0,maxh);prepareGeometryChange();rect_.setSize(QSizeF(nw,nh));update();if(changed_)changed_(index_,{static_cast<int>(std::lround(pos().x())),static_cast<int>(std::lround(pos().y())),static_cast<int>(std::lround(nw)),static_cast<int>(std::lround(nh))});e->accept();}
    void mouseReleaseEvent(QGraphicsSceneMouseEvent* e)override{if(resizing_){resizing_=false;setCursor(Qt::OpenHandCursor);e->accept();return;}setCursor(Qt::OpenHandCursor);QGraphicsItem::mouseReleaseEvent(e);}
};

class MenuCanvasHost final : public QWidget {
    QGraphicsScene* scene_=nullptr;
    QGraphicsView* view_=nullptr;
public:
    explicit MenuCanvasHost(QGraphicsScene* scene,QWidget* parent=nullptr):QWidget(parent),scene_(scene){
        setMinimumHeight(320);setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Expanding);
        setStyleSheet("background:#303038;");
        view_=new QGraphicsView(scene,this);view_->setRenderHint(QPainter::Antialiasing);
        view_->setFrameShape(QFrame::NoFrame);view_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);view_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        view_->setAlignment(Qt::AlignCenter);view_->setStyleSheet("background:black;");
    }
    QGraphicsView* view() const { return view_; }
    void refit(){
        const auto rect=scene_?scene_->sceneRect():QRectF(0,0,kProjectDesignWidth,kProjectDesignHeight);const double aspect=rect.height()>0.0?rect.width()/rect.height():16.0/9.0;
        int vw=width(),vh=static_cast<int>(std::lround(static_cast<double>(vw)/aspect));
        if(vh>height()){vh=height();vw=static_cast<int>(std::lround(static_cast<double>(vh)*aspect));}
        view_->setGeometry((width()-vw)/2,(height()-vh)/2,vw,vh);view_->fitInView(rect,Qt::KeepAspectRatio);
    }
protected:
    void resizeEvent(QResizeEvent* e) override {QWidget::resizeEvent(e);refit();}
};
}

struct BuildResult {
    QString error;
    bool cancelled = false;
};

class Window : public QMainWindow {
    static constexpr std::size_t BackIndex = std::numeric_limits<std::size_t>::max();
    QTableWidget* titles_=nullptr;
    QTreeWidget* menu_tree_=nullptr;
    QListWidget* button_list_=nullptr;
    QListWidget* overlay_list_=nullptr;
    QGraphicsScene* scene_=nullptr;QGraphicsView* view_=nullptr;MenuCanvasHost* canvas_host_=nullptr;
    QLineEdit* volume_=nullptr;QLineEdit* output_=nullptr;QLineEdit* menu_name_=nullptr;QLineEdit* bg_=nullptr;QComboBox* bg_type_=nullptr;QLineEdit* menu_audio_path_=nullptr;
    ColorButton* bg_color_=nullptr;
    QCheckBox *inherit_bg_=nullptr,*inherit_bg_color_=nullptr,*inherit_style_=nullptr,*inherit_audio_=nullptr,*inherit_duration_=nullptr,*inherit_encoding_=nullptr,*loop_media_=nullptr,*auto_back_=nullptr;
    QPushButton *default_style_button_=nullptr,*edit_back_button_=nullptr,*add_menu_button_=nullptr,*remove_menu_button_=nullptr;
    QDoubleSpinBox* duration_=nullptr;
    QComboBox* menu_video_=nullptr;QSpinBox* menu_bitrate_=nullptr;QSpinBox* menu_minrate_=nullptr;QSpinBox* menu_maxrate_=nullptr;QComboBox* menu_preset_=nullptr;QCheckBox* menu_two_pass_=nullptr;QSpinBox* menu_keyframe_=nullptr;QComboBox* menu_audio_codec_=nullptr;QSpinBox* menu_audio_bitrate_=nullptr;QPushButton* menu_advanced_=nullptr;
    QProgressBar* progress_=nullptr;QPushButton* build_=nullptr;QPushButton* cancel_=nullptr;QComboBox* size_target_=nullptr;QCheckBox* automatic_bitrate_=nullptr;QComboBox* target_=nullptr;QComboBox* frame_rate_=nullptr;QComboBox* menu_resolution_=nullptr;QComboBox* menu_aspect_=nullptr;
    bool cache_enabled_state_=true;
    bool compliance_cache_enabled_state_=true;
    bool refresh_encode_cache_state_=false;
    bool refresh_compliance_cache_state_=false;
    std::shared_ptr<std::atomic_bool> cancel_requested_;
    bool close_after_render_=false;
    std::unique_ptr<QFutureWatcher<BuildResult>> watcher_;
    std::vector<NavigationAction> first_play_actions_;
    Menu root_menu_;
    std::string current_menu_id_="top";
    int next_menu_id_=1;
    bool loading_menu_=false;
    bool debug_mode_=false;
    AuthoringLimits authoring_limits_{};
    bool project_modified_=false;
    bool suppress_modified_=true;
    DiscTarget displayed_target_=DiscTarget::BluRay1080;
    std::string menu_hd_resolution_="1920x1080",menu_uhd_resolution_="1920x1080",menu_dvd_resolution_="720x480";
    std::string menu_hd_aspect_="16:9",menu_uhd_aspect_="16:9",menu_dvd_aspect_="16:9";
    struct MenuBoundsSnapshot {std::vector<Rect> buttons;std::vector<Rect> overlays;Rect back;std::vector<MenuBoundsSnapshot> submenus;};
    std::optional<MenuBoundsSnapshot> wide_bounds_snapshot_;
    QString project_file_;
    NewProjectDefaults saved_defaults_=load_new_project_defaults();
    NewProjectDefaults project_defaults_=saved_defaults_;
    static constexpr int RoleAudioLanguage = Qt::UserRole + 20;
    static constexpr int RoleChapters = Qt::UserRole + 21;
    static constexpr int RoleAc3Bitrate = Qt::UserRole + 22;
    static constexpr int RoleDcaBitrate = Qt::UserRole + 23;
    static constexpr int RoleHdProfile = Qt::UserRole + 24;
    static constexpr int RoleUhdProfile = Qt::UserRole + 25;
    static constexpr int RoleDvdProfile = Qt::UserRole + 26;
    static constexpr int RoleChapterMode = Qt::UserRole + 27;
    static constexpr int RoleChapterInterval = Qt::UserRole + 28;
    static constexpr int RoleHdFrameRate = Qt::UserRole + 29;
    static constexpr int RoleUhdFrameRate = Qt::UserRole + 30;
    static constexpr int RoleDvdFrameRate = Qt::UserRole + 31;
    static constexpr int RoleHdResolution = Qt::UserRole + 32;
    static constexpr int RoleUhdResolution = Qt::UserRole + 33;
    static constexpr int RoleDvdResolution = Qt::UserRole + 34;
    static constexpr int RoleHdAspect = Qt::UserRole + 35;
    static constexpr int RoleUhdAspect = Qt::UserRole + 36;
    static constexpr int RoleDvdAspect = Qt::UserRole + 37;
    static constexpr int RoleTitleMenuActions = Qt::UserRole + 38;
    static constexpr int RoleTitleUopMask = Qt::UserRole + 39;
    static constexpr int RoleAudioStreamSettings = Qt::UserRole + 40;
    static constexpr int RoleSubtitleStreamSettings = Qt::UserRole + 41;
    static constexpr int RoleExternalSubtitles = Qt::UserRole + 42;
    static constexpr int RoleSubtitleDefaultStyle = Qt::UserRole + 43;
    static constexpr int RoleDefaultAudioStream = Qt::UserRole + 44;
    static constexpr int RoleDefaultSubtitleStream = Qt::UserRole + 45;
    static int profile_role(DiscTarget target){switch(target){case DiscTarget::UltraHdBluRay2160:return RoleUhdProfile;case DiscTarget::DvdVideo480p:return RoleDvdProfile;case DiscTarget::BluRay1080:return RoleHdProfile;}return RoleHdProfile;}
    static int frame_rate_role(DiscTarget target){switch(target){case DiscTarget::UltraHdBluRay2160:return RoleUhdFrameRate;case DiscTarget::DvdVideo480p:return RoleDvdFrameRate;case DiscTarget::BluRay1080:return RoleHdFrameRate;}return RoleHdFrameRate;}
    static int resolution_role(DiscTarget target){switch(target){case DiscTarget::UltraHdBluRay2160:return RoleUhdResolution;case DiscTarget::DvdVideo480p:return RoleDvdResolution;case DiscTarget::BluRay1080:return RoleHdResolution;}return RoleHdResolution;}
    static int aspect_role(DiscTarget target){switch(target){case DiscTarget::UltraHdBluRay2160:return RoleUhdAspect;case DiscTarget::DvdVideo480p:return RoleDvdAspect;case DiscTarget::BluRay1080:return RoleHdAspect;}return RoleHdAspect;}
    static EncodingProfile default_profile_for_target(DiscTarget target){switch(target){case DiscTarget::UltraHdBluRay2160:return default_uhd_encoding_profile();case DiscTarget::DvdVideo480p:return default_dvd_encoding_profile();case DiscTarget::BluRay1080:return EncodingProfile{};}return EncodingProfile{};}
    static QString output_target_name(DiscTarget target){switch(target){case DiscTarget::DvdVideo480p:return QStringLiteral("DVD");case DiscTarget::UltraHdBluRay2160:return QStringLiteral("Ultra HD Blu-ray");case DiscTarget::BluRay1080:return QStringLiteral("Blu-ray");}return QStringLiteral("Blu-ray");}
    std::string& stored_menu_resolution(DiscTarget target){switch(target){case DiscTarget::UltraHdBluRay2160:return menu_uhd_resolution_;case DiscTarget::DvdVideo480p:return menu_dvd_resolution_;case DiscTarget::BluRay1080:return menu_hd_resolution_;}return menu_hd_resolution_;}
    const std::string& stored_menu_resolution(DiscTarget target)const{return const_cast<Window*>(this)->stored_menu_resolution(target);}
    std::string& stored_menu_aspect(DiscTarget target){switch(target){case DiscTarget::UltraHdBluRay2160:return menu_uhd_aspect_;case DiscTarget::DvdVideo480p:return menu_dvd_aspect_;case DiscTarget::BluRay1080:return menu_hd_aspect_;}return menu_hd_aspect_;}
    const std::string& stored_menu_aspect(DiscTarget target)const{return const_cast<Window*>(this)->stored_menu_aspect(target);}
    void load_menu_mode_state(const Project& p){menu_hd_resolution_=p.menu_resolution;menu_uhd_resolution_=p.menu_uhd_resolution;menu_dvd_resolution_=p.menu_dvd_resolution;menu_hd_aspect_=p.menu_aspect_ratio;menu_uhd_aspect_=p.menu_uhd_aspect_ratio;menu_dvd_aspect_=p.menu_dvd_aspect_ratio;}
    void store_visible_menu_mode(){if(!menu_resolution_||!menu_aspect_)return;stored_menu_resolution(displayed_target_)=menu_resolution_->currentData().toString().toStdString();stored_menu_aspect(displayed_target_)=menu_aspect_->currentData().toString().toStdString();}
    static bool four_three(const std::string& aspect){return aspect=="4:3";}
    static QRectF design_aperture(const std::string& aspect){if(four_three(aspect)){constexpr int width=kProjectDesignHeight*4/3;constexpr int x=(kProjectDesignWidth-width)/2;return QRectF(x,0,width,kProjectDesignHeight);}return QRectF(0,0,kProjectDesignWidth,kProjectDesignHeight);}
    static bool same_rect(const Rect& a,const Rect& b){return a.x==b.x&&a.y==b.y&&a.width==b.width&&a.height==b.height;}
    static Rect clamp_rect_to_aperture(Rect r,const std::string& aspect){const auto a=design_aperture(aspect);const int left=static_cast<int>(a.left()),top=static_cast<int>(a.top()),width=static_cast<int>(a.width()),height=static_cast<int>(a.height());r.width=std::clamp(r.width,1,width);r.height=std::clamp(r.height,1,height);r.x=std::clamp(r.x,left,left+width-r.width);r.y=std::clamp(r.y,top,top+height-r.height);return r;}
    static MenuBoundsSnapshot capture_menu_bounds(const Menu& m){MenuBoundsSnapshot s;for(const auto& b:m.buttons)s.buttons.push_back(b.bounds);for(const auto& o:m.overlays)s.overlays.push_back(o.bounds);s.back=m.back_button.bounds;for(const auto& c:m.submenus)s.submenus.push_back(capture_menu_bounds(c));return s;}
    static void restore_menu_bounds(Menu& m,const MenuBoundsSnapshot& s){for(std::size_t i=0;i<std::min(m.buttons.size(),s.buttons.size());++i)m.buttons[i].bounds=s.buttons[i];for(std::size_t i=0;i<std::min(m.overlays.size(),s.overlays.size());++i)m.overlays[i].bounds=s.overlays[i];m.back_button.bounds=s.back;for(std::size_t i=0;i<std::min(m.submenus.size(),s.submenus.size());++i)restore_menu_bounds(m.submenus[i],s.submenus[i]);}
    static void clamp_menu_tree(Menu& m,const std::string& aspect){for(auto& b:m.buttons)b.bounds=clamp_rect_to_aperture(b.bounds,aspect);for(auto& o:m.overlays)o.bounds=clamp_rect_to_aperture(o.bounds,aspect);m.back_button.bounds=clamp_rect_to_aperture(m.back_button.bounds,aspect);for(auto& c:m.submenus)clamp_menu_tree(c,aspect);}
    void invalidate_reversible_aspect_positions(){wide_bounds_snapshot_.reset();}
    void apply_editor_aspect_transition(const std::string& previous,const std::string& next){const bool was=four_three(previous),now=four_three(next);if(was==now){refresh_canvas();return;}if(now){if(!wide_bounds_snapshot_)wide_bounds_snapshot_=capture_menu_bounds(root_menu_);clamp_menu_tree(root_menu_,next);}else if(wide_bounds_snapshot_){restore_menu_bounds(root_menu_,*wide_bounds_snapshot_);wide_bounds_snapshot_.reset();}refresh_canvas();}
    void initialize_editor_bounds_for_loaded_project(){wide_bounds_snapshot_.reset();if(four_three(stored_menu_aspect(displayed_target_)))clamp_menu_tree(root_menu_,stored_menu_aspect(displayed_target_));}
    void update_build_button_text(){if(build_)build_->setText(QStringLiteral("Build %1 image").arg(output_target_name(displayed_target_)));}
    void update_window_title(){
        const QString name=project_file_.isEmpty()?QStringLiteral("Untitled project"):QFileInfo(project_file_).fileName();
        setWindowTitle(QStringLiteral("%1[*]").arg(name));
        setWindowModified(project_modified_);
    }
    void mark_modified(){if(suppress_modified_)return;project_modified_=true;update_window_title();}
    void mark_clean(){project_modified_=false;update_window_title();}
    void watch_editor(QWidget* widget){
        if(widget->property("bdmvauthorBuildOnly").toBool())return;
        if(auto* e=qobject_cast<QLineEdit*>(widget))connect(e,&QLineEdit::textEdited,this,[this](const QString&){mark_modified();});
        if(auto* c=qobject_cast<QComboBox*>(widget))connect(c,QOverload<int>::of(&QComboBox::activated),this,[this](int){mark_modified();});
        if(auto* c=qobject_cast<QCheckBox*>(widget))connect(c,&QCheckBox::clicked,this,[this](bool){mark_modified();});
        if(auto* spin=qobject_cast<QAbstractSpinBox*>(widget))connect(spin,&QAbstractSpinBox::editingFinished,this,[this]{mark_modified();});
    }
    bool maybe_save_modified(){
        if(!project_modified_)return true;
        const auto answer=QMessageBox::warning(this,"Unsaved changes",
            "The current project has unsaved changes. Do you want to save them?",
            QMessageBox::Save|QMessageBox::Discard|QMessageBox::Cancel,QMessageBox::Save);
        if(answer==QMessageBox::Cancel)return false;
        if(answer==QMessageBox::Discard)return true;
        return save_project(false);
    }
    void show_help_topic(const QString& title,const QString& text){
        QDialog d(this);d.setWindowTitle(QStringLiteral("BDMV Author Help — %1").arg(title));d.resize(760,560);
        auto* layout=new QVBoxLayout(&d);auto* body=new QPlainTextEdit(text,&d);body->setReadOnly(true);body->setLineWrapMode(QPlainTextEdit::WidgetWidth);layout->addWidget(body,1);
        auto* buttons=new QDialogButtonBox(QDialogButtonBox::Close,&d);connect(buttons,&QDialogButtonBox::rejected,&d,&QDialog::reject);layout->addWidget(buttons);d.exec();
    }
    void show_about(){QMessageBox box(this);box.setWindowTitle("About BDMV Author");box.setTextFormat(Qt::RichText);box.setText(QStringLiteral("<b>BDMV Author %1</b><br><br>Java-free Blu-ray, Ultra HD Blu-ray, and DVD-Video authoring with optional native menus.<br><br>Licensed under Apache-2.0.").arg(QString::fromLatin1(BDMVAUTHOR_VERSION)));box.setIconPixmap(bdmvauthor_app_icon().pixmap(128,128));box.setStandardButtons(QMessageBox::Ok);box.exec();}
    void refresh_debug_rate_controls(){
        const bool allow=authoring_limits_.allow_exceeding_format_limits;
        if(menu_video_&&menu_bitrate_&&menu_minrate_&&menu_maxrate_){const auto codec=static_cast<VideoCodec>(menu_video_->currentData().toInt());configure_video_bitrate_spin(menu_bitrate_,displayed_target_,codec,menu_bitrate_->value(),allow);configure_video_minrate_spin(menu_minrate_,displayed_target_,codec,menu_minrate_->value(),allow);configure_video_maxrate_spin(menu_maxrate_,displayed_target_,codec,menu_maxrate_->value(),allow);}
        if(menu_audio_codec_&&menu_audio_bitrate_){const auto codec=static_cast<AudioCodec>(menu_audio_codec_->currentData().toInt());configure_audio_bitrate_spin(menu_audio_bitrate_,displayed_target_,codec,menu_audio_bitrate_->value(),allow);}
        if(!titles_)return;
        for(int r=0;r<titles_->rowCount();++r){auto* video=qobject_cast<QComboBox*>(titles_->cellWidget(r,2));auto* bitrate=qobject_cast<QSpinBox*>(titles_->cellWidget(r,3));auto* audio=qobject_cast<QComboBox*>(titles_->cellWidget(r,6));auto* audio_bitrate=qobject_cast<QSpinBox*>(titles_->cellWidget(r,7));auto* minrate=qobject_cast<QSpinBox*>(titles_->cellWidget(r,17));auto* maxrate=qobject_cast<QSpinBox*>(titles_->cellWidget(r,18));if(video&&bitrate&&minrate&&maxrate){const auto codec=static_cast<VideoCodec>(video->currentData().toInt());configure_video_bitrate_spin(bitrate,displayed_target_,codec,bitrate->value(),allow);configure_video_minrate_spin(minrate,displayed_target_,codec,minrate->value(),allow);configure_video_maxrate_spin(maxrate,displayed_target_,codec,maxrate->value(),allow);}if(audio&&audio_bitrate)configure_audio_bitrate_spin(audio_bitrate,displayed_target_,static_cast<AudioCodec>(audio->currentData().toInt()),audio_bitrate->value(),allow);}
    }
    void edit_debug_transport_limits(){
        QDialog d(this);d.setWindowTitle("Debug Settings — Transport bitrate limits");
        auto* layout=new QVBoxLayout(&d);
        auto* note=new QLabel("These debug overrides apply only to this --debug session. They are not saved to application settings or project files.",&d);
        note->setWordWrap(true);layout->addWidget(note);
        auto* form=new QFormLayout;
        auto* bd=new QSpinBox(&d);bd->setRange(1000,1000000);bd->setSuffix(" kb/s");bd->setSingleStep(1000);bd->setValue(authoring_limits_.bluray_transport_bitrate_kbps);
        auto* uhd=new QSpinBox(&d);uhd->setRange(1000,1000000);uhd->setSuffix(" kb/s");uhd->setSingleStep(1000);uhd->setValue(authoring_limits_.uhd_transport_bitrate_kbps);
        auto* dvd=new QSpinBox(&d);dvd->setRange(1000,1000000);dvd->setSuffix(" kb/s");dvd->setSingleStep(1000);dvd->setValue(authoring_limits_.dvd_transport_bitrate_kbps);
        form->addRow("Blu-ray combined transport limit",bd);form->addRow("UHD Blu-ray combined transport limit",uhd);form->addRow("DVD program-stream mux limit",dvd);layout->addLayout(form);
        auto* buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&d);connect(buttons,&QDialogButtonBox::accepted,&d,&QDialog::accept);connect(buttons,&QDialogButtonBox::rejected,&d,&QDialog::reject);layout->addWidget(buttons);
        if(d.exec()!=QDialog::Accepted)return;
        authoring_limits_.bluray_transport_bitrate_kbps=bd->value();
        authoring_limits_.uhd_transport_bitrate_kbps=uhd->value();
        authoring_limits_.dvd_transport_bitrate_kbps=dvd->value();
        statusBar()->showMessage(QString("Debug transport limits for this session: Blu-ray %1 kb/s, UHD %2 kb/s, DVD %3 kb/s%4").arg(bd->value()).arg(uhd->value()).arg(dvd->value()).arg(authoring_limits_.allow_exceeding_format_limits?QStringLiteral(" (over-limit values enabled)"):QStringLiteral(" (standard ceilings still enforced)")),7000);
    }
public:
    explicit Window(bool debug_mode=false):debug_mode_(debug_mode){
        Project initial_project;apply_new_project_defaults(initial_project,project_defaults_);root_menu_=initial_project.menu;root_menu_.id="top";root_menu_.name="Top Menu";
        root_menu_.inherit_background_image=false;root_menu_.inherit_background_color=false;root_menu_.inherit_button_style=false;root_menu_.inherit_encoding=false;root_menu_.inherit_audio=false;root_menu_.inherit_duration=false;
        resize(1780,980);
        auto* fileMenu=menuBar()->addMenu("&File");
        auto* newProject=fileMenu->addAction("&New project");
        auto* openProject=fileMenu->addAction("&Open project…");
        auto* saveProject=fileMenu->addAction("&Save project");
        auto* saveProjectAs=fileMenu->addAction("Save project &as…");
        fileMenu->addSeparator();auto* quitAction=fileMenu->addAction("&Quit");
        auto* settingsMenu=menuBar()->addMenu("&Settings");auto* defaultsAction=settingsMenu->addAction("Defaults for &new projects…");auto* toolsAction=settingsMenu->addAction("External &tools and encoders…");auto* cacheManagerAction=settingsMenu->addAction("&Cache settings…");
        QAction* debugLimitsAction=nullptr;QAction* debugAllowExceedAction=nullptr;
        if(debug_mode_){auto* debugMenu=menuBar()->addMenu("&Debug");debugAllowExceedAction=debugMenu->addAction("Allow exceeding format limits");debugAllowExceedAction->setCheckable(true);debugAllowExceedAction->setChecked(false);debugAllowExceedAction->setToolTip("Session-only: allow audio/video bitrate and mux/transport settings above disc-format ceilings. This can create noncompliant or unplayable output.");debugMenu->addSeparator();debugLimitsAction=debugMenu->addAction("Transport bitrate &limits…");}
        auto* helpMenu=menuBar()->addMenu("&Help");auto* helpTopics=helpMenu->addMenu("&Help Topics");
        auto* gettingStarted=helpTopics->addAction("&Getting Started");auto* titleHelp=helpTopics->addAction("&Titles and Video Modes");auto* chapterHelp=helpTopics->addAction("&Chapters and Navigation");auto* menuHelp=helpTopics->addAction("&Menus and Buttons");auto* buildHelp=helpTopics->addAction("&Building Disc Images");
        helpMenu->addSeparator();auto* aboutAction=helpMenu->addAction("&About BDMV Author");
        newProject->setShortcut(QKeySequence::New);saveProject->setShortcut(QKeySequence::Save);openProject->setShortcut(QKeySequence::Open);quitAction->setShortcut(QKeySequence::Quit);
        connect(newProject,&QAction::triggered,this,[this]{new_project();});
        connect(defaultsAction,&QAction::triggered,this,[this]{edit_new_project_defaults();});
        connect(toolsAction,&QAction::triggered,this,[this]{edit_tool_settings();});connect(cacheManagerAction,&QAction::triggered,this,[this]{edit_cache_settings();});
        if(debugAllowExceedAction)connect(debugAllowExceedAction,&QAction::toggled,this,[this](bool enabled){authoring_limits_.allow_exceeding_format_limits=enabled;refresh_debug_rate_controls();statusBar()->showMessage(enabled?"DEBUG: format bitrate/transport ceilings are disabled for this session. Output may be noncompliant.":"Debug format-limit override disabled; standard bitrate/transport ceilings restored.",7000);});
        if(debugLimitsAction)connect(debugLimitsAction,&QAction::triggered,this,[this]{edit_debug_transport_limits();});
        connect(openProject,&QAction::triggered,this,[this]{open_project();});
        connect(saveProject,&QAction::triggered,this,[this]{save_project(false);});
        connect(saveProjectAs,&QAction::triggered,this,[this]{save_project(true);});
        connect(quitAction,&QAction::triggered,this,&QWidget::close);
        connect(gettingStarted,&QAction::triggered,this,[this]{show_help_topic("Getting Started",
            "1. Add one or more video titles with Add video. The Add Video dialog can create a menu button for each title automatically; this is enabled by default.\n2. Choose Blu-ray, Ultra HD Blu-ray, or DVD-Video as the output target.\n3. Configure the menu and navigation. To make a menu-less disc, select the main menu and choose Remove menu.\n4. Choose a disc-image path and select Build.\n\nWith no main menu and no custom startup sequence, all titles play in order once and playback stops at the end. Startup sequence… can override that behavior. Project files store authoring choices but reference source media by path, so keep the source files available when reopening a project.");});
        connect(titleHelp,&QAction::triggered,this,[this]{show_help_topic("Titles and Video Modes",
            "Each title has independent codec, bitrate, frame-rate, resolution, aspect-ratio, and GOP/keyframe settings for each output target. Auto resolution chooses the closest legal mode to the source and prefers upscaling when two choices are equally close.\n\nStandard Blu-ray supports its legal HD and SD modes. Ultra HD Blu-ray HEVC supports 3840×2160 and 1920×1080, while UHD AVC/H.264 is limited to 1920×1080p23.976/24, 8-bit BT.709 SDR, at up to 40 Mb/s. Compliant AVC bonus features can be remuxed without re-encoding. DVD supports NTSC/Film and PAL raster families; PAL cannot be mixed with the NTSC/Film family on one disc.");});
        connect(chapterHelp,&QAction::triggered,this,[this]{show_help_topic("Chapters and Navigation",
            "Per-title chapter policy can use source chapters with a five-minute fallback, manual chapter points, a fixed interval, or no additional chapters. Chapter 1 is always the title start. Menu buttons and startup actions may target a whole title or a specific chapter.\n\nEach title's Navigation… button controls what happens when the player Menu/Root Menu button is pressed during that title. The default is to return to Top/Main Menu; on a menu-less project the default is to stop. Edit action sequence… can replace that default with an ordered navigation sequence. The same dialog controls User Operation Prohibitions. Nothing is prohibited by default; checked operations are blocked while the title plays. If Menu Call is prohibited, the Menu button itself cannot invoke the configured Menu-button action.\n\nIn Edit actions… or Startup sequence…, Set repeat… assigns a repeat count to one action. Repeat group… wraps a contiguous action range so the entire series repeats together; finite groups may run 1–1000 times, and a final group containing at least one Play Title action may repeat forever. Nested repeat groups and menu jumps inside repeat groups are not supported.");});
        connect(menuHelp,&QAction::triggered,this,[this]{show_help_topic("Menus and Buttons",
            "The 3840×2160 design canvas is shared by all targets. Menus may contain title/chapter buttons, audio/subtitle controls, links to submenus, text labels, and images. Menu video resolution and aspect are selected separately from title modes; 4:3 is available where the disc standard permits it. UHD menus default to 1920×1080 as a compatibility workaround for a VLC UHD Blu-ray mouse-coordinate bug; 3840×2160 remains available for other players, and BDMV Author warns when it is selected. New-project default menu resolutions and disc-size targets for Blu-ray, UHD, and DVD are independently configurable in Settings → Defaults for new projects.\n\nMenu duration is automatic whenever motion video or menu audio is present. With both, the longer source determines the duration and the shorter source loops. A separate Menu audio file overrides audio embedded in the background video; otherwise the video's first audio stream is used. Duration remains manually adjustable for still-image menus with no audio. Loop menu audio/video repeats the media continuously; turn it off to play the media once and leave the menu on its final frame.\n\nThe main menu itself may be removed. A menu-less disc defaults to playing every title in order once and then stopping, while Startup sequence… remains fully editable. Add video normally creates a menu button automatically; clear its checkbox when a title should not receive one.");});
        connect(buildHelp,&QAction::triggered,this,[this]{show_help_topic("Building Disc Images",
            "The Build button creates an image for the selected target. Blu-ray and Ultra HD Blu-ray use the BDMV authoring path and UDF image writer; DVD-Video uses dvdauthor/spumux and mkisofs DVD-Video output. Size target selects the intended image/disc capacity; Automatic video bitrate is enabled by default and chooses the highest video bitrate that fits after audio, subtitles, muxing/filesystem overhead, and a safety margin. Unlimited removes the capacity limit but not disc-format bitrate limits. Manual bitrate mode is rejected before encoding if the configured project cannot fit. Encoding progress appears in the progress bar, and Cancel stops active helper processes and removes temporary render files. Cache controls and the cache manager are under Settings → Cache settings…. Save the .bdmvproject separately if you want to edit the authoring project later.");});
        connect(aboutAction,&QAction::triggered,this,[this]{show_about();});
        auto* c=new QWidget;setCentralWidget(c);auto* root=new QVBoxLayout(c);
        auto* project=new QHBoxLayout;volume_=new QLineEdit(QString::fromStdString(project_defaults_.volume_label));output_=new QLineEdit;output_->setPlaceholderText("Choose a disc image file…");target_=new QComboBox;target_->addItem("Blu-ray",static_cast<int>(DiscTarget::BluRay1080));target_->addItem("Ultra HD Blu-ray",static_cast<int>(DiscTarget::UltraHdBluRay2160));target_->addItem("DVD-Video",static_cast<int>(DiscTarget::DvdVideo480p));load_menu_mode_state(initial_project);displayed_target_=initial_project.target;target_->setCurrentIndex(target_->findData(static_cast<int>(displayed_target_)));frame_rate_=new QComboBox;populate_frame_rate_choices(initial_project.frame_rate);menu_resolution_=new QComboBox;populate_resolution_choices(menu_resolution_,displayed_target_,stored_menu_resolution(displayed_target_),true);menu_aspect_=new QComboBox;populate_aspect_choices(menu_aspect_,displayed_target_,stored_menu_aspect(displayed_target_),false,stored_menu_resolution(displayed_target_));auto* outBrowse=new QPushButton("Browse…");auto* startupSequence=new QPushButton("Startup sequence…");project->addWidget(new QLabel("Output target"));project->addWidget(target_);project->addWidget(new QLabel("Menu resolution"));project->addWidget(menu_resolution_);project->addWidget(new QLabel("Menu aspect"));project->addWidget(menu_aspect_);project->addWidget(new QLabel("Menu/default frame rate"));project->addWidget(frame_rate_);project->addWidget(startupSequence);project->addSpacing(12);project->addWidget(new QLabel("UDF Disc label"));project->addWidget(volume_);project->addSpacing(12);project->addWidget(new QLabel("Disc image"));project->addWidget(output_,1);project->addWidget(outBrowse);root->addLayout(project);
        connect(outBrowse,&QPushButton::clicked,this,[this]{choose_output_image();});
        connect(menu_resolution_,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this](int){const auto previous_aspect=stored_menu_aspect(displayed_target_);stored_menu_resolution(displayed_target_)=menu_resolution_->currentData().toString().toStdString();if(displayed_target_==DiscTarget::UltraHdBluRay2160&&stored_menu_resolution(displayed_target_)=="3840x2160")warn_uhd_4k_menu_vlc_mouse_bug(this);const auto keep=menu_aspect_->currentData().toString().toStdString();populate_aspect_choices(menu_aspect_,displayed_target_,keep,false,stored_menu_resolution(displayed_target_));stored_menu_aspect(displayed_target_)=menu_aspect_->currentData().toString().toStdString();apply_editor_aspect_transition(previous_aspect,stored_menu_aspect(displayed_target_));});
        connect(menu_aspect_,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this](int){const auto previous=stored_menu_aspect(displayed_target_);stored_menu_aspect(displayed_target_)=menu_aspect_->currentData().toString().toStdString();apply_editor_aspect_transition(previous,stored_menu_aspect(displayed_target_));});

        auto* split=new QSplitter;root->addWidget(split,1);
        auto* left=new QWidget;auto* lv=new QVBoxLayout(left);
        titles_=new QTableWidget(0,19);titles_->setSelectionBehavior(QAbstractItemView::SelectRows);titles_->setSelectionMode(QAbstractItemView::ExtendedSelection);titles_->setHorizontalHeaderLabels({"Title","Source","Video","Bitrate","Encoder preset","2-pass","Default audio","Default audio bitrate","Chapters","Frame rate","Force re-encode","Resolution","Aspect","GOP / keyframe","Advanced","Navigation / UOP","Streams","Minrate","Maxrate"});titles_->horizontalHeader()->setSectionResizeMode(0,QHeaderView::ResizeToContents);titles_->horizontalHeader()->setSectionResizeMode(1,QHeaderView::Stretch);for(int col=2;col<19;++col)titles_->horizontalHeader()->setSectionResizeMode(col,QHeaderView::ResizeToContents);
        lv->addWidget(new QLabel("Titles — a title may be linked from any number of menu pages"));lv->addWidget(titles_,2);
        auto* titleBtns=new QHBoxLayout;auto* addTitle=new QPushButton("Add video…");auto* removeTitle=new QPushButton("Remove title");titleBtns->addWidget(addTitle);titleBtns->addWidget(removeTitle);lv->addLayout(titleBtns);
        lv->addWidget(new QLabel("Menus"));menu_tree_=new QTreeWidget;menu_tree_->setHeaderHidden(true);lv->addWidget(menu_tree_,1);
        auto* menuBtns=new QHBoxLayout;add_menu_button_=new QPushButton("Add menu");remove_menu_button_=new QPushButton("Remove menu");menuBtns->addWidget(add_menu_button_);menuBtns->addWidget(remove_menu_button_);lv->addLayout(menuBtns);split->addWidget(left);

        auto* right=new QWidget;auto* rv=new QVBoxLayout(right);
        scene_=new QGraphicsScene(0,0,kProjectDesignWidth,kProjectDesignHeight);canvas_host_=new MenuCanvasHost(scene_,right);view_=canvas_host_->view();rv->addWidget(new QLabel("Current menu — 3840×2160 design space; 4:3 menus use the centered 2880×2160 aperture"));rv->addWidget(canvas_host_,3);
        auto* lower=new QSplitter(Qt::Horizontal);rv->addWidget(lower,2);
        auto* buttonPanel=new QWidget;auto* bp=new QVBoxLayout(buttonPanel);bp->addWidget(new QLabel("Buttons on this menu"));button_list_=new QListWidget;bp->addWidget(button_list_,1);auto* b1=new QHBoxLayout;auto* addTitleButton=new QPushButton("Add title/chapter button…");auto* addStreamButton=new QPushButton("Add audio/subtitle button…");auto* addMenuButton=new QPushButton("Add menu link…");b1->addWidget(addTitleButton);b1->addWidget(addStreamButton);b1->addWidget(addMenuButton);bp->addLayout(b1);auto* b2=new QHBoxLayout;auto* editButton=new QPushButton("Edit selected…");auto* delButton=new QPushButton("Remove selected");b2->addWidget(editButton);b2->addWidget(delButton);bp->addLayout(b2);
        bp->addSpacing(8);bp->addWidget(new QLabel("Labels burned into menu video"));overlay_list_=new QListWidget;bp->addWidget(overlay_list_,1);auto* l1=new QHBoxLayout;auto* addTextLabel=new QPushButton("Add text…");auto* addImageLabel=new QPushButton("Add image…");l1->addWidget(addTextLabel);l1->addWidget(addImageLabel);bp->addLayout(l1);auto* l2=new QHBoxLayout;auto* editLabel=new QPushButton("Edit label…");auto* delLabel=new QPushButton("Remove label");l2->addWidget(editLabel);l2->addWidget(delLabel);bp->addLayout(l2);lower->addWidget(buttonPanel);

        auto* settingsScroll=new QScrollArea;settingsScroll->setWidgetResizable(true);auto* settings=new QWidget;settingsScroll->setWidget(settings);auto* sv=new QVBoxLayout(settings);
        auto* menuGroup=new QGroupBox("Menu page settings");auto* mf=new QFormLayout(menuGroup);menu_name_=new QLineEdit;mf->addRow("Name",menu_name_);
        inherit_bg_=new QCheckBox("Inherit from parent");bg_type_=new QComboBox;bg_type_->addItems({"Still image","Video"});bg_=new QLineEdit;auto* bgrow=new QWidget;auto* bgl=new QHBoxLayout(bgrow);bgl->setContentsMargins(0,0,0,0);bgl->addWidget(inherit_bg_);bgl->addWidget(bg_type_);bgl->addWidget(bg_,1);auto* bgBrowse=new QPushButton("Browse…");bgl->addWidget(bgBrowse);mf->addRow("Background",bgrow);
        inherit_bg_color_=new QCheckBox("Inherit from parent");bg_color_=new ColorButton({0,0,0,255});auto* colorrow=new QWidget;auto* colorl=new QHBoxLayout(colorrow);colorl->setContentsMargins(0,0,0,0);colorl->addWidget(inherit_bg_color_);colorl->addWidget(bg_color_,1);mf->addRow("Background color",colorrow);
        inherit_style_=new QCheckBox("Inherit from parent");default_style_button_=new QPushButton("Edit default text-button style…");auto* stylerow=new QWidget;auto* stylel=new QHBoxLayout(stylerow);stylel->setContentsMargins(0,0,0,0);stylel->addWidget(inherit_style_);stylel->addWidget(default_style_button_,1);mf->addRow("Button style",stylerow);
        inherit_audio_=new QCheckBox("Inherit from parent");menu_audio_path_=new QLineEdit;auto* ar=new QWidget;auto* al=new QHBoxLayout(ar);al->setContentsMargins(0,0,0,0);al->addWidget(inherit_audio_);al->addWidget(menu_audio_path_,1);auto* audioBrowse=new QPushButton("Browse…");al->addWidget(audioBrowse);mf->addRow("Menu audio",ar);
        inherit_duration_=new QCheckBox("Inherit from parent");duration_=new QDoubleSpinBox;duration_->setRange(0.001,86400.0);duration_->setDecimals(3);duration_->setSuffix(" s");auto* dr=new QWidget;auto* dl=new QHBoxLayout(dr);dl->setContentsMargins(0,0,0,0);dl->addWidget(inherit_duration_);dl->addWidget(duration_,1);mf->addRow("Duration",dr);loop_media_=new QCheckBox("Loop menu audio/video");loop_media_->setChecked(true);loop_media_->setToolTip("When enabled, menu media repeats continuously. When disabled, media plays once and the menu remains on its final frame.");mf->addRow("Media playback",loop_media_);
        auto_back_=new QCheckBox("Automatically add Back button");edit_back_button_=new QPushButton("Edit Back button…");auto* br=new QWidget;auto* bl=new QHBoxLayout(br);bl->setContentsMargins(0,0,0,0);bl->addWidget(auto_back_);bl->addWidget(edit_back_button_);mf->addRow("Submenu back",br);sv->addWidget(menuGroup);
        auto* encGroup=new QGroupBox("Menu encoding");auto* encForm=new QFormLayout(encGroup);inherit_encoding_=new QCheckBox("Inherit encoding from parent");encForm->addRow(inherit_encoding_);{const auto& initial_menu_encoding=encoding_for_target(root_menu_,displayed_target_);menu_video_=video_combo(encGroup,displayed_target_,initial_menu_encoding.video_codec);menu_bitrate_=bitrate_spin(encGroup);configure_video_bitrate_spin(menu_bitrate_,displayed_target_,initial_menu_encoding.video_codec,initial_menu_encoding.video_bitrate_kbps,authoring_limits_.allow_exceeding_format_limits);menu_minrate_=video_rate_limit_spin(encGroup);configure_video_minrate_spin(menu_minrate_,displayed_target_,initial_menu_encoding.video_codec,initial_menu_encoding.video_min_bitrate_kbps,authoring_limits_.allow_exceeding_format_limits);menu_maxrate_=video_rate_limit_spin(encGroup);configure_video_maxrate_spin(menu_maxrate_,displayed_target_,initial_menu_encoding.video_codec,initial_menu_encoding.video_max_bitrate_kbps,authoring_limits_.allow_exceeding_format_limits);menu_preset_=preset_combo(encGroup);menu_two_pass_=new QCheckBox("Enable two-pass encoding",encGroup);menu_keyframe_=keyframe_interval_spin(encGroup,initial_menu_encoding.keyframe_interval_frames);configure_keyframe_interval_spin(menu_keyframe_,displayed_target_,initial_menu_encoding.video_codec,frame_rate_->currentData().toString().toStdString(),initial_menu_encoding.keyframe_interval_frames);menu_audio_codec_=audio_combo(encGroup,displayed_target_,initial_menu_encoding.audio_codec);menu_audio_bitrate_=audio_bitrate_spin(encGroup);configure_audio_bitrate_spin(menu_audio_bitrate_,displayed_target_,initial_menu_encoding.audio_codec,audio_bitrate_kbps(initial_menu_encoding),authoring_limits_.allow_exceeding_format_limits);menu_advanced_=new QPushButton("Advanced…",encGroup);}encForm->addRow("Video codec",menu_video_);encForm->addRow("Video bitrate",menu_bitrate_);encForm->addRow("Video minrate",menu_minrate_);encForm->addRow("Video maxrate",menu_maxrate_);encForm->addRow("Encoder speed preset",menu_preset_);encForm->addRow("Passes",menu_two_pass_);encForm->addRow("GOP / keyframe interval",menu_keyframe_);encForm->addRow("Audio codec",menu_audio_codec_);encForm->addRow("Audio bitrate",menu_audio_bitrate_);encForm->addRow("Advanced codec options",menu_advanced_);sv->addWidget(encGroup);sv->addStretch(1);lower->addWidget(settingsScroll);lower->setStretchFactor(0,1);lower->setStretchFactor(1,2);
        split->addWidget(right);split->setStretchFactor(0,2);split->setStretchFactor(1,3);

        cache_enabled_state_=project_defaults_.use_encode_cache;
        compliance_cache_enabled_state_=project_defaults_.use_compliance_cache;
        auto* sizeRow=new QHBoxLayout;
        size_target_=new QComboBox(this);
        size_target_->setMinimumWidth(420);
        size_target_->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);
        size_target_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        size_target_->setMinimumContentsLength(40);
        automatic_bitrate_=new QCheckBox("Automatic video bitrate",this);
        automatic_bitrate_->setChecked(true);
        automatic_bitrate_->setToolTip("Choose the highest video bitrate that fits the selected image size after audio, subtitles, muxing, filesystem/control-file overhead, and a safety margin are reserved.");
        sizeRow->addWidget(new QLabel("Size target:"));
        sizeRow->addWidget(size_target_,1);
        sizeRow->addWidget(automatic_bitrate_);
        sizeRow->addWidget(new QLabel("Unlimited removes only the capacity limit; disc-format bitrate limits still apply."));
        sizeRow->addStretch(1);
        root->addLayout(sizeRow);
        populate_size_target_choices(displayed_target_,initial_project.disc_capacity_bytes);
        auto* bottom=new QHBoxLayout;progress_=new QProgressBar;progress_->setRange(0,10000);progress_->setFormat("0.00%");build_=new QPushButton;cancel_=new QPushButton("Cancel");cancel_->setEnabled(false);update_build_button_text();bottom->addWidget(progress_,1);bottom->addWidget(cancel_);bottom->addWidget(build_);root->addLayout(bottom);statusBar()->showMessage("Native nested HDMV menus; no Java/BD-J is generated");

        connect(addTitle,&QPushButton::clicked,this,[this]{add_videos();});
        connect(removeTitle,&QPushButton::clicked,this,[this]{remove_selected_titles();});
        connect(add_menu_button_,&QPushButton::clicked,this,[this]{add_submenu();});connect(remove_menu_button_,&QPushButton::clicked,this,[this]{remove_current_menu();});
        connect(addTitleButton,&QPushButton::clicked,this,[this]{add_title_button();});connect(addStreamButton,&QPushButton::clicked,this,[this]{add_stream_button();});connect(addMenuButton,&QPushButton::clicked,this,[this]{add_menu_link();});connect(editButton,&QPushButton::clicked,this,[this]{edit_selected_button();});connect(delButton,&QPushButton::clicked,this,[this]{remove_selected_button();});
        connect(addTextLabel,&QPushButton::clicked,this,[this]{add_text_label();});connect(addImageLabel,&QPushButton::clicked,this,[this]{add_image_label();});connect(editLabel,&QPushButton::clicked,this,[this]{edit_selected_label();});connect(delLabel,&QPushButton::clicked,this,[this]{remove_selected_label();});
        connect(button_list_,&QListWidget::itemDoubleClicked,this,[this](QListWidgetItem*){edit_selected_button();});connect(overlay_list_,&QListWidget::itemDoubleClicked,this,[this](QListWidgetItem*){edit_selected_label();});
        connect(menu_tree_,&QTreeWidget::currentItemChanged,this,[this](QTreeWidgetItem* item,QTreeWidgetItem*){if(!item)return;current_menu_id_=item->data(0,Qt::UserRole).toString().toStdString();load_menu_controls();refresh_canvas();refresh_button_list();refresh_overlay_list();});
        connect(bgBrowse,&QPushButton::clicked,this,[this]{const bool video=bg_type_->currentIndex()==1;const auto filter=video?QString("Video files (*.mp4 *.mkv *.mov *.m2ts *.mts *.ts *.mpeg *.mpg *.avi *.webm);;All files (*)"):QString("Image files (*.png *.jpg *.jpeg *.webp *.bmp *.tif *.tiff);;All files (*)");const auto f=QFileDialog::getOpenFileName(this,video?"Menu background video":"Menu background image",bg_->text(),filter);if(!f.isEmpty()){bg_->setText(f);mark_modified();load_menu_controls();}});
        connect(audioBrowse,&QPushButton::clicked,this,[this]{const auto f=QFileDialog::getOpenFileName(this,"Menu audio",menu_audio_path_->text());if(!f.isEmpty()){menu_audio_path_->setText(f);mark_modified();load_menu_controls();}});
        connect(default_style_button_,&QPushButton::clicked,this,[this]{auto* m=current_menu();if(!m)return;ButtonStyleDialog d(this,m->default_button_style);if(d.exec()==QDialog::Accepted){m->default_button_style=d.value();mark_modified();refresh_canvas();}});
        connect(edit_back_button_,&QPushButton::clicked,this,[this]{edit_back_button();});
        connect(menu_name_,&QLineEdit::editingFinished,this,[this]{if(loading_menu_)return;if(auto*m=current_menu()){m->name=menu_name_->text().toStdString();mark_modified();if(auto* parent=parent_of(root_menu_,m->id))for(auto& b:parent->buttons)if(b.auto_submenu_link&&b.target_menu_id==m->id){b.label=m->name;for(auto& a:b.actions)if(a.kind==NavigationActionKind::Menu&&a.target_menu_id==m->id)a.target_menu_id=m->id;}rebuild_menu_tree();refresh_button_list();}});
        connect(bg_,&QLineEdit::textChanged,this,[this](const QString& v){if(loading_menu_)return;if(auto*m=current_menu()){if(bg_type_->currentIndex()==1){m->background_video=v.toStdString();m->background_image.clear();}else{m->background_image=v.toStdString();m->background_video.clear();}mark_modified();}update_enabled();refresh_canvas();});
        connect(bg_type_,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this](int index){if(loading_menu_)return;if(auto*m=current_menu()){const auto path=bg_->text().toStdString();if(index==1){m->background_video=path;m->background_image.clear();}else{m->background_image=path;m->background_video.clear();}mark_modified();}load_menu_controls();refresh_canvas();});
        connect(menu_audio_path_,&QLineEdit::textChanged,this,[this](const QString& v){if(loading_menu_)return;if(auto*m=current_menu()){m->audio_source=v.toStdString();mark_modified();}update_enabled();});
        connect(bg_,&QLineEdit::editingFinished,this,[this]{if(!loading_menu_)load_menu_controls();});
        connect(menu_audio_path_,&QLineEdit::editingFinished,this,[this]{if(!loading_menu_)load_menu_controls();});
        connect(bg_color_,&QPushButton::clicked,this,[this]{if(loading_menu_)return;if(auto*m=current_menu()){const auto chosen=bg_color_->value();if(!(m->background_color==chosen))mark_modified();m->background_color=chosen;refresh_canvas();}});
        connect(duration_,QOverload<double>::of(&QDoubleSpinBox::valueChanged),this,[this](double v){if(loading_menu_)return;if(auto*m=current_menu()){m->duration_seconds=v;mark_modified();}});connect(loop_media_,&QCheckBox::toggled,this,[this](bool v){if(loading_menu_)return;if(auto*m=current_menu()){m->loop_media=v;mark_modified();}});
        connect(auto_back_,&QCheckBox::toggled,this,[this](bool v){if(loading_menu_)return;if(auto*m=current_menu()){m->auto_back_button=v;mark_modified();edit_back_button_->setEnabled(v);refresh_canvas();}});
        auto encoding_changed=[this]{if(loading_menu_)return;if(auto*m=current_menu()){auto& active=encoding_for_target(*m,displayed_target_);const auto base=active;active=profile_from_widgets(menu_video_,menu_bitrate_,menu_preset_,menu_two_pass_,menu_audio_codec_,menu_audio_bitrate_,&base,menu_keyframe_,menu_minrate_,menu_maxrate_);mark_modified();}};
        connect(menu_video_,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this,encoding_changed](int){const auto codec=static_cast<VideoCodec>(menu_video_->currentData().toInt());configure_video_bitrate_spin(menu_bitrate_,displayed_target_,codec,menu_bitrate_->value(),authoring_limits_.allow_exceeding_format_limits);configure_video_minrate_spin(menu_minrate_,displayed_target_,codec,menu_minrate_->value(),authoring_limits_.allow_exceeding_format_limits);configure_video_maxrate_spin(menu_maxrate_,displayed_target_,codec,menu_maxrate_->value(),authoring_limits_.allow_exceeding_format_limits);configure_keyframe_interval_spin(menu_keyframe_,displayed_target_,codec,frame_rate_->currentData().toString().toStdString());encoding_changed();});connect(menu_bitrate_,QOverload<int>::of(&QSpinBox::valueChanged),this,[encoding_changed](int){encoding_changed();});connect(menu_minrate_,QOverload<int>::of(&QSpinBox::valueChanged),this,[encoding_changed](int){encoding_changed();});connect(menu_maxrate_,QOverload<int>::of(&QSpinBox::valueChanged),this,[encoding_changed](int){encoding_changed();});connect(menu_preset_,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[encoding_changed](int){encoding_changed();});connect(menu_two_pass_,&QCheckBox::toggled,this,[encoding_changed](bool){encoding_changed();});connect(menu_keyframe_,QOverload<int>::of(&QSpinBox::valueChanged),this,[encoding_changed](int){encoding_changed();});connect(menu_audio_bitrate_,QOverload<int>::of(&QSpinBox::valueChanged),this,[encoding_changed](int){encoding_changed();});connect(menu_advanced_,&QPushButton::clicked,this,[this,encoding_changed]{if(auto*m=current_menu()){auto& active=encoding_for_target(*m,displayed_target_);const auto base=active;active=profile_from_widgets(menu_video_,menu_bitrate_,menu_preset_,menu_two_pass_,menu_audio_codec_,menu_audio_bitrate_,&base,menu_keyframe_,menu_minrate_,menu_maxrate_);if(edit_advanced_codec_options(this,displayed_target_,active))encoding_changed();else active=base;}});
        connect(menu_audio_codec_,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this,encoding_changed](int){if(loading_menu_)return;if(auto*m=current_menu()){const auto codec=static_cast<AudioCodec>(menu_audio_codec_->currentData().toInt());const auto& active=encoding_for_target(*m,displayed_target_);const int bitrate=codec==AudioCodec::Dca?active.dca_bitrate_kbps:active.ac3_bitrate_kbps;configure_audio_bitrate_spin(menu_audio_bitrate_,displayed_target_,codec,bitrate,authoring_limits_.allow_exceeding_format_limits);}encoding_changed();});
        connect(inherit_bg_,&QCheckBox::toggled,this,[this](bool v){set_inherit_background_image(v);});connect(inherit_bg_color_,&QCheckBox::toggled,this,[this](bool v){set_inherit_background_color(v);});connect(inherit_style_,&QCheckBox::toggled,this,[this](bool v){set_inherit_style(v);});connect(inherit_audio_,&QCheckBox::toggled,this,[this](bool v){set_inherit_audio(v);});connect(inherit_duration_,&QCheckBox::toggled,this,[this](bool v){set_inherit_duration(v);});connect(inherit_encoding_,&QCheckBox::toggled,this,[this](bool v){set_inherit_encoding(v);});
        connect(scene_,&QGraphicsScene::selectionChanged,this,[this]{for(auto* item:scene_->selectedItems()){if(auto* b=dynamic_cast<ButtonPreviewItem*>(item)){if(b->index()!=BackIndex&&b->index()<static_cast<std::size_t>(button_list_->count()))button_list_->setCurrentRow(static_cast<int>(b->index()));break;}if(auto* o=dynamic_cast<OverlayPreviewItem*>(item)){if(o->index()<static_cast<std::size_t>(overlay_list_->count()))overlay_list_->setCurrentRow(static_cast<int>(o->index()));break;}}});
        connect(target_,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this](int){const auto previous_rate=frame_rate_->currentData().toString().toStdString();const auto previous_aspect=stored_menu_aspect(displayed_target_);const auto previous_target=displayed_target_;const auto previous_capacity=selected_disc_capacity_bytes();const auto next=static_cast<DiscTarget>(target_->currentData().toInt());store_visible_title_profiles(displayed_target_);store_visible_menu_mode();displayed_target_=next;const auto target_default=project_file_.isEmpty()?new_project_disc_capacity_for_target(project_defaults_,next):default_disc_capacity_bytes(next);const auto previous_default=project_file_.isEmpty()?new_project_disc_capacity_for_target(project_defaults_,previous_target):default_disc_capacity_bytes(previous_target);const auto next_capacity=previous_capacity==previous_default?target_default:((disc_capacity_is_unlimited(previous_capacity)||target_has_capacity_preset(next,previous_capacity))?previous_capacity:target_default);populate_size_target_choices(next,next_capacity);populate_frame_rate_choices(previous_rate);populate_resolution_choices(menu_resolution_,displayed_target_,stored_menu_resolution(displayed_target_),true);populate_aspect_choices(menu_aspect_,displayed_target_,stored_menu_aspect(displayed_target_),false,stored_menu_resolution(displayed_target_));stored_menu_resolution(displayed_target_)=menu_resolution_->currentData().toString().toStdString();stored_menu_aspect(displayed_target_)=menu_aspect_->currentData().toString().toStdString();apply_editor_aspect_transition(previous_aspect,stored_menu_aspect(displayed_target_));load_visible_title_profiles(displayed_target_);load_menu_controls();update_build_button_text();statusBar()->showMessage(next==DiscTarget::DvdVideo480p?"DVD-Video menu resolution is independent and defaults to 720×480 NTSC; PAL cannot be mixed with the NTSC family.":"Each output target keeps its own menu resolution/aspect settings.",5000);});
        connect(frame_rate_,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this](int){const auto project_rate=frame_rate_->currentData().toString().toStdString();if(menu_video_&&menu_keyframe_)configure_keyframe_interval_spin(menu_keyframe_,displayed_target_,static_cast<VideoCodec>(menu_video_->currentData().toInt()),project_rate);for(int r=0;r<titles_->rowCount();++r){auto* rate=qobject_cast<QComboBox*>(titles_->cellWidget(r,9));auto* video=qobject_cast<QComboBox*>(titles_->cellWidget(r,2));auto* keyframe=qobject_cast<QSpinBox*>(titles_->cellWidget(r,13));if(rate&&video&&keyframe&&rate->currentData().toString()==QStringLiteral("inherit"))configure_keyframe_interval_spin(keyframe,displayed_target_,static_cast<VideoCodec>(video->currentData().toInt()),project_rate);}});
        connect(startupSequence,&QPushButton::clicked,this,[this]{edit_startup_sequence();});
        connect(build_,&QPushButton::clicked,this,[this]{build_project();});connect(cancel_,&QPushButton::clicked,this,[this]{cancel_render();});connect(automatic_bitrate_,&QCheckBox::toggled,this,[this](bool){update_enabled();});
        connect(titles_,&QTableWidget::itemChanged,this,[this](QTableWidgetItem*){mark_modified();});
        for(auto* editor:findChildren<QWidget*>())watch_editor(editor);
        rebuild_menu_tree();load_menu_controls();refresh_all();suppress_modified_=false;mark_clean();refresh_external_tool_availability();QTimer::singleShot(0,this,[]{maybe_run_scheduled_cache_cleanup();});QTimer::singleShot(0,this,[this]{show_tool_warnings();});
    }
protected:
    void closeEvent(QCloseEvent* event) override {
        if(render_active()){
            const auto answer=QMessageBox::warning(this,"Render in progress","A disc image is currently being rendered. Closing BDMV Author will cancel the render/encoding, stop helper processes, and remove temporary render files. Close anyway?",QMessageBox::Close|QMessageBox::Cancel,QMessageBox::Cancel);
            if(answer!=QMessageBox::Close){event->ignore();return;}
            close_after_render_=true;
            cancel_render();
            event->ignore();
            return;
        }
        if(maybe_save_modified())event->accept();else event->ignore();
    }
private:
    static bool target_has_capacity_preset(DiscTarget target,std::uint64_t bytes){return disc_size_is_preset(target,bytes);}
    static QString capacity_label(std::uint64_t bytes){if(disc_capacity_is_unlimited(bytes))return "Unlimited";return QString::number(static_cast<double>(bytes)/1000000000.0,'f',bytes%1000000000ULL?1:0)+" GB (custom)";}
    void populate_size_target_choices(DiscTarget target,std::uint64_t selected){
        if(!size_target_)return;const QSignalBlocker blocker(size_target_);size_target_->clear();
        for(const auto& entry:disc_size_presets(target))size_target_->addItem(entry.first,QVariant::fromValue<qulonglong>(entry.second));
        int index=size_target_->findData(QVariant::fromValue<qulonglong>(selected));
        if(index<0){size_target_->addItem(capacity_label(selected),QVariant::fromValue<qulonglong>(selected));index=size_target_->count()-1;}
        size_target_->setCurrentIndex(index);
    }
    std::uint64_t selected_disc_capacity_bytes() const {return size_target_?static_cast<std::uint64_t>(size_target_->currentData().toULongLong()):default_disc_capacity_bytes(displayed_target_);}
    bool render_active() const {return watcher_&&!watcher_->isFinished();}
    void cancel_render(){
        if(!render_active()||!cancel_requested_)return;
        cancel_requested_->store(true,std::memory_order_release);
        if(cancel_)cancel_->setEnabled(false);
        statusBar()->showMessage("Cancelling render and cleaning temporary files…");
    }
    void edit_cache_settings(){
        QDialog d(this);d.setWindowTitle("Cache settings");auto* root=new QVBoxLayout(&d);
        auto* encode=new QCheckBox("Use encoded-media cache",&d);encode->setChecked(cache_enabled_state_);
        auto* compliance=new QCheckBox("Use cached compliance analysis",&d);compliance->setChecked(compliance_cache_enabled_state_);
        auto* refresh_encode=new QCheckBox("Refresh encoded-media cache this build",&d);refresh_encode->setChecked(refresh_encode_cache_state_);
        auto* refresh_compliance=new QCheckBox("Re-run compliance analysis this build",&d);refresh_compliance->setChecked(refresh_compliance_cache_state_);
        auto sync=[=]{refresh_encode->setEnabled(encode->isChecked());refresh_compliance->setEnabled(compliance->isChecked());if(!encode->isChecked())refresh_encode->setChecked(false);if(!compliance->isChecked())refresh_compliance->setChecked(false);};
        connect(encode,&QCheckBox::toggled,&d,[=](bool){sync();});connect(compliance,&QCheckBox::toggled,&d,[=](bool){sync();});sync();
        root->addWidget(encode);root->addWidget(compliance);root->addSpacing(8);root->addWidget(refresh_encode);root->addWidget(refresh_compliance);
        auto* manager=new QPushButton("Cache manager…",&d);root->addWidget(manager);connect(manager,&QPushButton::clicked,&d,[this]{CacheManagerDialog(this).exec();});
        auto* buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&d);root->addWidget(buttons);connect(buttons,&QDialogButtonBox::accepted,&d,&QDialog::accept);connect(buttons,&QDialogButtonBox::rejected,&d,&QDialog::reject);
        if(d.exec()!=QDialog::Accepted)return;
        const bool persistent_changed=cache_enabled_state_!=encode->isChecked()||compliance_cache_enabled_state_!=compliance->isChecked();
        cache_enabled_state_=encode->isChecked();compliance_cache_enabled_state_=compliance->isChecked();refresh_encode_cache_state_=refresh_encode->isChecked();refresh_compliance_cache_state_=refresh_compliance->isChecked();
        if(persistent_changed)mark_modified();
    }
    void set_target_availability(){
        if(!target_)return;
        for(int row=0;row<target_->count();++row){const auto target=static_cast<DiscTarget>(target_->itemData(row).toInt());set_combo_item_enabled(target_,row,target_available(target),QStringLiteral("Required external tools/encoders for this output target are unavailable. Configure them in Settings → External tools and encoders."));}
    }
    void refresh_external_tool_availability(){
        set_target_availability();
        if(menu_video_&&menu_audio_codec_)load_menu_controls();
        if(titles_)load_visible_title_profiles(displayed_target_);
        if(build_)build_->setEnabled(target_available(displayed_target_));
        statusBar()->showMessage(QStringLiteral("External-tool scan complete. %1").arg(target_available(displayed_target_)?"The selected target is available.":"The selected target is unavailable; see Settings → External tools and encoders."),5000);
    }
    void edit_tool_settings(){
        ToolSettingsDialog d(this,g_tool_capabilities.config);if(d.exec()!=QDialog::Accepted)return;save_tool_configuration(d.value());g_tool_capabilities=probe_tool_capabilities(d.value());refresh_external_tool_availability();if(!g_tool_capabilities.warnings.isEmpty())QMessageBox::warning(this,"External tool warnings",g_tool_capabilities.warnings.join("\n\n")+"\n\nUnavailable codec choices and targets have been disabled.");
    }
    void show_tool_warnings(){
        if(g_tool_capabilities.warnings.isEmpty())return;
        QMessageBox::warning(this,"External tool warnings",g_tool_capabilities.warnings.join("\n\n")+"\n\nOpen Settings → External tools and encoders to select alternate executables or providers. Unavailable codecs are disabled rather than making the whole FFmpeg installation unusable.");
    }
    QString profile_tool_problem(const EncodingProfile& e,DiscTarget target,const QString& where)const{
        if(!video_codec_available(target,e.video_codec))return QStringLiteral("%1 uses %2, but that encoder is unavailable.").arg(where,video_codec_display_name(e.video_codec));
        if(!audio_codec_available(target,e.audio_codec))return QStringLiteral("%1 uses %2, but that FFmpeg audio encoder is unavailable.").arg(where,audio_codec_display_name(e.audio_codec));
        return {};
    }
    QString build_tool_problem(const Project& p)const{
        if(!target_available(p.target))return QStringLiteral("The selected %1 target is unavailable with the currently configured external tools. Open Settings → External tools and encoders.").arg(output_target_name(p.target));
        for(std::size_t i=0;i<p.titles.size();++i){const auto problem=profile_tool_problem(encoding_for_target(p.titles[i],p.target),p.target,QStringLiteral("Title %1").arg(i+1));if(!problem.isEmpty())return problem;}
        std::function<QString(const Menu&)> check_menu=[&](const Menu& m)->QString{if(m.id.empty())return {};const auto effective=effective_menu(m);const auto problem=profile_tool_problem(encoding_for_target(effective,p.target),p.target,QStringLiteral("Menu %1").arg(QString::fromStdString(m.name.empty()?m.id:m.name)));if(!problem.isEmpty())return problem;for(const auto& child:m.submenus){const auto nested=check_menu(child);if(!nested.isEmpty())return nested;}return {};};
        return check_menu(p.menu);
    }
    void populate_frame_rate_choices(const std::string& selected) {
        const QSignalBlocker blocker(frame_rate_);
        frame_rate_->clear();
        frame_rate_->addItem("Auto (match source)", QStringLiteral("auto"));
        if(displayed_target_==DiscTarget::DvdVideo480p){
            frame_rate_->addItem("Film DVD — 23.976p, 720×480", QStringLiteral("film-dvd"));
            frame_rate_->addItem("NTSC DVD — 29.97i, 720×480", QStringLiteral("ntsc-dvd"));
            frame_rate_->addItem("PAL DVD — 25i, 720×576", QStringLiteral("pal-dvd"));
        }else if(displayed_target_==DiscTarget::UltraHdBluRay2160){
            frame_rate_->addItem("23.976p", QStringLiteral("23.976p"));
            frame_rate_->addItem("24p", QStringLiteral("24p"));
            frame_rate_->addItem("25p", QStringLiteral("25p"));
            frame_rate_->addItem("50p (experimental — explicit only)", QStringLiteral("50p"));
            frame_rate_->addItem("59.94p (experimental — explicit only)", QStringLiteral("59.94p"));
            frame_rate_->addItem("60p (experimental — explicit only)", QStringLiteral("60p"));
        }else{
            frame_rate_->addItem("23.976p", QStringLiteral("23.976p"));
            frame_rate_->addItem("24p", QStringLiteral("24p"));
            frame_rate_->addItem("25p (1080 AVC — x264 fake interlaced)", QStringLiteral("25p"));
            frame_rate_->addItem("29.97p (1080 AVC — x264 fake interlaced)", QStringLiteral("29.97p"));
            frame_rate_->addItem("50i", QStringLiteral("50i"));
            frame_rate_->addItem("59.94i", QStringLiteral("59.94i"));
            frame_rate_->addItem("50p (720p only)", QStringLiteral("50p"));
            frame_rate_->addItem("59.94p (720p only)", QStringLiteral("59.94p"));
        }
        int index=frame_rate_->findData(QString::fromStdString(selected));
        if(index<0) index=0;
        frame_rate_->setCurrentIndex(index);
    }
    static void populate_title_frame_rate_choices(QComboBox* combo, DiscTarget target, const std::string& selected) {
        const QSignalBlocker blocker(combo);
        combo->clear();
        combo->addItem("Project default", QStringLiteral("inherit"));
        combo->addItem("Auto (match title)", QStringLiteral("auto"));
        if(target==DiscTarget::DvdVideo480p){
            combo->addItem("Film DVD — 23.976p", QStringLiteral("film-dvd"));
            combo->addItem("NTSC DVD — 29.97i", QStringLiteral("ntsc-dvd"));
            combo->addItem("PAL DVD — 25i", QStringLiteral("pal-dvd"));
        }else if(target==DiscTarget::UltraHdBluRay2160){
            combo->addItem("23.976p", QStringLiteral("23.976p"));
            combo->addItem("24p", QStringLiteral("24p"));
            combo->addItem("25p", QStringLiteral("25p"));
            combo->addItem("50p (experimental — explicit only)", QStringLiteral("50p"));
            combo->addItem("59.94p (experimental — explicit only)", QStringLiteral("59.94p"));
            combo->addItem("60p (experimental — explicit only)", QStringLiteral("60p"));
        }else{
            combo->addItem("23.976p", QStringLiteral("23.976p"));
            combo->addItem("24p", QStringLiteral("24p"));
            combo->addItem("25p (1080 AVC — x264 fake interlaced)", QStringLiteral("25p"));
            combo->addItem("29.97p (1080 AVC — x264 fake interlaced)", QStringLiteral("29.97p"));
            combo->addItem("50i", QStringLiteral("50i"));
            combo->addItem("59.94i", QStringLiteral("59.94i"));
            combo->addItem("50p (720p only)", QStringLiteral("50p"));
            combo->addItem("59.94p (720p only)", QStringLiteral("59.94p"));
        }
        int index=combo->findData(QString::fromStdString(selected));
        if(index<0) index=0;
        combo->setCurrentIndex(index);
        combo->setToolTip(target==DiscTarget::DvdVideo480p
            ? "Film and NTSC titles may coexist; PAL titles require an all-PAL disc."
            : "Per-title override. UHD 50/59.94/60p modes are experimental and are never selected by Auto; they must be chosen explicitly. 25p/29.97p at 1080 lines require AVC/x264 fake-interlaced signaling; true 50i/59.94i remain separate choices.");
    }
    static Menu* find_menu(Menu& root,const std::string& id){if(root.id==id)return &root;for(auto& c:root.submenus)if(auto* x=find_menu(c,id))return x;return nullptr;}
    static const Menu* find_menu(const Menu& root,const std::string& id){if(root.id==id)return &root;for(const auto& c:root.submenus)if(auto* x=find_menu(c,id))return x;return nullptr;}
    static Menu* parent_of(Menu& root,const std::string& id){for(auto& c:root.submenus){if(c.id==id)return &root;if(auto* x=parent_of(c,id))return x;}return nullptr;}
    static const Menu* parent_of(const Menu& root,const std::string& id){for(const auto& c:root.submenus){if(c.id==id)return &root;if(auto* x=parent_of(c,id))return x;}return nullptr;}
    Menu* current_menu(){if(root_menu_.id.empty()||current_menu_id_.empty())return nullptr;return find_menu(root_menu_,current_menu_id_);} const Menu* current_menu()const{if(root_menu_.id.empty()||current_menu_id_.empty())return nullptr;return find_menu(root_menu_,current_menu_id_);}
    Menu effective_menu(const Menu& source) const {
        const auto* parent=parent_of(root_menu_,source.id);if(!parent)return source;Menu p=effective_menu(*parent),e=source;
        if(source.inherit_background_image){e.background_image=p.background_image;e.background_video=p.background_video;}if(source.inherit_background_color)e.background_color=p.background_color;if(source.inherit_button_style)e.default_button_style=p.default_button_style;if(source.inherit_encoding){e.encoding=p.encoding;e.uhd_encoding=p.uhd_encoding;e.dvd_encoding=p.dvd_encoding;}if(source.inherit_audio)e.audio_source=p.audio_source;if(source.inherit_duration)e.duration_seconds=p.duration_seconds;e.submenus.clear();return e;
    }
    static bool automatic_menu_duration(const Menu& menu){return !menu.background_video.empty()||!menu.audio_source.empty();}
    void set_duration_control_from_menu(const Menu& menu){
        const bool automatic=automatic_menu_duration(menu);duration_->setMinimum(automatic?0.001:10.0);duration_->setDecimals(automatic?3:1);duration_->setSuffix(automatic?" s (automatic)":" s");
        if(!automatic){duration_->setToolTip("Manual duration is available for still-image menus with no menu audio.");duration_->setValue(menu.duration_seconds);return;}
        double resolved=0.0;
        if(!menu.background_video.empty())if(const auto d=probe_gui_stream_duration(QString::fromStdString(menu.background_video.string()),"v:0"))resolved=std::max(resolved,*d);
        if(!menu.audio_source.empty()){
            if(const auto d=probe_gui_stream_duration(QString::fromStdString(menu.audio_source.string()),"a:0"))resolved=std::max(resolved,*d);
        }else if(!menu.background_video.empty()){
            // With no explicit audio file, the authoring path uses the first
            // audio stream embedded in the background video when present.
            if(const auto d=probe_gui_stream_duration(QString::fromStdString(menu.background_video.string()),"a:0"))resolved=std::max(resolved,*d);
        }
        if(resolved>0.0)duration_->setValue(resolved);else duration_->setValue(std::max(0.001,menu.duration_seconds));
        duration_->setToolTip("Automatic: the longer of the background video and chosen menu audio. A separate menu-audio file overrides audio embedded in the background video.");
    }
    Rect default_bounds(std::size_t n) const {const int col=static_cast<int>((n/8)%2),row=static_cast<int>(n%8);return {320+col*1640,300+row*210,project_defaults_.button_width,project_defaults_.button_height};}
    static void populate_resolution_choices(QComboBox* combo, DiscTarget target, const std::string& selected, bool menu) {
        const QSignalBlocker blocker(combo);combo->clear();
        if(!menu)combo->addItem("Auto (closest source)",QStringLiteral("auto"));
        auto add=[&](const char* text,const char* token){combo->addItem(text,QString::fromLatin1(token));};
        if(target==DiscTarget::UltraHdBluRay2160){if(menu){add("1920×1080 (recommended for VLC)","1920x1080");add("3840×2160","3840x2160");}else{add("3840×2160","3840x2160");add("1920×1080","1920x1080");}}
        else if(target==DiscTarget::BluRay1080){add("1920×1080","1920x1080");add("1440×1080 anamorphic","1440x1080");add("1280×720","1280x720");add("720×576 SD","720x576");add("720×480 SD","720x480");}
        else{add("720×480 NTSC","720x480");add("720×576 PAL","720x576");add("704×480 NTSC","704x480");add("704×576 PAL","704x576");add("352×480 NTSC","352x480");add("352×576 PAL","352x576");add("352×240 NTSC","352x240");add("352×288 PAL","352x288");}
        std::string wanted=selected;if(menu&&wanted=="highest")wanted=target==DiscTarget::UltraHdBluRay2160?"3840x2160":target==DiscTarget::DvdVideo480p?"720x480":"1920x1080";else if(menu&&wanted.empty())wanted=target==DiscTarget::UltraHdBluRay2160?"1920x1080":target==DiscTarget::DvdVideo480p?"720x480":"1920x1080";
        int index=combo->findData(QString::fromStdString(wanted));if(index<0){const auto fallback=target==DiscTarget::UltraHdBluRay2160?QStringLiteral("1920x1080"):target==DiscTarget::DvdVideo480p?QStringLiteral("720x480"):QStringLiteral("1920x1080");index=menu?combo->findData(fallback):0;}if(index<0)index=0;combo->setCurrentIndex(index);
        combo->setToolTip(menu?"Menu resolution is saved independently for Blu-ray, Ultra HD Blu-ray, and DVD. New projects use 1920×1080 for Blu-ray and UHD, and 720×480 NTSC for DVD. UHD defaults to 1080p to work around a VLC mouse-navigation coordinate bug; selecting 3840×2160 remains available.":"Auto chooses the legal raster closest to the source, preferring scaling up when two choices are equally close.");
    }
    static void populate_aspect_choices(QComboBox* combo, DiscTarget target, const std::string& selected, bool title, const std::string& resolution = {}) {
        const QSignalBlocker blocker(combo);combo->clear();
        const auto r=QString::fromStdString(resolution).toLower();
        const bool resolution_auto=r.isEmpty()||r=="auto";
        bool allow_wide=true,allow_four_three=false;
        if(target==DiscTarget::UltraHdBluRay2160){allow_four_three=false;}
        else if(target==DiscTarget::BluRay1080){allow_four_three=resolution_auto||r=="720x480"||r=="720x576";}
        else{allow_four_three=true;allow_wide=resolution_auto||r=="720x480"||r=="720x576";}
        if(title&&resolution_auto)combo->addItem("Auto (match source)",QStringLiteral("auto"));
        if(allow_wide)combo->addItem("16:9",QStringLiteral("16:9"));
        if(allow_four_three)combo->addItem("4:3",QStringLiteral("4:3"));
        int index=combo->findData(QString::fromStdString(selected));if(index<0)index=0;combo->setCurrentIndex(index);
        combo->setToolTip(target==DiscTarget::UltraHdBluRay2160?"Ultra HD Blu-ray primary video may be 3840×2160 or 1920×1080; both are 16:9.":"Aspect choices are restricted to combinations legal for the selected raster. Blu-ray HD modes are 16:9; DVD widescreen is offered at full-D1 720-wide modes while reduced DVD rasters are 4:3.");
    }
    void set_profile_widgets(const EncodingProfile& e){populate_video_combo(menu_video_,displayed_target_,e.video_codec);configure_video_bitrate_spin(menu_bitrate_,displayed_target_,e.video_codec,e.video_bitrate_kbps,authoring_limits_.allow_exceeding_format_limits);configure_video_minrate_spin(menu_minrate_,displayed_target_,e.video_codec,e.video_min_bitrate_kbps,authoring_limits_.allow_exceeding_format_limits);configure_video_maxrate_spin(menu_maxrate_,displayed_target_,e.video_codec,e.video_max_bitrate_kbps,authoring_limits_.allow_exceeding_format_limits);menu_preset_->setCurrentText(QString::fromStdString(e.video_codec==VideoCodec::Hevc?e.x265_preset:e.x264_preset));menu_two_pass_->setChecked(e.two_pass);configure_keyframe_interval_spin(menu_keyframe_,displayed_target_,e.video_codec,frame_rate_->currentData().toString().toStdString(),e.keyframe_interval_frames);populate_audio_combo(menu_audio_codec_,displayed_target_,e.audio_codec);configure_audio_bitrate_spin(menu_audio_bitrate_,displayed_target_,e.audio_codec,audio_bitrate_kbps(e),authoring_limits_.allow_exceeding_format_limits);}
    void append_tree(QTreeWidgetItem* parent,const Menu& m){if(m.id.empty())return;auto* item=parent?new QTreeWidgetItem(parent):new QTreeWidgetItem(menu_tree_);item->setText(0,QString::fromStdString(m.name));item->setData(0,Qt::UserRole,QString::fromStdString(m.id));for(const auto& c:m.submenus)append_tree(item,c);}
    void rebuild_menu_tree(){const auto keep=current_menu_id_;menu_tree_->clear();if(!root_menu_.id.empty())append_tree(nullptr,root_menu_);menu_tree_->expandAll();QTreeWidgetItemIterator it(menu_tree_);while(*it){if((*it)->data(0,Qt::UserRole).toString().toStdString()==keep){menu_tree_->setCurrentItem(*it);break;}++it;}if(!menu_tree_->currentItem()&&menu_tree_->topLevelItemCount()>0)menu_tree_->setCurrentItem(menu_tree_->topLevelItem(0));if(add_menu_button_)add_menu_button_->setText(root_menu_.id.empty()?"Add main menu":"Add submenu");if(remove_menu_button_)remove_menu_button_->setEnabled(!root_menu_.id.empty());}
    static bool gui_supported_subtitle_codec(const QString& codec){
        const auto c=codec.toLower();
        return c=="hdmv_pgs_subtitle"||c=="subrip"||c=="srt"||c=="ass"||c=="ssa"||c=="webvtt"||c=="text"||c=="mov_text";
    }
    std::vector<TargetChoice> stream_target_choices() const {
        std::vector<TargetChoice> out;
        for(int r=0;r<titles_->rowCount();++r){
            const auto* source_item=titles_->item(r,1);if(!source_item||source_item->text().isEmpty())continue;
            QProcess probe;
            start_tool_process(probe,g_tool_capabilities.ffprobe,QStringList{"-v","error","-show_entries","stream=codec_type,codec_name:stream_tags=language,title","-of","json",source_item->text()});
            if(!probe.waitForStarted(2000)||!probe.waitForFinished(5000)||probe.exitStatus()!=QProcess::NormalExit||probe.exitCode()!=0)continue;
            QJsonParseError parse_error;const auto doc=QJsonDocument::fromJson(probe.readAllStandardOutput(),&parse_error);if(parse_error.error!=QJsonParseError::NoError||!doc.isObject())continue;
            int audio_number=0,subtitle_number=0;
            for(const auto& v:doc.object().value("streams").toArray()){
                const auto o=v.toObject();const auto type=o.value("codec_type").toString();const auto codec=o.value("codec_name").toString();const auto tags=o.value("tags").toObject();
                const auto language=tags.value("language").toString();const auto track_title=tags.value("title").toString();
                if(type=="audio"){
                    ++audio_number;TargetChoice t;t.kind=MenuButtonTargetKind::AudioTrack;t.title=static_cast<std::uint16_t>(r+1);t.stream=static_cast<std::uint16_t>(audio_number);
                    QString detail=language;if(detail.isEmpty())detail=track_title;if(!track_title.isEmpty()&&track_title!=detail)detail+=(detail.isEmpty()?QString():QString(" — "))+track_title;
                    t.label=QString("Title %1 — Audio %2").arg(r+1).arg(audio_number);if(!detail.isEmpty())t.label+=" — "+detail;if(!codec.isEmpty())t.label+=QString(" [%1]").arg(codec);
                    t.suggested_label=!track_title.isEmpty()?track_title:(!language.isEmpty()?QString("Audio — %1").arg(language):QString("Audio %1").arg(audio_number));out.push_back(std::move(t));
                }else if(type=="subtitle"&&gui_supported_subtitle_codec(codec)){
                    ++subtitle_number;TargetChoice t;t.kind=MenuButtonTargetKind::SubtitleTrack;t.title=static_cast<std::uint16_t>(r+1);t.stream=static_cast<std::uint16_t>(subtitle_number);
                    QString detail=language;if(detail.isEmpty())detail=track_title;if(!track_title.isEmpty()&&track_title!=detail)detail+=(detail.isEmpty()?QString():QString(" — "))+track_title;
                    t.label=QString("Title %1 — Subtitle %2").arg(r+1).arg(subtitle_number);if(!detail.isEmpty())t.label+=" — "+detail;if(!codec.isEmpty())t.label+=QString(" [%1]").arg(codec);
                    t.suggested_label=!track_title.isEmpty()?track_title:(!language.isEmpty()?QString("Subtitles — %1").arg(language):QString("Subtitles %1").arg(subtitle_number));out.push_back(std::move(t));
                }
            }
            for(const auto& attached:external_subtitles_from_variant(source_item->data(RoleExternalSubtitles))){
                ++subtitle_number;TargetChoice t;t.kind=MenuButtonTargetKind::SubtitleTrack;t.title=static_cast<std::uint16_t>(r+1);t.stream=static_cast<std::uint16_t>(subtitle_number);
                const auto file=QString::fromStdString(attached.source.filename().string());const auto lang=QString::fromStdString(attached.language);
                t.label=QString("Title %1 — Subtitle %2 — %3").arg(r+1).arg(subtitle_number).arg(file);if(!lang.isEmpty())t.label+=QString(" — %1").arg(lang);t.label+=" [attached]";
                t.suggested_label=!lang.isEmpty()&&lang!="und"?QString("Subtitles — %1").arg(lang):file;out.push_back(std::move(t));
            }
            if(subtitle_number>0){TargetChoice t;t.kind=MenuButtonTargetKind::SubtitleOff;t.title=static_cast<std::uint16_t>(r+1);t.label=QString("Title %1 — Subtitles off").arg(r+1);t.suggested_label="Subtitles off";out.push_back(std::move(t));}
        }
        return out;
    }
    std::vector<TargetChoice> target_choices() const {
        auto out=title_target_choices();
        auto streams=stream_target_choices();out.insert(out.end(),std::make_move_iterator(streams.begin()),std::make_move_iterator(streams.end()));
        std::function<void(const Menu&)> add=[&](const Menu& m){TargetChoice t;t.label="Menu — "+QString::fromStdString(m.name);t.kind=MenuButtonTargetKind::Menu;t.menu_id=m.id;out.push_back(std::move(t));for(const auto& c:m.submenus)add(c);};if(!root_menu_.id.empty())add(root_menu_);return out;
    }
    void refresh_button_list(){button_list_->clear();const auto* m=current_menu();if(!m)return;for(const auto& b:m->buttons){QStringList actions;for(const auto& action:button_action_sequence(b))actions<<stored_action_label(action);button_list_->addItem(QString::fromStdString(b.label)+"  →  "+actions.join(" → "));}}
    void refresh_overlay_list(){overlay_list_->clear();const auto* m=current_menu();if(!m)return;for(const auto& o:m->overlays)overlay_list_->addItem(o.kind==MenuOverlayKind::Text?QString("Text — %1").arg(QString::fromStdString(o.text)):QString("Image — %1").arg(QString::fromStdString(o.image.filename().string())));}
    void refresh_canvas(){
        scene_->clear();const auto aperture=design_aperture(stored_menu_aspect(displayed_target_));scene_->setSceneRect(aperture);const auto* m=current_menu();if(!m){if(canvas_host_)canvas_host_->refit();return;}const auto e=effective_menu(*m);scene_->setBackgroundBrush(QBrush(qcolor(e.background_color)));
        QPixmap background(QString::fromStdString(e.background_image.string()));if(!background.isNull()){const auto scaled=background.scaled(aperture.size().toSize(),Qt::KeepAspectRatio,Qt::SmoothTransformation);auto* bg=scene_->addPixmap(scaled);bg->setPos(aperture.center().x()-scaled.width()/2.0,aperture.center().y()-scaled.height()/2.0);bg->setZValue(-100.0);}else if(!e.background_video.empty()){auto* note=scene_->addText(QString("Video background\n%1").arg(QFileInfo(QString::fromStdString(e.background_video.string())).fileName()),QFont("sans-serif",56));note->setDefaultTextColor(QColor(180,180,190));note->setTextWidth(std::min(1800.0,aperture.width()-80.0));note->setPos(aperture.center().x()-note->textWidth()/2.0,aperture.center().y()-180.0);note->setZValue(-100.0);}
        for(std::size_t i=0;i<m->overlays.size();++i){const auto& o=m->overlays[i];QPixmap image(QString::fromStdString(o.image.string()));auto* item=new OverlayPreviewItem(i,o,image,[this](std::size_t n,const Rect& bounds){if(auto* cm=current_menu();cm&&n<cm->overlays.size()&&!same_rect(cm->overlays[n].bounds,bounds)){invalidate_reversible_aspect_positions();cm->overlays[n].bounds=bounds;mark_modified();}});scene_->addItem(item);}
        for(std::size_t i=0;i<m->buttons.size();++i){const auto& b=m->buttons[i];const auto& style=b.use_custom_style?b.style:e.default_button_style;QPixmap normal(QString::fromStdString(b.normal_image.string()));QPixmap selected(QString::fromStdString(b.selected_image.string()));auto* item=new ButtonPreviewItem(i,b,style,normal,selected,[this](std::size_t n,const Rect& bounds){if(auto* cm=current_menu();cm&&n<cm->buttons.size()&&!same_rect(cm->buttons[n].bounds,bounds)){invalidate_reversible_aspect_positions();cm->buttons[n].bounds=bounds;mark_modified();}});scene_->addItem(item);}
        if(parent_of(root_menu_,m->id)&&m->auto_back_button){const auto& b=m->back_button;const auto& style=b.use_custom_style?b.style:e.default_button_style;QPixmap normal(QString::fromStdString(b.normal_image.string()));QPixmap selected(QString::fromStdString(b.selected_image.string()));auto* item=new ButtonPreviewItem(BackIndex,b,style,normal,selected,[this](std::size_t,const Rect& bounds){if(auto* cm=current_menu();cm&&!same_rect(cm->back_button.bounds,bounds)){invalidate_reversible_aspect_positions();cm->back_button.bounds=bounds;mark_modified();}});scene_->addItem(item);}
        if(canvas_host_)canvas_host_->refit();else view_->fitInView(scene_->sceneRect(),Qt::KeepAspectRatio);
    }
    void refresh_all(){rebuild_menu_tree();refresh_button_list();refresh_overlay_list();refresh_canvas();}
    bool automatic_video_bitrate_enabled() const{return automatic_bitrate_&&automatic_bitrate_->isChecked();}
    void update_enabled(){const bool present=current_menu()!=nullptr;const bool sub=present&&parent_of(root_menu_,current_menu_id_)!=nullptr;const bool automatic=present&&automatic_menu_duration(effective_menu(*current_menu()));inherit_bg_->setEnabled(present&&sub);inherit_bg_color_->setEnabled(present&&sub);inherit_style_->setEnabled(present&&sub);inherit_audio_->setEnabled(present&&sub);inherit_duration_->setEnabled(present&&sub&&!automatic);inherit_encoding_->setEnabled(present&&sub);menu_name_->setEnabled(present);bg_type_->setEnabled(present&&(!sub||!inherit_bg_->isChecked()));bg_->setEnabled(present&&(!sub||!inherit_bg_->isChecked()));bg_color_->setEnabled(present&&(!sub||!inherit_bg_color_->isChecked()));default_style_button_->setEnabled(present&&(!sub||!inherit_style_->isChecked()));menu_audio_path_->setEnabled(present&&(!sub||!inherit_audio_->isChecked()));duration_->setEnabled(present&&!automatic&&(!sub||!inherit_duration_->isChecked()));loop_media_->setEnabled(present&&automatic);const bool enc=present&&(!sub||!inherit_encoding_->isChecked());menu_video_->setEnabled(enc);menu_bitrate_->setEnabled(enc&&!automatic_video_bitrate_enabled());menu_minrate_->setEnabled(enc);menu_maxrate_->setEnabled(enc);menu_preset_->setEnabled(enc);menu_two_pass_->setEnabled(enc);menu_keyframe_->setEnabled(enc);menu_audio_codec_->setEnabled(enc);menu_audio_bitrate_->setEnabled(enc&&audio_codec_has_configurable_bitrate(static_cast<AudioCodec>(menu_audio_codec_->currentData().toInt())));menu_advanced_->setEnabled(enc);auto_back_->setEnabled(present&&sub);edit_back_button_->setEnabled(present&&sub&&auto_back_->isChecked());for(int r=0;r<titles_->rowCount();++r)if(auto* bitrate=qobject_cast<QSpinBox*>(titles_->cellWidget(r,3)))bitrate->setEnabled(!automatic_video_bitrate_enabled());}
    void load_menu_controls(){auto* m=current_menu();if(!m){loading_menu_=true;menu_name_->clear();bg_->clear();menu_audio_path_->clear();loading_menu_=false;update_enabled();return;}const auto e=effective_menu(*m);const bool sub=parent_of(root_menu_,m->id)!=nullptr;loading_menu_=true;menu_name_->setText(QString::fromStdString(m->name));inherit_bg_->setChecked(sub&&m->inherit_background_image);inherit_bg_color_->setChecked(sub&&m->inherit_background_color);inherit_style_->setChecked(sub&&m->inherit_button_style);inherit_audio_->setChecked(sub&&m->inherit_audio);inherit_duration_->setChecked(sub&&m->inherit_duration);inherit_encoding_->setChecked(sub&&m->inherit_encoding);bg_type_->setCurrentIndex(e.background_video.empty()?0:1);bg_->setText(QString::fromStdString((e.background_video.empty()?e.background_image:e.background_video).string()));bg_color_->setValue(e.background_color);menu_audio_path_->setText(QString::fromStdString(e.audio_source.string()));loop_media_->setChecked(e.loop_media);set_duration_control_from_menu(e);set_profile_widgets(encoding_for_target(e,displayed_target_));auto_back_->setChecked(sub&&m->auto_back_button);loading_menu_=false;update_enabled();}
    template<class Copy> void toggle_inherit(bool v,bool Menu::*flag,Copy copy){if(loading_menu_)return;auto* m=current_menu();if(!m||!parent_of(root_menu_,m->id))return;if(!v){const auto e=effective_menu(*m);copy(*m,e);}m->*flag=v;load_menu_controls();refresh_canvas();}
    void set_inherit_background_image(bool v){toggle_inherit(v,&Menu::inherit_background_image,[](Menu&m,const Menu&e){m.background_image=e.background_image;m.background_video=e.background_video;});}
    void set_inherit_background_color(bool v){toggle_inherit(v,&Menu::inherit_background_color,[](Menu&m,const Menu&e){m.background_color=e.background_color;});}
    void set_inherit_style(bool v){toggle_inherit(v,&Menu::inherit_button_style,[](Menu&m,const Menu&e){m.default_button_style=e.default_button_style;});}
    void set_inherit_audio(bool v){toggle_inherit(v,&Menu::inherit_audio,[](Menu&m,const Menu&e){m.audio_source=e.audio_source;});}
    void set_inherit_duration(bool v){toggle_inherit(v,&Menu::inherit_duration,[](Menu&m,const Menu&e){m.duration_seconds=e.duration_seconds;});}
    void set_inherit_encoding(bool v){toggle_inherit(v,&Menu::inherit_encoding,[](Menu&m,const Menu&e){m.encoding=e.encoding;m.uhd_encoding=e.uhd_encoding;m.dvd_encoding=e.dvd_encoding;});}
    static QString chapter_summary(ChapterMode mode,double interval,std::size_t manual_count){
        switch(mode){
            case ChapterMode::SourceOrFiveMinute:return "Source / 5 min fallback";
            case ChapterMode::Manual:return QString("Manual (%1)").arg(manual_count+1U);
            case ChapterMode::Interval:{const int min=static_cast<int>(interval/60.0);const double sec=std::fmod(interval,60.0);return min>0?QString("Every %1m %2s").arg(min).arg(sec,0,'f',sec==std::floor(sec)?0:1):QString("Every %1s").arg(interval,0,'f',interval==std::floor(interval)?0:1);}
            case ChapterMode::None:return "None";
        }
        return "Chapters";
    }
    static std::vector<double> chapter_list_from_item(const QTableWidgetItem* item){std::vector<double> out;if(!item)return out;for(const auto& v:item->data(RoleChapters).toList())out.push_back(v.toDouble());return out;}
    void edit_title_chapters(QTableWidgetItem* sourceItem,QPushButton* button){if(!sourceItem||!button)return;const auto mode=static_cast<ChapterMode>(sourceItem->data(RoleChapterMode).toInt());const double interval=sourceItem->data(RoleChapterInterval).toDouble();ChapterSettingsDialog d(this,mode,chapter_list_from_item(sourceItem),interval>0.0?interval:300.0);if(d.exec()!=QDialog::Accepted)return;QVariantList chapters;for(double c:d.chapters())chapters.push_back(c);sourceItem->setData(RoleChapters,chapters);sourceItem->setData(RoleChapterMode,static_cast<int>(d.mode()));sourceItem->setData(RoleChapterInterval,d.interval_seconds());button->setText(chapter_summary(d.mode(),d.interval_seconds(),d.chapters().size()));}
    static QString gui_chapter_time(double seconds){const auto total=static_cast<long long>(std::llround(std::max(0.0,seconds)));const auto h=total/3600LL,m=(total/60LL)%60LL,sec=total%60LL;return h>0?QString("%1:%2:%3").arg(h).arg(m,2,10,QChar('0')).arg(sec,2,10,QChar('0')):QString("%1:%2").arg(m).arg(sec,2,10,QChar('0'));}
    std::vector<double> resolved_gui_chapter_starts(int row) const {
        const auto* item=titles_->item(row,1);if(!item)return {};
        const auto mode=static_cast<ChapterMode>(item->data(RoleChapterMode).toInt());
        if(mode==ChapterMode::None)return {};
        auto normalize=[](std::vector<double> v,double duration){std::sort(v.begin(),v.end());std::vector<double> out;for(double x:v){if(!std::isfinite(x)||x<=0.001)continue;if(duration>0.0&&x>=duration-0.001)continue;if(!out.empty()&&std::abs(x-out.back())<=0.001)continue;out.push_back(x);}return out;};
        double duration=0.0;std::vector<double> source;
        if(mode==ChapterMode::SourceOrFiveMinute||mode==ChapterMode::Interval||mode==ChapterMode::Manual){QProcess probe;start_tool_process(probe,g_tool_capabilities.ffprobe,QStringList{"-v","error","-show_entries","format=duration:chapter=start_time","-of","json",item->text()});if(probe.waitForStarted(2000)&&probe.waitForFinished(5000)&&probe.exitStatus()==QProcess::NormalExit&&probe.exitCode()==0){QJsonParseError e;const auto doc=QJsonDocument::fromJson(probe.readAllStandardOutput(),&e);if(e.error==QJsonParseError::NoError&&doc.isObject()){duration=doc.object().value("format").toObject().value("duration").toString().toDouble();if(mode==ChapterMode::SourceOrFiveMinute)for(const auto& v:doc.object().value("chapters").toArray())source.push_back(v.toObject().value("start_time").toString().toDouble());}}}
        if(mode==ChapterMode::Manual)return normalize(chapter_list_from_item(item),duration);
        if(mode==ChapterMode::SourceOrFiveMinute){auto out=normalize(std::move(source),duration);if(!out.empty())return out;}
        const double interval=mode==ChapterMode::Interval?item->data(RoleChapterInterval).toDouble():300.0;std::vector<double> out;if(duration>0.0&&interval>0.0)for(double t=interval;t<duration-0.001&&out.size()<998U;t+=interval)out.push_back(t);return out;
    }
    std::vector<TargetChoice> title_target_choices() const {
        std::vector<TargetChoice> out;
        for(int r=0;r<titles_->rowCount();++r){const auto name=titles_->item(r,0)?titles_->item(r,0)->text():QString();TargetChoice base;base.label=QString("Title %1 — %2").arg(r+1).arg(name);base.suggested_label=name;base.kind=MenuButtonTargetKind::Title;base.title=static_cast<std::uint16_t>(r+1);out.push_back(base);const auto starts=resolved_gui_chapter_starts(r);for(std::size_t i=0;i<starts.size();++i){TargetChoice t;t.kind=MenuButtonTargetKind::Title;t.title=static_cast<std::uint16_t>(r+1);t.chapter=static_cast<std::uint16_t>(i+2U);t.label=QString("Title %1 — Chapter %2 (%3)").arg(r+1).arg(i+2U).arg(gui_chapter_time(starts[i]));t.suggested_label=QString("%1 — Chapter %2").arg(name).arg(i+2U);out.push_back(std::move(t));}}
        return out;
    }
    void append_title_row(const Title& t){
        const int r=titles_->rowCount();titles_->insertRow(r);
        auto* nameItem=new QTableWidgetItem(QString::fromStdString(t.name));
        auto* sourceItem=new QTableWidgetItem(QString::fromStdString(t.source.string()));
        sourceItem->setData(RoleAudioLanguage,QString::fromStdString(t.audio_language));
        QVariantList chapters;for(double c:t.chapters_seconds)chapters.push_back(c);sourceItem->setData(RoleChapters,chapters);sourceItem->setData(RoleChapterMode,static_cast<int>(t.chapter_mode));sourceItem->setData(RoleChapterInterval,t.chapter_interval_seconds);
        sourceItem->setData(RoleAc3Bitrate,t.encoding.ac3_bitrate_kbps);sourceItem->setData(RoleDcaBitrate,t.encoding.dca_bitrate_kbps);sourceItem->setData(RoleHdProfile,encoding_map(t.encoding));sourceItem->setData(RoleUhdProfile,encoding_map(t.uhd_encoding));sourceItem->setData(RoleDvdProfile,encoding_map(t.dvd_encoding));sourceItem->setData(RoleHdFrameRate,QString::fromStdString(t.frame_rate));sourceItem->setData(RoleUhdFrameRate,QString::fromStdString(t.uhd_frame_rate));sourceItem->setData(RoleDvdFrameRate,QString::fromStdString(t.dvd_frame_rate));sourceItem->setData(RoleHdResolution,QString::fromStdString(t.resolution));sourceItem->setData(RoleUhdResolution,QString::fromStdString(t.uhd_resolution));sourceItem->setData(RoleDvdResolution,QString::fromStdString(t.dvd_resolution));sourceItem->setData(RoleHdAspect,QString::fromStdString(t.aspect_ratio));sourceItem->setData(RoleUhdAspect,QString::fromStdString(t.uhd_aspect_ratio));sourceItem->setData(RoleDvdAspect,QString::fromStdString(t.dvd_aspect_ratio));sourceItem->setData(RoleTitleMenuActions,navigation_actions_variant(t.menu_button_actions));sourceItem->setData(RoleTitleUopMask,QVariant::fromValue<qulonglong>(t.prohibited_user_operations));sourceItem->setData(RoleAudioStreamSettings,audio_stream_settings_variant(t.audio_stream_settings));sourceItem->setData(RoleSubtitleStreamSettings,subtitle_stream_settings_variant(t.subtitle_stream_settings));sourceItem->setData(RoleExternalSubtitles,external_subtitles_variant(t.external_subtitles));sourceItem->setData(RoleSubtitleDefaultStyle,subtitle_style_map(t.subtitle_default_style));sourceItem->setData(RoleDefaultAudioStream,t.default_audio_stream);sourceItem->setData(RoleDefaultSubtitleStream,t.default_subtitle_stream);
        titles_->setItem(r,0,nameItem);titles_->setItem(r,1,sourceItem);
        const auto& active=encoding_for_target(t,displayed_target_);
        auto* video=video_combo(titles_,displayed_target_,active.video_codec);titles_->setCellWidget(r,2,video);
        auto* bitrate=bitrate_spin(titles_);configure_video_bitrate_spin(bitrate,displayed_target_,active.video_codec,active.video_bitrate_kbps,authoring_limits_.allow_exceeding_format_limits);titles_->setCellWidget(r,3,bitrate);
        auto* preset=preset_combo(titles_);preset->setCurrentText(QString::fromStdString(active.video_codec==VideoCodec::Hevc?active.x265_preset:active.x264_preset));titles_->setCellWidget(r,4,preset);
        auto* two=new QCheckBox(titles_);two->setChecked(active.two_pass);titles_->setCellWidget(r,5,two);
        auto* audio=audio_combo(titles_,displayed_target_,active.audio_codec);titles_->setCellWidget(r,6,audio);
        auto* audioBitrate=audio_bitrate_spin(titles_);configure_audio_bitrate_spin(audioBitrate,displayed_target_,active.audio_codec,audio_bitrate_kbps(active),authoring_limits_.allow_exceeding_format_limits);titles_->setCellWidget(r,7,audioBitrate);auto* chapterButton=new QPushButton(chapter_summary(t.chapter_mode,t.chapter_interval_seconds,t.chapters_seconds.size()),titles_);chapterButton->setToolTip("Choose source, manual, timed, or no chapter divisions for this title.");titles_->setCellWidget(r,8,chapterButton);auto* frameRate=new QComboBox(titles_);populate_title_frame_rate_choices(frameRate,displayed_target_,frame_rate_for_target(t,displayed_target_));titles_->setCellWidget(r,9,frameRate);auto* force=new QCheckBox;force->setChecked(t.force_reencode);force->setToolTip("Force both video and audio for this title to be re-encoded using the selected fallback encoding settings.");force->setStyleSheet("margin-left: 12px; margin-right: 12px;");titles_->setCellWidget(r,10,force);auto* resolution=new QComboBox(titles_);populate_resolution_choices(resolution,displayed_target_,resolution_for_target(t,displayed_target_),false);titles_->setCellWidget(r,11,resolution);auto* aspect=new QComboBox(titles_);populate_aspect_choices(aspect,displayed_target_,aspect_ratio_for_target(t,displayed_target_),true,resolution_for_target(t,displayed_target_));titles_->setCellWidget(r,12,aspect);auto* keyframe=keyframe_interval_spin(titles_,active.keyframe_interval_frames);{auto rate_token=frameRate->currentData().toString().toStdString();if(rate_token=="inherit")rate_token=frame_rate_->currentData().toString().toStdString();configure_keyframe_interval_spin(keyframe,displayed_target_,active.video_codec,rate_token,active.keyframe_interval_frames);}titles_->setCellWidget(r,13,keyframe);auto* advanced=new QPushButton("Advanced…",titles_);advanced->setToolTip("Set codec-private options for this title while preserving disc-spec-critical settings.");titles_->setCellWidget(r,14,advanced);auto* navigation=new QPushButton("Navigation…",titles_);navigation->setToolTip("Set this title's player Menu-button behavior and User Operation Prohibitions.");titles_->setCellWidget(r,15,navigation);auto* streams=new QPushButton("Streams…",titles_);streams->setToolTip("Configure individual audio codecs, bitrates, LPCM format, downmix/channel layout, advanced options, language flags, and attached subtitle files.");titles_->setCellWidget(r,16,streams);auto* minrate=video_rate_limit_spin(titles_);configure_video_minrate_spin(minrate,displayed_target_,active.video_codec,active.video_min_bitrate_kbps,authoring_limits_.allow_exceeding_format_limits);titles_->setCellWidget(r,17,minrate);auto* maxrate=video_rate_limit_spin(titles_);configure_video_maxrate_spin(maxrate,displayed_target_,active.video_codec,active.video_max_bitrate_kbps,authoring_limits_.allow_exceeding_format_limits);titles_->setCellWidget(r,18,maxrate);
        connect(chapterButton,&QPushButton::clicked,this,[this,sourceItem,chapterButton]{edit_title_chapters(sourceItem,chapterButton);});
        connect(resolution,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this,resolution,aspect](int){const auto keep=aspect->currentData().toString().toStdString();populate_aspect_choices(aspect,displayed_target_,keep,true,resolution->currentData().toString().toStdString());});
        connect(video,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this,video,bitrate,minrate,maxrate,frameRate,keyframe](int){const auto codec=static_cast<VideoCodec>(video->currentData().toInt());configure_video_bitrate_spin(bitrate,displayed_target_,codec,bitrate->value(),authoring_limits_.allow_exceeding_format_limits);configure_video_minrate_spin(minrate,displayed_target_,codec,minrate->value(),authoring_limits_.allow_exceeding_format_limits);configure_video_maxrate_spin(maxrate,displayed_target_,codec,maxrate->value(),authoring_limits_.allow_exceeding_format_limits);auto rate_token=frameRate->currentData().toString().toStdString();if(rate_token=="inherit")rate_token=frame_rate_->currentData().toString().toStdString();configure_keyframe_interval_spin(keyframe,displayed_target_,codec,rate_token);});
        connect(frameRate,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this,video,frameRate,keyframe](int){auto rate_token=frameRate->currentData().toString().toStdString();if(rate_token=="inherit")rate_token=frame_rate_->currentData().toString().toStdString();configure_keyframe_interval_spin(keyframe,displayed_target_,static_cast<VideoCodec>(video->currentData().toInt()),rate_token);});
        connect(audioBitrate,QOverload<int>::of(&QSpinBox::valueChanged),this,[this,sourceItem,audio](int v){const auto role=profile_role(displayed_target_);auto e=encoding_from_map(sourceItem->data(role),default_profile_for_target(displayed_target_));e.audio_codec=static_cast<AudioCodec>(audio->currentData().toInt());if(audio_codec_has_configurable_bitrate(e.audio_codec))set_audio_bitrate_kbps(e,v);sourceItem->setData(role,encoding_map(e));});
        connect(audio,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this,sourceItem,audio,audioBitrate](int){const auto role=profile_role(displayed_target_);auto e=encoding_from_map(sourceItem->data(role),default_profile_for_target(displayed_target_));const auto codec=static_cast<AudioCodec>(audio->currentData().toInt());e.audio_codec=codec;configure_audio_bitrate_spin(audioBitrate,displayed_target_,codec,audio_bitrate_kbps(e),authoring_limits_.allow_exceeding_format_limits);sourceItem->setData(role,encoding_map(e));});
        connect(advanced,&QPushButton::clicked,this,[this,advanced]{int row=-1;for(int i=0;i<titles_->rowCount();++i)if(titles_->cellWidget(i,14)==advanced){row=i;break;}if(row<0)return;auto* source=titles_->item(row,1);if(!source)return;auto e=row_profile(row);if(edit_advanced_codec_options(this,displayed_target_,e)){source->setData(profile_role(displayed_target_),encoding_map(e));mark_modified();}});
        connect(navigation,&QPushButton::clicked,this,[this,navigation]{int row=-1;for(int i=0;i<titles_->rowCount();++i)if(titles_->cellWidget(i,15)==navigation){row=i;break;}if(row<0)return;auto* source=titles_->item(row,1);if(!source)return;TitleNavigationDialog d(this,navigation_actions_from_variant(source->data(RoleTitleMenuActions)),source->data(RoleTitleUopMask).toULongLong(),target_choices(),!root_menu_.id.empty());if(d.exec()!=QDialog::Accepted)return;source->setData(RoleTitleMenuActions,navigation_actions_variant(d.actions()));source->setData(RoleTitleUopMask,QVariant::fromValue<qulonglong>(d.prohibited_user_operations()));mark_modified();});
        connect(streams,&QPushButton::clicked,this,[this,streams]{int row=-1;for(int i=0;i<titles_->rowCount();++i)if(titles_->cellWidget(i,16)==streams){row=i;break;}if(row<0)return;auto* source=titles_->item(row,1);if(!source)return;store_visible_title_profiles(displayed_target_);const auto hd=stored_row_profile(row,DiscTarget::BluRay1080);const auto uhd=stored_row_profile(row,DiscTarget::UltraHdBluRay2160);const auto dvd=stored_row_profile(row,DiscTarget::DvdVideo480p);const auto stream_profile=row_profile(row);TitleStreamsDialog d(this,source->text(),displayed_target_,stream_profile.video_bitrate_kbps,hd,uhd,dvd,subtitle_style_from_map(source->data(RoleSubtitleDefaultStyle)),audio_stream_settings_from_variant(source->data(RoleAudioStreamSettings)),subtitle_stream_settings_from_variant(source->data(RoleSubtitleStreamSettings)),external_subtitles_from_variant(source->data(RoleExternalSubtitles)),source->data(RoleDefaultAudioStream).toInt(),source->data(RoleDefaultSubtitleStream).isValid()?source->data(RoleDefaultSubtitleStream).toInt():-1,authoring_limits_.allow_exceeding_format_limits);if(d.exec()!=QDialog::Accepted)return;source->setData(RoleAudioStreamSettings,audio_stream_settings_variant(d.audio_settings()));source->setData(RoleSubtitleStreamSettings,subtitle_stream_settings_variant(d.subtitle_settings()));source->setData(RoleExternalSubtitles,external_subtitles_variant(d.external_subtitles()));source->setData(RoleDefaultAudioStream,d.default_audio_stream());source->setData(RoleDefaultSubtitleStream,d.default_subtitle_stream());mark_modified();});
        bitrate->setEnabled(!automatic_video_bitrate_enabled());
        watch_editor(video);watch_editor(bitrate);watch_editor(minrate);watch_editor(maxrate);watch_editor(preset);watch_editor(two);watch_editor(audio);watch_editor(audioBitrate);watch_editor(frameRate);watch_editor(force);watch_editor(resolution);watch_editor(aspect);watch_editor(keyframe);
    }
    void add_videos(){QFileDialog d(this,"Add video titles");d.setFileMode(QFileDialog::ExistingFiles);d.setOption(QFileDialog::DontUseNativeDialog,true);auto* add_button=new QCheckBox("Automatically add a menu button for each video",&d);add_button->setChecked(true);if(root_menu_.id.empty()){add_button->setChecked(false);add_button->setEnabled(false);add_button->setToolTip("No main menu is present. Add a main menu first to create automatic buttons.");}if(auto* grid=qobject_cast<QGridLayout*>(d.layout()))grid->addWidget(add_button,grid->rowCount(),0,1,std::max(1,grid->columnCount()));if(d.exec()!=QDialog::Accepted)return;const bool create_button=add_button->isChecked();for(const auto& path:d.selectedFiles())add_title(path,create_button);refresh_all();}
    void add_title(const QString& path,bool create_button=true){Title t=make_title_from_defaults(project_defaults_);t.name=QFileInfo(path).completeBaseName().toStdString();t.source=path.toStdString();append_title_row(t);const int r=titles_->rowCount()-1;if(create_button&&!root_menu_.id.empty()){auto* m=current_menu();if(!m)m=&root_menu_;MenuButton b;b.label=t.name;b.target_kind=MenuButtonTargetKind::Title;b.target_title=static_cast<std::uint16_t>(r+1);b.bounds=clamp_rect_to_aperture(default_bounds(m->buttons.size()),stored_menu_aspect(displayed_target_));invalidate_reversible_aspect_positions();m->buttons.push_back(std::move(b));}mark_modified();}
    EncodingProfile stored_row_profile(int r,DiscTarget target)const{const auto* item=titles_->item(r,1);const EncodingProfile fallback=default_profile_for_target(target);if(!item)return fallback;return encoding_from_map(item->data(profile_role(target)),fallback);}
    EncodingProfile row_profile(int r)const{auto*v=qobject_cast<QComboBox*>(titles_->cellWidget(r,2));auto*b=qobject_cast<QSpinBox*>(titles_->cellWidget(r,3));auto*preset=qobject_cast<QComboBox*>(titles_->cellWidget(r,4));auto*two=qobject_cast<QCheckBox*>(titles_->cellWidget(r,5));auto*a=qobject_cast<QComboBox*>(titles_->cellWidget(r,6));auto*ab=qobject_cast<QSpinBox*>(titles_->cellWidget(r,7));auto*k=qobject_cast<QSpinBox*>(titles_->cellWidget(r,13));auto*minrate=qobject_cast<QSpinBox*>(titles_->cellWidget(r,17));auto*maxrate=qobject_cast<QSpinBox*>(titles_->cellWidget(r,18));if(!v||!b||!preset||!two||!a||!ab||!k||!minrate||!maxrate)throw std::runtime_error("internal title encoding-control error");const auto base=stored_row_profile(r,displayed_target_);return profile_from_widgets(v,b,preset,two,a,ab,&base,k,minrate,maxrate);}
    std::string stored_row_frame_rate(int r,DiscTarget target)const{const auto* item=titles_->item(r,1);if(!item)return "inherit";const auto v=item->data(frame_rate_role(target)).toString().toStdString();return v.empty()?"inherit":v;}
    std::string stored_row_resolution(int r,DiscTarget target)const{const auto* item=titles_->item(r,1);if(!item)return "auto";const auto v=item->data(resolution_role(target)).toString().toStdString();return v.empty()?"auto":v;}
    std::string stored_row_aspect(int r,DiscTarget target)const{const auto* item=titles_->item(r,1);if(!item)return "auto";const auto v=item->data(aspect_role(target)).toString().toStdString();return v.empty()?"auto":v;}
    std::string row_frame_rate(int r)const{auto* rate=qobject_cast<QComboBox*>(titles_->cellWidget(r,9));return rate?rate->currentData().toString().toStdString():"inherit";}
    std::string row_resolution(int r)const{auto* combo=qobject_cast<QComboBox*>(titles_->cellWidget(r,11));return combo?combo->currentData().toString().toStdString():"auto";}
    std::string row_aspect(int r)const{auto* combo=qobject_cast<QComboBox*>(titles_->cellWidget(r,12));return combo?combo->currentData().toString().toStdString():"auto";}
    void store_visible_title_profiles(DiscTarget target){for(int r=0;r<titles_->rowCount();++r)if(auto* item=titles_->item(r,1)){item->setData(profile_role(target),encoding_map(row_profile(r)));item->setData(frame_rate_role(target),QString::fromStdString(row_frame_rate(r)));item->setData(resolution_role(target),QString::fromStdString(row_resolution(r)));item->setData(aspect_role(target),QString::fromStdString(row_aspect(r)));}}
    void set_row_profile_widgets(int r,const EncodingProfile& e,DiscTarget target){auto*v=qobject_cast<QComboBox*>(titles_->cellWidget(r,2));auto*b=qobject_cast<QSpinBox*>(titles_->cellWidget(r,3));auto*preset=qobject_cast<QComboBox*>(titles_->cellWidget(r,4));auto*two=qobject_cast<QCheckBox*>(titles_->cellWidget(r,5));auto*a=qobject_cast<QComboBox*>(titles_->cellWidget(r,6));auto*ab=qobject_cast<QSpinBox*>(titles_->cellWidget(r,7));auto*k=qobject_cast<QSpinBox*>(titles_->cellWidget(r,13));auto*minrate=qobject_cast<QSpinBox*>(titles_->cellWidget(r,17));auto*maxrate=qobject_cast<QSpinBox*>(titles_->cellWidget(r,18));auto*rate=qobject_cast<QComboBox*>(titles_->cellWidget(r,9));if(!v||!b||!preset||!two||!a||!ab||!k||!minrate||!maxrate)return;const QSignalBlocker bv(v),bb(b),bp(preset),bt(two),ba(a),bab(ab),bk(k),bmin(minrate),bmax(maxrate);populate_video_combo(v,target,e.video_codec);configure_video_bitrate_spin(b,target,e.video_codec,e.video_bitrate_kbps,authoring_limits_.allow_exceeding_format_limits);configure_video_minrate_spin(minrate,target,e.video_codec,e.video_min_bitrate_kbps,authoring_limits_.allow_exceeding_format_limits);configure_video_maxrate_spin(maxrate,target,e.video_codec,e.video_max_bitrate_kbps,authoring_limits_.allow_exceeding_format_limits);preset->setCurrentText(QString::fromStdString(e.video_codec==VideoCodec::Hevc?e.x265_preset:e.x264_preset));two->setChecked(e.two_pass);auto rate_token=rate?rate->currentData().toString().toStdString():std::string("auto");if(rate_token=="inherit")rate_token=frame_rate_->currentData().toString().toStdString();configure_keyframe_interval_spin(k,target,e.video_codec,rate_token,e.keyframe_interval_frames);populate_audio_combo(a,target,e.audio_codec);configure_audio_bitrate_spin(ab,target,e.audio_codec,audio_bitrate_kbps(e),authoring_limits_.allow_exceeding_format_limits);}
    void load_visible_title_profiles(DiscTarget target){for(int r=0;r<titles_->rowCount();++r){if(auto* rate=qobject_cast<QComboBox*>(titles_->cellWidget(r,9)))populate_title_frame_rate_choices(rate,target,stored_row_frame_rate(r,target));set_row_profile_widgets(r,stored_row_profile(r,target),target);if(auto* resolution=qobject_cast<QComboBox*>(titles_->cellWidget(r,11)))populate_resolution_choices(resolution,target,stored_row_resolution(r,target),false);if(auto* aspect=qobject_cast<QComboBox*>(titles_->cellWidget(r,12)))populate_aspect_choices(aspect,target,stored_row_aspect(r,target),true,stored_row_resolution(r,target));}}

    void remove_selected_titles(){invalidate_reversible_aspect_positions();std::vector<int> rows;for(const auto& index:titles_->selectionModel()->selectedRows())rows.push_back(index.row());if(rows.empty()&&titles_->currentRow()>=0)rows.push_back(titles_->currentRow());std::sort(rows.begin(),rows.end(),std::greater<int>());rows.erase(std::unique(rows.begin(),rows.end()),rows.end());for(const int row:rows){if(row<0||row>=titles_->rowCount())continue;const unsigned n=static_cast<unsigned>(row+1);titles_->removeRow(row);remove_title_references(root_menu_,static_cast<std::uint16_t>(n));remove_title_references(first_play_actions_,static_cast<std::uint16_t>(n));}mark_modified();refresh_all();}
    void add_submenu(){if(root_menu_.id.empty()){Project p;apply_new_project_defaults(p,project_defaults_);root_menu_=p.menu;root_menu_.id="top";root_menu_.name="Top Menu";root_menu_.inherit_background_image=false;root_menu_.inherit_background_color=false;root_menu_.inherit_button_style=false;root_menu_.inherit_encoding=false;root_menu_.inherit_audio=false;root_menu_.inherit_duration=false;current_menu_id_="top";next_menu_id_=1;invalidate_reversible_aspect_positions();mark_modified();refresh_all();load_menu_controls();statusBar()->showMessage("Main menu restored.",5000);return;}auto* parent=current_menu();if(!parent)parent=&root_menu_;Menu child;child.id="menu-"+std::to_string(next_menu_id_++);child.name="Submenu "+std::to_string(next_menu_id_-1);MenuButton link;link.label=child.name;link.target_kind=MenuButtonTargetKind::Menu;link.target_menu_id=child.id;link.auto_submenu_link=true;NavigationAction link_action;link_action.kind=NavigationActionKind::Menu;link_action.target_menu_id=child.id;set_single_button_action(link,std::move(link_action));link.bounds=clamp_rect_to_aperture(default_bounds(parent->buttons.size()),stored_menu_aspect(displayed_target_));invalidate_reversible_aspect_positions();parent->buttons.push_back(std::move(link));parent->submenus.push_back(std::move(child));mark_modified();current_menu_id_=parent->submenus.back().id;rebuild_menu_tree();load_menu_controls();refresh_button_list();refresh_overlay_list();refresh_canvas();}
    static void collect_menu_ids(const Menu& m,std::vector<std::string>& ids){if(!m.id.empty())ids.push_back(m.id);for(const auto& c:m.submenus)collect_menu_ids(c,ids);}
    static void remove_menu_links(Menu& m,const std::vector<std::string>& ids){m.buttons.erase(std::remove_if(m.buttons.begin(),m.buttons.end(),[&](MenuButton& b){if(b.actions.empty())return b.target_kind==MenuButtonTargetKind::Menu&&std::find(ids.begin(),ids.end(),b.target_menu_id)!=ids.end();b.actions.erase(std::remove_if(b.actions.begin(),b.actions.end(),[&](const NavigationAction& a){return a.kind==NavigationActionKind::Menu&&std::find(ids.begin(),ids.end(),a.target_menu_id)!=ids.end();}),b.actions.end());if(b.actions.empty())return true;mirror_first_action_to_legacy_target(b);return false;}),m.buttons.end());for(auto& c:m.submenus)remove_menu_links(c,ids);}
    void remove_current_menu(){if(root_menu_.id.empty())return;invalidate_reversible_aspect_positions();auto* victim=current_menu();if(!victim)victim=&root_menu_;std::vector<std::string> ids;collect_menu_ids(*victim,ids);first_play_actions_.erase(std::remove_if(first_play_actions_.begin(),first_play_actions_.end(),[&](const NavigationAction& a){return a.kind==NavigationActionKind::Menu&&std::find(ids.begin(),ids.end(),a.target_menu_id)!=ids.end();}),first_play_actions_.end());if(victim==&root_menu_){root_menu_=Menu{};root_menu_.id.clear();root_menu_.name.clear();root_menu_.submenus.clear();current_menu_id_.clear();next_menu_id_=1;mark_modified();refresh_all();load_menu_controls();statusBar()->showMessage("Main menu removed. By default all titles play in order and playback stops at the end.",6000);return;}auto* parent=parent_of(root_menu_,victim->id);if(!parent)return;const auto old=victim->id;parent->submenus.erase(std::remove_if(parent->submenus.begin(),parent->submenus.end(),[&](const Menu& m){return m.id==old;}),parent->submenus.end());remove_menu_links(root_menu_,ids);current_menu_id_=parent->id;mark_modified();refresh_all();load_menu_controls();}
    void add_title_button(){auto* m=current_menu();if(!m||titles_->rowCount()==0)return;const auto targets=title_target_choices();if(targets.empty())return;QStringList choices;for(const auto& t:targets)choices<<t.label;bool ok=false;const auto picked=QInputDialog::getItem(this,"Add title/chapter button","Target",choices,0,false,&ok);if(!ok)return;const qsizetype idx=choices.indexOf(picked);if(idx<0)return;const auto& t=targets[static_cast<std::size_t>(idx)];MenuButton b;b.label=(t.suggested_label.isEmpty()?t.label:t.suggested_label).toStdString();b.target_kind=MenuButtonTargetKind::Title;b.target_title=t.title;b.target_chapter=t.chapter;set_single_button_action(b,action_from_choice(t));b.bounds=clamp_rect_to_aperture(default_bounds(m->buttons.size()),stored_menu_aspect(displayed_target_));invalidate_reversible_aspect_positions();m->buttons.push_back(std::move(b));mark_modified();refresh_button_list();refresh_canvas();}
    void add_stream_button(){auto* m=current_menu();if(!m)return;const auto targets=stream_target_choices();if(targets.empty()){QMessageBox::information(this,"Audio/subtitle button","No selectable audio or subtitle tracks were found. BDMV Author uses ffprobe to inspect the source titles; supported subtitle sources are PGS and common text subtitle formats.");return;}QStringList choices;for(const auto& t:targets)choices<<t.label;bool ok=false;const auto picked=QInputDialog::getItem(this,"Add audio/subtitle button","Action",choices,0,false,&ok);if(!ok)return;const qsizetype idx=choices.indexOf(picked);if(idx<0)return;const auto& t=targets[static_cast<std::size_t>(idx)];MenuButton b;b.label=(t.suggested_label.isEmpty()?t.label:t.suggested_label).toStdString();b.target_kind=t.kind;b.target_title=t.title;b.target_chapter=t.chapter;b.target_stream=t.stream;b.target_menu_id=t.menu_id;set_single_button_action(b,action_from_choice(t));b.bounds=clamp_rect_to_aperture(default_bounds(m->buttons.size()),stored_menu_aspect(displayed_target_));invalidate_reversible_aspect_positions();m->buttons.push_back(std::move(b));mark_modified();refresh_button_list();refresh_canvas();}
    void add_menu_link(){auto* m=current_menu();if(!m)return;std::vector<const Menu*> menus;std::function<void(const Menu&)> add=[&](const Menu& x){if(x.id!=m->id)menus.push_back(&x);for(const auto& c:x.submenus)add(c);};add(root_menu_);if(menus.empty())return;QStringList choices;for(const auto* x:menus)choices<<QString::fromStdString(x->name+" ["+x->id+"]");bool ok=false;const auto picked=QInputDialog::getItem(this,"Add menu link","Target menu",choices,0,false,&ok);if(!ok)return;const qsizetype idx=choices.indexOf(picked);if(idx<0)return;MenuButton b;b.label=menus[static_cast<std::size_t>(idx)]->name;b.target_kind=MenuButtonTargetKind::Menu;b.target_menu_id=menus[static_cast<std::size_t>(idx)]->id;NavigationAction a;a.kind=NavigationActionKind::Menu;a.target_menu_id=b.target_menu_id;set_single_button_action(b,std::move(a));b.bounds=clamp_rect_to_aperture(default_bounds(m->buttons.size()),stored_menu_aspect(displayed_target_));invalidate_reversible_aspect_positions();m->buttons.push_back(std::move(b));mark_modified();refresh_button_list();refresh_canvas();}
    void add_text_label(){auto* m=current_menu();if(!m)return;MenuOverlay o;o.kind=MenuOverlayKind::Text;o.text="Text label";o.bounds={320,240+static_cast<int>(m->overlays.size())*200,1440,160};o.font_family=project_defaults_.label_font_family;o.font_size_px=project_defaults_.label_font_size_px;o.bold=project_defaults_.label_bold;o.italic=project_defaults_.label_italic;OverlayPropertiesDialog d(this,o);if(d.exec()!=QDialog::Accepted)return;{auto value=d.value();value.bounds=clamp_rect_to_aperture(value.bounds,stored_menu_aspect(displayed_target_));invalidate_reversible_aspect_positions();m->overlays.push_back(std::move(value));}mark_modified();refresh_overlay_list();refresh_canvas();}
    void add_image_label(){auto* m=current_menu();if(!m)return;const auto f=QFileDialog::getOpenFileName(this,"Menu label image");if(f.isEmpty())return;MenuOverlay o;o.kind=MenuOverlayKind::Image;o.image=f.toStdString();o.bounds={320,240+static_cast<int>(m->overlays.size())*240,960,240};OverlayPropertiesDialog d(this,o);if(d.exec()!=QDialog::Accepted)return;{auto value=d.value();value.bounds=clamp_rect_to_aperture(value.bounds,stored_menu_aspect(displayed_target_));invalidate_reversible_aspect_positions();m->overlays.push_back(std::move(value));}mark_modified();refresh_overlay_list();refresh_canvas();}
    void edit_selected_label(){auto* m=current_menu();const int row=overlay_list_->currentRow();if(!m||row<0||static_cast<std::size_t>(row)>=m->overlays.size())return;OverlayPropertiesDialog d(this,m->overlays[static_cast<std::size_t>(row)]);if(d.exec()!=QDialog::Accepted)return;{auto value=d.value();value.bounds=clamp_rect_to_aperture(value.bounds,stored_menu_aspect(displayed_target_));invalidate_reversible_aspect_positions();m->overlays[static_cast<std::size_t>(row)]=std::move(value);}mark_modified();refresh_overlay_list();refresh_canvas();}
    void remove_selected_label(){auto* m=current_menu();const int row=overlay_list_->currentRow();if(!m||row<0||static_cast<std::size_t>(row)>=m->overlays.size())return;invalidate_reversible_aspect_positions();m->overlays.erase(m->overlays.begin()+row);mark_modified();refresh_overlay_list();refresh_canvas();}
    void edit_selected_button(){auto* m=current_menu();const int row=button_list_->currentRow();if(!m||row<0||static_cast<std::size_t>(row)>=m->buttons.size())return;const auto e=effective_menu(*m);ButtonPropertiesDialog d(this,m->buttons[static_cast<std::size_t>(row)],e.default_button_style,target_choices());if(d.exec()!=QDialog::Accepted)return;{auto value=d.value();value.bounds=clamp_rect_to_aperture(value.bounds,stored_menu_aspect(displayed_target_));invalidate_reversible_aspect_positions();m->buttons[static_cast<std::size_t>(row)]=std::move(value);}mark_modified();refresh_button_list();refresh_canvas();}
    void remove_selected_button(){auto* m=current_menu();const int row=button_list_->currentRow();if(!m||row<0||static_cast<std::size_t>(row)>=m->buttons.size())return;invalidate_reversible_aspect_positions();m->buttons.erase(m->buttons.begin()+row);mark_modified();refresh_button_list();refresh_canvas();}
    void edit_back_button(){auto* m=current_menu();auto* parent=parent_of(root_menu_,current_menu_id_);if(!m||!parent)return;const auto e=effective_menu(*m);TargetChoice t;t.label="Parent menu — "+QString::fromStdString(parent->name);t.kind=MenuButtonTargetKind::Menu;t.menu_id=parent->id;auto initial=m->back_button;set_single_button_action(initial,action_from_choice(t));ButtonPropertiesDialog d(this,initial,e.default_button_style,{t},true);if(d.exec()!=QDialog::Accepted)return;m->back_button=d.value();m->back_button.bounds=clamp_rect_to_aperture(m->back_button.bounds,stored_menu_aspect(displayed_target_));invalidate_reversible_aspect_positions();set_single_button_action(m->back_button,action_from_choice(t));mark_modified();refresh_canvas();}
    void edit_startup_sequence(){const bool menuless=root_menu_.id.empty();auto initial=first_play_actions_;if(menuless&&initial.empty())initial=default_menuless_startup_actions(static_cast<std::size_t>(titles_->rowCount()));ActionSequenceDialog d(this,"Disc startup / First Playback sequence",initial,target_choices());if(d.exec()!=QDialog::Accepted)return;first_play_actions_=d.value();if(menuless&&is_default_menuless_startup_sequence(first_play_actions_,static_cast<std::size_t>(titles_->rowCount())))first_play_actions_.clear();mark_modified();statusBar()->showMessage(menuless?(first_play_actions_.empty()?"Menu-less default: play all titles in order, then stop.":"Menu-less startup sequence updated; playback stops when the sequence ends."):(first_play_actions_.empty()?"Disc will start at the top menu.":"Disc startup sequence updated."),5000);}
    Project snapshot_project() {
        store_visible_menu_mode();Project p;p.volume_label=volume_->text().toStdString();p.output_image=output_->text().toStdString();p.menu=root_menu_;p.target=displayed_target_;p.frame_rate=frame_rate_->currentData().toString().toStdString();p.menu_resolution=menu_hd_resolution_;p.menu_uhd_resolution=menu_uhd_resolution_;p.menu_dvd_resolution=menu_dvd_resolution_;p.menu_aspect_ratio=menu_hd_aspect_;p.menu_uhd_aspect_ratio=menu_uhd_aspect_;p.menu_dvd_aspect_ratio=menu_dvd_aspect_;p.disc_capacity_bytes=selected_disc_capacity_bytes();p.automatic_video_bitrate=automatic_bitrate_?automatic_bitrate_->isChecked():true;p.use_encode_cache=cache_enabled_state_;p.use_compliance_cache=compliance_cache_enabled_state_;p.refresh_encode_cache=refresh_encode_cache_state_;p.refresh_compliance_cache=refresh_compliance_cache_state_;p.force_reencode=false;p.first_play_actions=first_play_actions_;
        for(int r=0;r<titles_->rowCount();++r){Title t;t.name=titles_->item(r,0)->text().toStdString();t.source=titles_->item(r,1)->text().toStdString();t.encoding=stored_row_profile(r,DiscTarget::BluRay1080);t.uhd_encoding=stored_row_profile(r,DiscTarget::UltraHdBluRay2160);t.dvd_encoding=stored_row_profile(r,DiscTarget::DvdVideo480p);t.frame_rate=stored_row_frame_rate(r,DiscTarget::BluRay1080);t.uhd_frame_rate=stored_row_frame_rate(r,DiscTarget::UltraHdBluRay2160);t.dvd_frame_rate=stored_row_frame_rate(r,DiscTarget::DvdVideo480p);t.resolution=stored_row_resolution(r,DiscTarget::BluRay1080);t.uhd_resolution=stored_row_resolution(r,DiscTarget::UltraHdBluRay2160);t.dvd_resolution=stored_row_resolution(r,DiscTarget::DvdVideo480p);t.aspect_ratio=stored_row_aspect(r,DiscTarget::BluRay1080);t.uhd_aspect_ratio=stored_row_aspect(r,DiscTarget::UltraHdBluRay2160);t.dvd_aspect_ratio=stored_row_aspect(r,DiscTarget::DvdVideo480p);encoding_for_target(t,displayed_target_)=row_profile(r);frame_rate_for_target(t,displayed_target_)=row_frame_rate(r);resolution_for_target(t,displayed_target_)=row_resolution(r);aspect_ratio_for_target(t,displayed_target_)=row_aspect(r);if(auto* force=qobject_cast<QCheckBox*>(titles_->cellWidget(r,10)))t.force_reencode=force->isChecked();if(const auto* item=titles_->item(r,1)){t.audio_language=item->data(RoleAudioLanguage).toString().toStdString();if(t.audio_language.empty())t.audio_language="eng";t.chapter_mode=static_cast<ChapterMode>(item->data(RoleChapterMode).toInt());t.chapter_interval_seconds=item->data(RoleChapterInterval).toDouble();if(t.chapter_interval_seconds<=0.0)t.chapter_interval_seconds=300.0;for(const auto& v:item->data(RoleChapters).toList())t.chapters_seconds.push_back(v.toDouble());t.menu_button_actions=navigation_actions_from_variant(item->data(RoleTitleMenuActions));t.prohibited_user_operations=item->data(RoleTitleUopMask).toULongLong();t.audio_stream_settings=audio_stream_settings_from_variant(item->data(RoleAudioStreamSettings));t.subtitle_stream_settings=subtitle_stream_settings_from_variant(item->data(RoleSubtitleStreamSettings));t.external_subtitles=external_subtitles_from_variant(item->data(RoleExternalSubtitles));t.subtitle_default_style=subtitle_style_from_map(item->data(RoleSubtitleDefaultStyle));t.default_audio_stream=item->data(RoleDefaultAudioStream).toInt();t.default_subtitle_stream=item->data(RoleDefaultSubtitleStream).isValid()?item->data(RoleDefaultSubtitleStream).toInt():-1;}p.titles.push_back(std::move(t));}
        return p;
    }
    static int maximum_menu_number(const Menu& m){int best=0;if(m.id.rfind("menu-",0)==0){int parsed=0;if(parse_integer_exact(std::string_view(m.id).substr(5),parsed))best=parsed;}for(const auto& c:m.submenus)best=std::max(best,maximum_menu_number(c));return best;}
    void edit_new_project_defaults(){DefaultsDialog d(this,saved_defaults_);if(d.exec()!=QDialog::Accepted)return;saved_defaults_=d.value();save_new_project_defaults(saved_defaults_);statusBar()->showMessage("Defaults saved; they will be used by newly created projects.",5000);}
    void new_project(){
        if(!maybe_save_modified())return;
        suppress_modified_=true;project_defaults_=saved_defaults_;Project p;apply_new_project_defaults(p,project_defaults_);p.menu.id="top";p.menu.name="Top Menu";p.menu.inherit_background_image=false;p.menu.inherit_background_color=false;p.menu.inherit_button_style=false;p.menu.inherit_encoding=false;p.menu.inherit_audio=false;p.menu.inherit_duration=false;loading_menu_=true;titles_->setRowCount(0);volume_->setText(QString::fromStdString(p.volume_label));output_->clear();cache_enabled_state_=p.use_encode_cache;compliance_cache_enabled_state_=p.use_compliance_cache;refresh_encode_cache_state_=false;refresh_compliance_cache_state_=false;populate_size_target_choices(p.target,p.disc_capacity_bytes);automatic_bitrate_->setChecked(p.automatic_video_bitrate);load_menu_mode_state(p);displayed_target_=p.target;{const QSignalBlocker blocker(target_);target_->setCurrentIndex(target_->findData(static_cast<int>(displayed_target_)));}populate_frame_rate_choices(p.frame_rate);populate_resolution_choices(menu_resolution_,displayed_target_,stored_menu_resolution(displayed_target_),true);populate_aspect_choices(menu_aspect_,displayed_target_,stored_menu_aspect(displayed_target_),false,stored_menu_resolution(displayed_target_));update_build_button_text();root_menu_=p.menu;initialize_editor_bounds_for_loaded_project();first_play_actions_.clear();current_menu_id_="top";next_menu_id_=1;project_file_.clear();loading_menu_=false;progress_->setValue(0);progress_->setFormat("0.00%");rebuild_menu_tree();load_menu_controls();refresh_all();suppress_modified_=false;mark_clean();refresh_external_tool_availability();
    }
    void populate_project(const Project& p){
        suppress_modified_=true;project_defaults_=saved_defaults_;loading_menu_=true;volume_->setText(QString::fromStdString(p.volume_label));output_->setText(QString::fromStdString(p.output_image.string()));cache_enabled_state_=p.use_encode_cache;compliance_cache_enabled_state_=p.use_compliance_cache;refresh_encode_cache_state_=false;refresh_compliance_cache_state_=false;populate_size_target_choices(p.target,p.disc_capacity_bytes);automatic_bitrate_->setChecked(p.automatic_video_bitrate);load_menu_mode_state(p);displayed_target_=p.target;{const QSignalBlocker blocker(target_);target_->setCurrentIndex(target_->findData(static_cast<int>(displayed_target_)));}populate_frame_rate_choices(p.frame_rate);populate_resolution_choices(menu_resolution_,displayed_target_,stored_menu_resolution(displayed_target_),true);populate_aspect_choices(menu_aspect_,displayed_target_,stored_menu_aspect(displayed_target_),false,stored_menu_resolution(displayed_target_));update_build_button_text();titles_->setRowCount(0);for(const auto& t:p.titles)append_title_row(t);root_menu_=p.menu;concretize_gui_menu_fonts(root_menu_);initialize_editor_bounds_for_loaded_project();first_play_actions_=p.first_play_actions;current_menu_id_=root_menu_.id.empty()?std::string{}:root_menu_.id;next_menu_id_=maximum_menu_number(root_menu_)+1;loading_menu_=false;rebuild_menu_tree();load_menu_controls();refresh_all();suppress_modified_=false;refresh_external_tool_availability();
    }
    bool save_project(bool save_as){
        QString path=project_file_;if(save_as||path.isEmpty())path=QFileDialog::getSaveFileName(this,"Save BDMV Author project",path.isEmpty()?QString("project.bdmvproject"):path,"BDMV Author project (*.bdmvproject)");if(path.isEmpty())return false;if(!path.endsWith(".bdmvproject",Qt::CaseInsensitive))path+=".bdmvproject";
        QString error;if(!save_project_file(this,snapshot_project(),path,&error)){QMessageBox::critical(this,"Could not save project",error);return false;}project_file_=path;mark_clean();statusBar()->showMessage(QString("Saved project %1").arg(path),5000);return true;
    }
    void open_project(){
        const QString path=QFileDialog::getOpenFileName(this,"Open BDMV Author project",project_file_,"BDMV Author project (*.bdmvproject);;JSON files (*.json);;All files (*)");if(path.isEmpty())return;if(!maybe_save_modified())return;QString error;bool font_replacements=false;auto p=load_project_file(this,path,&error,&font_replacements);if(!p){if(!error.isEmpty())QMessageBox::critical(this,"Could not open project",error);return;}populate_project(*p);project_file_=path;if(font_replacements){mark_modified();statusBar()->showMessage(QString("Opened project %1 with replacement fonts; save the project to keep those replacements.").arg(path),8000);}else{mark_clean();statusBar()->showMessage(QString("Opened project %1").arg(path),5000);}
    }
    QString output_dialog_start() const {
        const QString current=output_?output_->text().trimmed():QString();
        if(!current.isEmpty())return current;
        if(!project_file_.isEmpty()){const QFileInfo info(project_file_);return info.absolutePath()+QDir::separator();}
        return {};
    }
    bool choose_output_image(){
        const auto f=QFileDialog::getSaveFileName(this,QStringLiteral("%1 image").arg(output_target_name(displayed_target_)),output_dialog_start(),"ISO image (*.iso);;UDF image (*.udf);;All files (*)");
        if(f.isEmpty())return false;
        if(output_->text()!=f){output_->setText(f);mark_modified();}
        return true;
    }
    void build_project(){
        if(render_active())return;
        if(titles_->rowCount()==0){QMessageBox::warning(this,"BDMV Author","Add at least one video title.");return;}
        if(output_->text().trimmed().isEmpty()&&!choose_output_image())return;
        Project p=snapshot_project();
        const auto problem=build_tool_problem(p);
        if(!problem.isEmpty()){QMessageBox::warning(this,"External tools unavailable",problem);return;}
        const auto build_target=p.target;
        const auto tools=author_tool_paths();
        build_->setEnabled(false);cancel_->setEnabled(true);progress_->setValue(0);progress_->setFormat("0.00%");
        close_after_render_=false;
        cancel_requested_=std::make_shared<std::atomic_bool>(false);
        const auto cancel_token=cancel_requested_;
        watcher_=std::make_unique<QFutureWatcher<BuildResult>>();
        connect(watcher_.get(),&QFutureWatcher<BuildResult>::finished,this,[this,build_target]{
            const BuildResult result=watcher_->result();
            build_->setEnabled(target_available(displayed_target_));cancel_->setEnabled(false);cancel_requested_.reset();
            if(result.cancelled){progress_->setFormat("Cancelled");statusBar()->showMessage("Render cancelled; temporary files cleaned up.",5000);}
            else if(result.error.isEmpty()){progress_->setValue(10000);progress_->setFormat("100.00%");refresh_encode_cache_state_=false;refresh_compliance_cache_state_=false;QMessageBox::information(this,"BDMV Author",QStringLiteral("%1 image created.").arg(output_target_name(build_target)));}
            else QMessageBox::critical(this,"BDMV Author",result.error);
            if(close_after_render_){close_after_render_=false;QTimer::singleShot(0,this,[this]{close();});}
        });
        auto limits=authoring_limits_;
        auto future=QtConcurrent::run([this,p,tools,limits,cancel_token]()->BuildResult{
            try{
                AuthorEngine e(tools,limits);
                e.author(p,[this,cancel_token](double v,const std::string&m){
                    if(cancel_token->load(std::memory_order_acquire))return;
                    if(m.rfind("NONCOMPLIANT:",0)==0)std::cerr<<"["<<std::fixed<<std::setprecision(2)<<v<<"%] "<<m<<'\n'<<std::flush;
                    QMetaObject::invokeMethod(this,[this,v,m,cancel_token]{if(cancel_token->load(std::memory_order_acquire))return;progress_->setValue(static_cast<int>(std::lround(v*100.0)));progress_->setFormat(QString::number(v,'f',2)+"%");statusBar()->showMessage(QString::fromStdString(m));},Qt::QueuedConnection);
                },[cancel_token]{return cancel_token->load(std::memory_order_acquire);});
                return {};
            }catch(const AuthorCancelled&){return {{},true};}
            catch(const std::exception&e){if(cancel_token->load(std::memory_order_acquire))return {{},true};return {QString::fromStdString(e.what()),false};}
        });
        watcher_->setFuture(future);
    }

};

int main(int argc,char**argv){bool debug_mode=false;for(int i=1;i<argc;++i)if(std::string_view(argv[i])=="--debug")debug_mode=true;QApplication app(argc,argv);QCoreApplication::setOrganizationName("BDMV Author");QCoreApplication::setApplicationName("BDMV Author");QGuiApplication::setApplicationDisplayName(QStringLiteral("BDMV Author"));QGuiApplication::setDesktopFileName(QStringLiteral("bdmvauthor"));const QIcon icon=bdmvauthor_app_icon();QApplication::setWindowIcon(icon);try{g_tool_capabilities=probe_tool_capabilities(load_tool_configuration());Window w(debug_mode);w.setWindowIcon(icon);w.show();return app.exec();}catch(const std::exception&e){QMessageBox::critical(nullptr,"BDMV Author startup error",QString::fromStdString(e.what()));return 1;}}
