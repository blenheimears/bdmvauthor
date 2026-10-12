// SPDX-License-Identifier: Apache-2.0
#include "bdmvauthor/author.hpp"
#include "bdmvauthor/font_renderer.hpp"
#include "bdmvauthor/project_file.hpp"
#include "bdmvauthor/menu_layout.hpp"
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
#include <QCryptographicHash>
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
#include <QScrollBar>
#include <QFile>
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
#include <QMap>
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
#include <QProxyStyle>
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
#include <mutex>
#include <initializer_list>
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
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace bdmvauthor;

#include "app_icon_data.inc"

#ifdef Q_OS_MACOS
class PersistentScrollBarStyle final : public QProxyStyle {
public:
    using QProxyStyle::QProxyStyle;
    int styleHint(StyleHint hint,const QStyleOption* option=nullptr,const QWidget* widget=nullptr,QStyleHintReturn* returnData=nullptr) const override {
        if(hint==QStyle::SH_ScrollBar_Transient)return 0;
        return QProxyStyle::styleHint(hint,option,widget,returnData);
    }
};
#endif

namespace {
QIcon bdmvauthor_app_icon(){QPixmap p;p.loadFromData(kBdmvAuthorIconPng,kBdmvAuthorIconPngSize,"PNG");return QIcon(p);}

namespace fs=std::filesystem;

fs::path default_gui_cache_root(){
    if(const char* override_dir=std::getenv("BDMVAUTHOR_CACHE_DIR");override_dir&&*override_dir)return fs::path(override_dir);
    const char* home=std::getenv("HOME");
#ifdef _WIN32
    if(!home||!*home)home=std::getenv("USERPROFILE");
#endif
    if(!home||!*home)return {};
    return fs::path(home)/".cache"/"bdmvauthor"/"encoded-clips";
}

fs::path default_gui_temporary_root(){
    if(const char* override_dir=std::getenv("BDMVAUTHOR_TEMP_DIR");override_dir&&*override_dir)return fs::path(override_dir);
    const auto value=QDir::tempPath();
    return value.isEmpty()?fs::path{}:fs::path(value.toStdString());
}

fs::path gui_configured_storage_path(const char* key,const fs::path& fallback){
    QSettings settings;const auto configured=settings.value(key).toString().trimmed();
    return configured.isEmpty()?fallback:fs::path(configured.toStdString());
}

fs::path gui_cache_root(){return gui_configured_storage_path("storage/cacheDirectory",default_gui_cache_root());}
fs::path gui_temporary_root(){return gui_configured_storage_path("storage/temporaryDirectory",default_gui_temporary_root());}
EncodeCachePolicy gui_encode_cache_policy(){
    QSettings settings;const auto value=settings.value("cache/encodedMediaPolicy",QStringLiteral("enabled")).toString();
    if(value=="hardware-bypass")return EncodeCachePolicy::DisableForHardwareEncodes;
    if(value=="disabled")return EncodeCachePolicy::Disabled;
    return EncodeCachePolicy::Enabled;
}
StoragePaths gui_storage_paths(){return StoragePaths{gui_cache_root(),gui_temporary_root(),gui_encode_cache_policy()};}

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

struct RemoteServerConfiguration {
    QString name;
    bool enabled=true;
    QString ssh,host,user,identity_file;
    int port=22;
    QString operating_system=QStringLiteral("auto");
    QString ffmpeg=QStringLiteral("ffmpeg"),x264=QStringLiteral("x264"),x265=QStringLiteral("x265"),vc1enc=QStringLiteral("vc1enc"),hdmaenc=QStringLiteral("hdmaenc");
    QString h264_provider=QStringLiteral("ffmpeg"),hevc_provider=QStringLiteral("ffmpeg");
    // Friendly per-server hardware API choices. The resolved encoder command
    // names are retained separately for the authoring engine and for backward
    // compatibility with existing application settings.
    QString hardware_encode_backend=QStringLiteral("auto");
    QString hardware_decode_backend=QStringLiteral("software");
    QString h264_hardware_encoder,hevc_hardware_encoder;
    QString hardware_decoder=QStringLiteral("software");
    QString hardware_device;
    int parallel_encodes=1;
};

struct ToolConfiguration {
    QString scaling_filter_backend=QStringLiteral("software");
    QString tonemap_filter_backend=QStringLiteral("software");
    QString filter_hardware_device;
    QString ffmpeg,ffprobe,x264,x265,vc1enc,hdmaenc,tsmuxer,dvdauthor,spumux,mplex,mkisofs,bdsup2sub;
    QString h264_provider=QStringLiteral("ffmpeg");
    QString hevc_provider=QStringLiteral("ffmpeg");
    // FFmpeg hardware backend/API selections are application settings and are
    // deliberately not serialized in .bdmvproject files.
    QString hardware_encode_backend=QStringLiteral("auto");
    QString hardware_decode_backend=QStringLiteral("software");
    // Remote encoder configuration is also application-only state. SSH uses
    // the user's normal key/agent/config authentication; BDMV Author does not
    // store SSH passwords.
    bool remote_encoding=false;
    QString ssh;
    QString remote_host,remote_user,remote_identity_file;
    int remote_ssh_port=22;
    QString remote_ffmpeg=QStringLiteral("ffmpeg"),remote_x264=QStringLiteral("x264"),remote_x265=QStringLiteral("x265"),remote_vc1enc=QStringLiteral("vc1enc"),remote_hdmaenc=QStringLiteral("hdmaenc");
    QList<RemoteServerConfiguration> remote_servers;
    bool local_encoding_enabled=true;
    bool local_fallback_when_remote_unavailable=false;
    // session = never persist worker affinity, local = machine-local companion, project = project JSON.
    QString worker_affinity_persistence=QStringLiteral("local");
    int local_parallel_encodes=1;
    int retry_delay_seconds=60;
    int maximum_retries=15;
    bool retry_forever=false;
    int failover_after_failures=3;
};
void refresh_legacy_remote_fields(ToolConfiguration& c) {
    c.remote_encoding=false;
    c.remote_host.clear();c.remote_user.clear();c.remote_identity_file.clear();c.remote_ssh_port=22;
    for(const auto& r:c.remote_servers){
        if(!r.enabled||r.host.trimmed().isEmpty())continue;
        c.remote_encoding=true;c.ssh=r.ssh;c.remote_host=r.host;c.remote_user=r.user;c.remote_ssh_port=r.port;
        c.remote_identity_file=r.identity_file;c.remote_ffmpeg=r.ffmpeg;c.remote_x264=r.x264;c.remote_x265=r.x265;
        c.remote_vc1enc=r.vc1enc;c.remote_hdmaenc=r.hdmaenc;break;
    }
}

struct ToolCapabilities {
    ToolConfiguration config;
    QString ffmpeg,ffprobe,x264,x265,vc1enc,hdmaenc,tsmuxer,dvdauthor,spumux,mplex,mkisofs,ssh;
    bool ffmpeg_ok=false,ffprobe_ok=false,x264_ok=false,x265_ok=false,vc1enc_ok=false,hdmaenc_ok=false,tsmuxer_ok=false;
    bool dvdauthor_ok=false,spumux_ok=false,mplex_ok=false,mkisofs_ok=false;
    bool ffmpeg_libx264=false,ffmpeg_libx265=false,ffmpeg_mpeg2=false;
    bool ffmpeg_ac3=false,ffmpeg_dca=false,ffmpeg_truehd=false,ffmpeg_pcm_s16le=false,ffmpeg_pcm_s16be=false,ffmpeg_pcm_s24le=false,ffmpeg_pcm_s24be=false;
    QStringList ffmpeg_hwaccels;
    QStringList ffmpeg_video_filters;
    QMap<QString,QString> ffmpeg_h264_hardware_encoders;
    QMap<QString,QString> ffmpeg_hevc_hardware_encoders;
    bool remote_ssh_ok=false,remote_ffmpeg_ok=false,remote_x264_ok=false,remote_x265_ok=false,remote_vc1enc_ok=false,remote_hdmaenc_ok=false;
    bool remote_ffmpeg_libx264=false,remote_ffmpeg_libx265=false,remote_ffmpeg_mpeg2=false;
    QStringList remote_ffmpeg_hwaccels;
    QStringList remote_ffmpeg_video_filters;
    QMap<QString,QString> remote_ffmpeg_h264_hardware_encoders;
    QMap<QString,QString> remote_ffmpeg_hevc_hardware_encoders;
    QString remote_error;
    QStringList warnings;
};
ToolCapabilities g_tool_capabilities;

ToolConfiguration load_tool_configuration(){
    QSettings s;ToolConfiguration c;
    c.ffmpeg=s.value("tools/ffmpeg").toString();c.ffprobe=s.value("tools/ffprobe").toString();
    c.x264=s.value("tools/x264").toString();c.x265=s.value("tools/x265").toString();c.vc1enc=s.value("tools/vc1enc").toString();c.hdmaenc=s.value("tools/hdmaenc").toString();c.tsmuxer=s.value("tools/tsmuxer").toString();
    c.bdsup2sub=s.value("tools/bdsup2sub").toString();
    c.dvdauthor=s.value("tools/dvdauthor").toString();c.spumux=s.value("tools/spumux").toString();c.mplex=s.value("tools/mplex").toString();c.mkisofs=s.value("tools/mkisofs").toString();
    c.h264_provider=s.value("tools/h264Provider",QStringLiteral("ffmpeg")).toString();
    c.hevc_provider=s.value("tools/hevcProvider",QStringLiteral("ffmpeg")).toString();
    c.hardware_encode_backend=s.value("hardware/encodingBackend",QStringLiteral("auto")).toString().trimmed().toLower();
    c.hardware_decode_backend=s.value("hardware/decodingBackend",QStringLiteral("software")).toString().trimmed().toLower();
    c.scaling_filter_backend=s.value("hardware/scalingFilter",QStringLiteral("software")).toString().trimmed().toLower();
    c.tonemap_filter_backend=s.value("hardware/tonemapFilter",QStringLiteral("software")).toString().trimmed().toLower();
    c.filter_hardware_device=s.value("hardware/filterDevice").toString().trimmed();
    c.remote_encoding=s.value("remoteEncoding/enabled",false).toBool();
    c.ssh=s.value("remoteEncoding/ssh").toString();
    c.remote_host=s.value("remoteEncoding/host").toString().trimmed();
    c.remote_user=s.value("remoteEncoding/user").toString().trimmed();
    c.remote_ssh_port=s.value("remoteEncoding/port",22).toInt();
    c.remote_identity_file=s.value("remoteEncoding/identityFile").toString().trimmed();
    c.remote_ffmpeg=s.value("remoteEncoding/ffmpeg",QStringLiteral("ffmpeg")).toString().trimmed();
    c.remote_x264=s.value("remoteEncoding/x264",QStringLiteral("x264")).toString().trimmed();
    c.remote_x265=s.value("remoteEncoding/x265",QStringLiteral("x265")).toString().trimmed();
    c.remote_vc1enc=s.value("remoteEncoding/vc1enc",QStringLiteral("vc1enc")).toString().trimmed();
    c.remote_hdmaenc=s.value("remoteEncoding/hdmaenc",QStringLiteral("hdmaenc")).toString().trimmed();
    c.local_encoding_enabled=s.value("encodingScheduler/localEnabled",true).toBool();
    c.local_fallback_when_remote_unavailable=s.value("encodingScheduler/localFallbackWhenRemoteUnavailable",false).toBool();
    c.worker_affinity_persistence=s.value("encodingScheduler/workerAffinityPersistence",QStringLiteral("local")).toString().trimmed().toLower();
    if(c.worker_affinity_persistence!="project"&&c.worker_affinity_persistence!="session")c.worker_affinity_persistence="local";
    c.local_parallel_encodes=std::clamp(s.value("encodingScheduler/localParallelEncodes",1).toInt(),1,64);
    c.retry_delay_seconds=std::clamp(s.value("encodingScheduler/retryDelaySeconds",60).toInt(),0,86400);
    c.maximum_retries=std::clamp(s.value("encodingScheduler/maximumRetries",15).toInt(),0,1000000);
    c.retry_forever=s.value("encodingScheduler/retryForever",false).toBool();
    c.failover_after_failures=std::clamp(s.value("encodingScheduler/failoverAfterFailures",3).toInt(),1,1000000);
    const int remote_count=s.beginReadArray("remoteServers");
    for(int i=0;i<remote_count;++i){s.setArrayIndex(i);RemoteServerConfiguration r;
        r.name=s.value("name").toString().trimmed();r.enabled=s.value("enabled",true).toBool();r.ssh=s.value("ssh").toString().trimmed();r.host=s.value("host").toString().trimmed();r.user=s.value("user").toString().trimmed();r.port=std::clamp(s.value("port",22).toInt(),1,65535);r.identity_file=s.value("identityFile").toString().trimmed();r.operating_system=s.value("operatingSystem",QStringLiteral("auto")).toString().trimmed().toLower();
        r.ffmpeg=s.value("ffmpeg",QStringLiteral("ffmpeg")).toString().trimmed();r.x264=s.value("x264",QStringLiteral("x264")).toString().trimmed();r.x265=s.value("x265",QStringLiteral("x265")).toString().trimmed();r.vc1enc=s.value("vc1enc",QStringLiteral("vc1enc")).toString().trimmed();r.hdmaenc=s.value("hdmaenc",QStringLiteral("hdmaenc")).toString().trimmed();
        r.h264_provider=s.value("h264Provider",QStringLiteral("ffmpeg")).toString();r.hevc_provider=s.value("hevcProvider",QStringLiteral("ffmpeg")).toString();r.h264_hardware_encoder=s.value("h264HardwareEncoder").toString().trimmed();r.hevc_hardware_encoder=s.value("hevcHardwareEncoder").toString().trimmed();r.hardware_decoder=s.value("hardwareDecoder",QStringLiteral("software")).toString().trimmed().toLower();r.hardware_device=s.value("hardwareDevice").toString().trimmed();r.hardware_encode_backend=s.value("hardwareEncodingBackend").toString().trimmed().toLower();r.hardware_decode_backend=s.value("hardwareDecodingBackend",r.hardware_decoder).toString().trimmed().toLower();r.parallel_encodes=std::clamp(s.value("parallelEncodes",1).toInt(),1,64);
        if(r.operating_system!="windows"&&r.operating_system!="posix")r.operating_system="auto";if(r.h264_provider!="standalone")r.h264_provider="ffmpeg";if(r.hevc_provider!="standalone")r.hevc_provider="ffmpeg";if(r.ffmpeg.isEmpty())r.ffmpeg="ffmpeg";if(r.x264.isEmpty())r.x264="x264";if(r.x265.isEmpty())r.x265="x265";if(r.vc1enc.isEmpty())r.vc1enc="vc1enc";if(r.hdmaenc.isEmpty())r.hdmaenc="hdmaenc";if(r.hardware_encode_backend.isEmpty()){const auto h=r.h264_hardware_encoder.startsWith("h264_")?r.h264_hardware_encoder.mid(5):QString();const auto e=r.hevc_hardware_encoder.startsWith("hevc_")?r.hevc_hardware_encoder.mid(5):QString();r.hardware_encode_backend=(!h.isEmpty()&&h==e)?h:QStringLiteral("auto");}if(r.hardware_decode_backend.isEmpty())r.hardware_decode_backend=r.hardware_decoder.isEmpty()?QStringLiteral("software"):r.hardware_decoder;if(r.hardware_decoder.isEmpty())r.hardware_decoder="software";
        c.remote_servers.push_back(std::move(r));}
    s.endArray();
    // One-time compatibility migration from the previous single-server keys.
    if(c.remote_servers.isEmpty()&&c.remote_encoding&&!c.remote_host.isEmpty()){RemoteServerConfiguration r;r.name=c.remote_host;r.enabled=true;r.ssh=c.ssh;r.host=c.remote_host;r.user=c.remote_user;r.port=c.remote_ssh_port;r.identity_file=c.remote_identity_file;r.ffmpeg=c.remote_ffmpeg;r.x264=c.remote_x264;r.x265=c.remote_x265;r.vc1enc=c.remote_vc1enc;r.hdmaenc=c.remote_hdmaenc;c.remote_servers.push_back(r);}
    // Mirror the first enabled profile into the legacy single-server fields
    // used by capability reporting and CLI compatibility.
    refresh_legacy_remote_fields(c);
    if(c.h264_provider!="standalone")c.h264_provider="ffmpeg";
    if(c.hevc_provider!="standalone")c.hevc_provider="ffmpeg";
    if(c.hardware_encode_backend.isEmpty())c.hardware_encode_backend="auto";
    if(c.hardware_decode_backend.isEmpty())c.hardware_decode_backend="software";
    if(c.scaling_filter_backend!="native" && c.scaling_filter_backend!="vaapi" && c.scaling_filter_backend!="cuda" && c.scaling_filter_backend!="vulkan" && c.scaling_filter_backend!="qsv")c.scaling_filter_backend="software";
    if(c.tonemap_filter_backend!="vaapi" && c.tonemap_filter_backend!="software-fast")c.tonemap_filter_backend="software";
    if(c.remote_ssh_port<1||c.remote_ssh_port>65535)c.remote_ssh_port=22;
    if(c.remote_ffmpeg.isEmpty())c.remote_ffmpeg="ffmpeg";if(c.remote_x264.isEmpty())c.remote_x264="x264";if(c.remote_x265.isEmpty())c.remote_x265="x265";if(c.remote_vc1enc.isEmpty())c.remote_vc1enc="vc1enc";if(c.remote_hdmaenc.isEmpty())c.remote_hdmaenc="hdmaenc";
    return c;
}
void save_tool_configuration(const ToolConfiguration& c){
    QSettings s;s.setValue("tools/ffmpeg",c.ffmpeg);s.setValue("tools/ffprobe",c.ffprobe);s.setValue("tools/x264",c.x264);s.setValue("tools/x265",c.x265);s.setValue("tools/vc1enc",c.vc1enc);s.setValue("tools/hdmaenc",c.hdmaenc);s.setValue("tools/tsmuxer",c.tsmuxer);
    s.setValue("tools/bdsup2sub",c.bdsup2sub);
    s.setValue("tools/dvdauthor",c.dvdauthor);s.setValue("tools/spumux",c.spumux);s.setValue("tools/mplex",c.mplex);s.setValue("tools/mkisofs",c.mkisofs);
    s.setValue("tools/h264Provider",c.h264_provider);s.setValue("tools/hevcProvider",c.hevc_provider);
    s.setValue("hardware/encodingBackend",c.hardware_encode_backend);
    s.setValue("hardware/decodingBackend",c.hardware_decode_backend);
    s.setValue("hardware/scalingFilter",c.scaling_filter_backend);
    s.setValue("hardware/tonemapFilter",c.tonemap_filter_backend);
    s.setValue("hardware/filterDevice",c.filter_hardware_device);
    s.setValue("remoteEncoding/enabled",c.remote_encoding);s.setValue("remoteEncoding/ssh",c.ssh);s.setValue("remoteEncoding/host",c.remote_host);s.setValue("remoteEncoding/user",c.remote_user);s.setValue("remoteEncoding/port",c.remote_ssh_port);s.setValue("remoteEncoding/identityFile",c.remote_identity_file);
    s.setValue("remoteEncoding/ffmpeg",c.remote_ffmpeg);s.setValue("remoteEncoding/x264",c.remote_x264);s.setValue("remoteEncoding/x265",c.remote_x265);s.setValue("remoteEncoding/vc1enc",c.remote_vc1enc);s.setValue("remoteEncoding/hdmaenc",c.remote_hdmaenc);    s.setValue("encodingScheduler/localEnabled",c.local_encoding_enabled);s.setValue("encodingScheduler/localFallbackWhenRemoteUnavailable",c.local_fallback_when_remote_unavailable);s.setValue("encodingScheduler/workerAffinityPersistence",c.worker_affinity_persistence);s.setValue("encodingScheduler/localParallelEncodes",c.local_parallel_encodes);s.setValue("encodingScheduler/retryDelaySeconds",c.retry_delay_seconds);s.setValue("encodingScheduler/maximumRetries",c.maximum_retries);s.setValue("encodingScheduler/retryForever",c.retry_forever);s.setValue("encodingScheduler/failoverAfterFailures",c.failover_after_failures);
    s.beginWriteArray("remoteServers");for(int i=0;i<c.remote_servers.size();++i){s.setArrayIndex(i);const auto&r=c.remote_servers[i];s.setValue("name",r.name);s.setValue("enabled",r.enabled);s.setValue("ssh",r.ssh);s.setValue("host",r.host);s.setValue("user",r.user);s.setValue("port",r.port);s.setValue("identityFile",r.identity_file);s.setValue("operatingSystem",r.operating_system);s.setValue("ffmpeg",r.ffmpeg);s.setValue("x264",r.x264);s.setValue("x265",r.x265);s.setValue("vc1enc",r.vc1enc);s.setValue("hdmaenc",r.hdmaenc);s.setValue("h264Provider",r.h264_provider);s.setValue("hevcProvider",r.hevc_provider);s.setValue("hardwareEncodingBackend",r.hardware_encode_backend);s.setValue("hardwareDecodingBackend",r.hardware_decode_backend);s.setValue("h264HardwareEncoder",r.h264_hardware_encoder);s.setValue("hevcHardwareEncoder",r.hevc_hardware_encoder);s.setValue("hardwareDecoder",r.hardware_decoder);s.setValue("hardwareDevice",r.hardware_device);s.setValue("parallelEncodes",r.parallel_encodes);}s.endArray();
}
QString resolve_tool(const QString& override_path,const QString& command,bool sibling=false){
    const QString specified=override_path.trimmed();
    bool use_sibling=sibling && (specified.isEmpty() || specified.compare(command,Qt::CaseInsensitive)==0
#ifdef _WIN32
        || specified.compare(command+QStringLiteral(".exe"),Qt::CaseInsensitive)==0
#endif
        );
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
        // A saved absolute path from a Nix/developer build can disappear when
        // the relocatable app is moved to another machine or after GC. Keep a
        // valid explicit override authoritative, but recover to the packaged
        // private sibling when the configured path is stale.
        if(sibling){
            QString sibling_name=command;
#ifdef _WIN32
            if(!sibling_name.endsWith(QStringLiteral(".exe"),Qt::CaseInsensitive))sibling_name+=QStringLiteral(".exe");
#endif
            const QString candidate=QCoreApplication::applicationDirPath()+QDir::separator()+sibling_name;
            QFileInfo sibling_info(candidate);if(sibling_info.isFile()&&sibling_info.isExecutable())return sibling_info.absoluteFilePath();
        }
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
QString remote_posix_quote(QString value){
    value.replace('\'',QStringLiteral("'\\''"));
    return QStringLiteral("'")+value+QStringLiteral("'");
}
QString remote_program_name(const QString& configured,const QString& fallback){const auto v=configured.trimmed();return v.isEmpty()?fallback:v;}
QString remote_target(const ToolConfiguration& c){return c.remote_user.trimmed().isEmpty()?c.remote_host.trimmed():c.remote_user.trimmed()+"@"+c.remote_host.trimmed();}
QString remote_target(const RemoteServerConfiguration& c){return c.user.trimmed().isEmpty()?c.host.trimmed():c.user.trimmed()+"@"+c.host.trimmed();}
QString remote_windows_quote(QString value){if(value.contains('"'))return {};return QStringLiteral("\"")+value+QStringLiteral("\"");}
QString remote_command_line(const QString& program,const QStringList& args){QStringList words;words<<remote_posix_quote(program);for(const auto& arg:args)words<<remote_posix_quote(arg);return words.join(' ');}
QString remote_server_command_line(const RemoteServerConfiguration& c,const QString& program,const QStringList& args){const bool windows=c.operating_system=="windows"||(c.operating_system=="auto"&&(program.endsWith(".exe",Qt::CaseInsensitive)||program.contains('\\')));QStringList words;auto quote=[windows](const QString& v){return windows?remote_windows_quote(v):remote_posix_quote(v);};words<<quote(program);for(const auto& arg:args)words<<quote(arg);return words.join(' ');}
QByteArray run_remote_capture(const ToolConfiguration& c,const QString& ssh,const QString& program,const QStringList& remote_args,bool* ok=nullptr,int timeout_ms=10000){
    if(ok)*ok=false;if(ssh.isEmpty()||!c.remote_encoding||c.remote_host.trimmed().isEmpty())return {};
    QStringList args={"-T","-o","BatchMode=yes","-o","ConnectTimeout=8","-o","ServerAliveInterval=15","-p",QString::number(c.remote_ssh_port)};
    if(!c.remote_identity_file.trimmed().isEmpty())args<<"-i"<<c.remote_identity_file.trimmed();
    args<<"--"<<remote_target(c)<<remote_command_line(program,remote_args);
    return run_tool_capture(ssh,args,ok,timeout_ms);
}
QByteArray run_remote_server_capture(const RemoteServerConfiguration& c,const QString& ssh,const QString& program,const QStringList& remote_args,bool* ok=nullptr,int timeout_ms=10000){
    if(ok)*ok=false;if(ssh.isEmpty()||c.host.trimmed().isEmpty())return {};
    QStringList args={"-T","-o","BatchMode=yes","-o","ConnectTimeout=8","-o","ServerAliveInterval=15","-p",QString::number(c.port)};
    if(!c.identity_file.trimmed().isEmpty())args<<"-i"<<c.identity_file.trimmed();
    args<<"--"<<remote_target(c)<<remote_server_command_line(c,program,remote_args);
    return run_tool_capture(ssh,args,ok,timeout_ms);
}

bool output_has_token(const QByteArray& out,const QByteArray& token){
    const auto lines=out.split('\n');for(const auto& line:lines){const auto fields=line.simplified().split(' ');for(const auto& field:fields)if(field==token)return true;}return false;
}
QStringList ffmpeg_encoder_names_with_prefix(const QByteArray& out,const QByteArray& prefix){
    QStringList names;
    for(const auto& line:out.split('\n')){
        for(const auto& field:line.simplified().split(' ')){
            if(field.startsWith(prefix)&&!field.contains('[')&&!field.contains(']')){
                const QString name=QString::fromLatin1(field);
                if(!names.contains(name))names.push_back(name);
            }
        }
    }
    return names;
}
// Filter names are listed by the active FFmpeg executable, not inferred from
// FFmpeg's online documentation (or from the available -hwaccel APIs).
QStringList ffmpeg_video_filter_names(const QByteArray& output) {
    QStringList names;
    for (const auto& raw : output.split('\n')) {
        const auto fields = raw.simplified().split(' ');
        if (fields.size() < 3 || fields.at(0).size() != 3 || fields.at(2) != "V->V") continue;
        const QString name = QString::fromLatin1(fields.at(1));
        if (!names.contains(name)) names.push_back(name);
    }
    return names;
}
// FFmpeg may have a valid filter even when its -filters listing could not be
// parsed or the process timed out. `-h filter=NAME` is a second, targeted
// capability check. Do not rely on the exit status alone: some FFmpeg releases
// exit successfully for "Unknown filter". Require the exact "Filter NAME" line.
bool ffmpeg_filter_help_confirms(const QByteArray& output,const QString& name) {
    const QByteArray expected = QByteArray("Filter ") + name.toLatin1();
    for(const auto& line:output.split('\n'))
        if(line.trimmed()==expected)return true;
    return false;
}
void supplement_local_ffmpeg_filters(QStringList& found,const QString& executable) {
    for(const auto& name:{"tonemap_vaapi","scale_vaapi","scale_qsv","scale_cuda","scale_vulkan"}) {
        const QString filter=QString::fromLatin1(name);
        if(found.contains(filter))continue;
        // Probe the exact FFmpeg binary selected by the GUI, not bare `ffmpeg`
        // from the invoking shell's PATH.
        const auto info=run_tool_capture(executable,{"-hide_banner","-h",QStringLiteral("filter=")+filter},nullptr,5000);
        if(ffmpeg_filter_help_confirms(info,filter))found<<filter;
    }
}
void supplement_remote_ffmpeg_filters(QStringList& found,const ToolConfiguration& config,
                                      const QString& ssh,const QString& executable) {
    // Avoid serial SSH probes for absent optional filters. Recheck a selected
    // backend when the remote -filters listing fails to detect it.
    QStringList selected;
    if(config.tonemap_filter_backend=="vaapi")selected<<"tonemap_vaapi";
    if(config.scaling_filter_backend!="software"&&config.scaling_filter_backend!="native")
        selected<<"scale_"+config.scaling_filter_backend;
    for(const QString& name:selected) {
        if(found.contains(name))continue;
        const auto info=run_remote_capture(config,ssh,executable,{"-hide_banner","-h",QStringLiteral("filter=")+name},nullptr,12000);
        if(ffmpeg_filter_help_confirms(info,name))found<<name;
    }
}
QString hardware_backend_from_encoder_name(const QString& encoder,const QString& codec_prefix){
    return encoder.startsWith(codec_prefix)?encoder.mid(codec_prefix.size()):QString();
}
QStringList ffmpeg_hwaccel_names(const QByteArray& out){
    QStringList result;
    for(const auto& raw:out.split('\n')){
        const auto line=raw.trimmed();
        if(line.isEmpty()||line.startsWith("Hardware acceleration"))continue;
        const auto fields=line.simplified().split(' ');
        if(fields.size()!=1)continue;
        const QString value=QString::fromLatin1(fields.front()).toLower();
        bool valid=!value.isEmpty();
        for(const QChar ch:value)if(!(ch.isLetterOrNumber()||ch=='_'||ch=='-')){valid=false;break;}
        if(valid&&!result.contains(value))result.push_back(value);
    }
    return result;
}
QString hardware_backend_label(const QString& backend){
    const auto b=backend.toLower();
    if(b=="auto")return "Automatic (FFmpeg)";
    if(b=="software")return "Software decoding";
    if(b=="custom")return "Custom override";
    if(b=="nvenc"||b=="cuda")return "NVIDIA CUDA / NVENC";
    if(b=="amf")return "AMD AMF";
    if(b=="qsv")return "Intel Quick Sync Video (QSV)";
    if(b=="vaapi")return "VAAPI";
    if(b=="vdpau")return "VDPAU";
    if(b=="vulkan")return "Vulkan Video";
    if(b=="videotoolbox")return "Apple VideoToolbox";
    if(b=="d3d11va")return "Direct3D 11 Video Acceleration";
    if(b=="d3d12va")return "Direct3D 12 Video Acceleration";
    if(b=="dxva2")return "DirectX Video Acceleration 2 (DXVA2)";
    if(b=="mf")return "Windows Media Foundation";
    if(b=="v4l2m2m")return "V4L2 mem2mem";
    if(b=="mediacodec")return "Android MediaCodec";
    if(b=="drm")return "DRM";
    if(b=="opencl")return "OpenCL";
    return QStringLiteral("FFmpeg %1").arg(backend);
}

struct RemoteHardwareCapabilities {
    bool ssh_ok=false,ffmpeg_ok=false;
    QString ssh,ffmpeg,error;
    QMap<QString,QString> h264_encoders;
    QMap<QString,QString> hevc_encoders;
    QStringList hwaccels;
};
RemoteHardwareCapabilities probe_remote_hardware_capabilities(const RemoteServerConfiguration& r){
    RemoteHardwareCapabilities out;
    if(r.host.trimmed().isEmpty()){out.error="Enter the server name or address before detecting hardware.";return out;}
    out.ssh=resolve_tool(r.ssh,"ssh",false);
    if(out.ssh.isEmpty()){out.error="SSH client was not found. Select an SSH client or install OpenSSH.";return out;}
    const auto fallback=(r.operating_system=="windows")?QStringLiteral("ffmpeg.exe"):QStringLiteral("ffmpeg");
    out.ffmpeg=remote_program_name(r.ffmpeg,fallback);
    bool version_ok=false;run_remote_server_capture(r,out.ssh,out.ffmpeg,{"-hide_banner","-version"},&version_ok,12000);
    out.ssh_ok=version_ok;out.ffmpeg_ok=version_ok;
    if(!version_ok){out.error=QStringLiteral("Could not connect to %1:%2 or run %3. Check SSH authentication, port, and the remote FFmpeg command.").arg(r.host).arg(r.port).arg(out.ffmpeg);return out;}
    bool enc_ok=false;const auto enc=run_remote_server_capture(r,out.ssh,out.ffmpeg,{"-hide_banner","-encoders"},&enc_ok,15000);
    if(enc_ok){
        for(const auto& encoder:ffmpeg_encoder_names_with_prefix(enc,"h264_")){const auto backend=hardware_backend_from_encoder_name(encoder,"h264_");if(!backend.isEmpty())out.h264_encoders.insert(backend,encoder);}
        for(const auto& encoder:ffmpeg_encoder_names_with_prefix(enc,"hevc_")){const auto backend=hardware_backend_from_encoder_name(encoder,"hevc_");if(!backend.isEmpty())out.hevc_encoders.insert(backend,encoder);}
    }
    bool hw_ok=false;const auto hw=run_remote_server_capture(r,out.ssh,out.ffmpeg,{"-hide_banner","-hwaccels"},&hw_ok,12000);if(hw_ok)out.hwaccels=ffmpeg_hwaccel_names(hw);
    if(!enc_ok&&!hw_ok)out.error="Connected successfully, but FFmpeg did not return encoder or hardware-acceleration capabilities.";
    return out;
}
QString preferred_remote_hardware_encoder(const QMap<QString,QString>& encoders){
    static const QStringList preferred={"videotoolbox","nvenc","qsv","amf","vaapi","vulkan","mf","v4l2m2m","mediacodec"};
    for(const auto& backend:preferred)if(encoders.contains(backend))return encoders.value(backend);
    return encoders.isEmpty()?QString():encoders.cbegin().value();
}

ToolCapabilities probe_tool_capabilities(const ToolConfiguration& config){
    ToolCapabilities c;c.config=config;
#if defined(_WIN32) || defined(__APPLE__)
    // Packaged Windows and macOS builds place FFmpeg/ffprobe and the optional
    // standalone encoders beside the GUI executable.  Prefer those private
    // copies before falling back to PATH so Finder-launched .app bundles do
    // not depend on the shell environment or a Nix installation.
    constexpr bool sibling_external_tools=true;
#else
    constexpr bool sibling_external_tools=false;
#endif
    c.ffmpeg=resolve_tool(config.ffmpeg,"ffmpeg",sibling_external_tools);c.ffprobe=resolve_tool(config.ffprobe,"ffprobe",sibling_external_tools);c.x264=resolve_tool(config.x264,"x264",sibling_external_tools);c.x265=resolve_tool(config.x265,"x265",sibling_external_tools);c.ssh=resolve_tool(config.ssh,"ssh",false);
    // vc1enc and hdmaenc are process frontends. They may be bundled, installed
    // separately, or provided only by a remote worker; local discovery must not
    // depend on whether their source trees were present when BDMV Author built.
    c.vc1enc=resolve_tool(config.vc1enc,"vc1enc",true);
    c.hdmaenc=resolve_tool(config.hdmaenc,"hdmaenc",true);
    // These helpers are bundled as private siblings by Windows and portable Linux.
    c.tsmuxer=resolve_tool(config.tsmuxer,"tsmuxer",true);c.dvdauthor=resolve_tool(config.dvdauthor,"dvdauthor",true);c.spumux=resolve_tool(config.spumux,"spumux",true);c.mplex=resolve_tool(config.mplex,"mplex",true);c.mkisofs=resolve_tool(config.mkisofs,"mkisofs",true);
    run_tool_capture(c.ffmpeg,{"-hide_banner","-version"},&c.ffmpeg_ok);
    run_tool_capture(c.ffprobe,{"-hide_banner","-version"},&c.ffprobe_ok);
    if(c.ffmpeg_ok){
        bool enc_ok=false;
        const auto enc=run_tool_capture(c.ffmpeg,{"-hide_banner","-encoders"},&enc_ok,10000);
        if(enc_ok){
            c.ffmpeg_libx264=output_has_token(enc,"libx264");c.ffmpeg_libx265=output_has_token(enc,"libx265");c.ffmpeg_mpeg2=output_has_token(enc,"mpeg2video");c.ffmpeg_ac3=output_has_token(enc,"ac3");c.ffmpeg_dca=output_has_token(enc,"dca");c.ffmpeg_truehd=output_has_token(enc,"truehd");c.ffmpeg_pcm_s16le=output_has_token(enc,"pcm_s16le");c.ffmpeg_pcm_s16be=output_has_token(enc,"pcm_s16be");c.ffmpeg_pcm_s24le=output_has_token(enc,"pcm_s24le");c.ffmpeg_pcm_s24be=output_has_token(enc,"pcm_s24be");
            for(const auto& encoder:ffmpeg_encoder_names_with_prefix(enc,"h264_")){
                const auto backend=hardware_backend_from_encoder_name(encoder,"h264_");
                if(!backend.isEmpty())c.ffmpeg_h264_hardware_encoders.insert(backend,encoder);
            }
            for(const auto& encoder:ffmpeg_encoder_names_with_prefix(enc,"hevc_")){
                const auto backend=hardware_backend_from_encoder_name(encoder,"hevc_");
                if(!backend.isEmpty())c.ffmpeg_hevc_hardware_encoders.insert(backend,encoder);
            }
        }
        bool hw_ok=false;
        const auto hw=run_tool_capture(c.ffmpeg,{"-hide_banner","-hwaccels"},&hw_ok,10000);
        if(hw_ok)c.ffmpeg_hwaccels=ffmpeg_hwaccel_names(hw);
        bool filters_ok=false;
        const auto filters=run_tool_capture(c.ffmpeg,{"-hide_banner","-filters"},&filters_ok,12000);
        if(filters_ok)c.ffmpeg_video_filters=ffmpeg_video_filter_names(filters);
        supplement_local_ffmpeg_filters(c.ffmpeg_video_filters,c.ffmpeg);
    }
    bool x264_run=false;const auto x264_help=run_tool_capture(c.x264,{"--fullhelp"},&x264_run,6000);c.x264_ok=x264_run&&x264_help.contains("--bluray-compat");
    bool x265_run=false;const auto x265_help=run_tool_capture(c.x265,{"--help"},&x265_run,6000);c.x265_ok=x265_run&&x265_help.contains("--uhd-bd")&&x265_help.contains("--output-depth");
    bool vc1_run=false;const auto vc1_help=run_tool_capture(c.vc1enc,{"--help"},&vc1_run,6000);c.vc1enc_ok=vc1_run&&vc1_help.contains("--bluray-compat")&&vc1_help.contains("auto|raw|m2ts")&&vc1_help.contains("--pass-stats")&&vc1_help.contains("--fast");
    bool hdma_run=false;const auto hdma_help=run_tool_capture(c.hdmaenc,{"--help"},&hdma_run,6000);
    c.hdmaenc_ok=hdma_run&&hdma_help.contains("--raw-pcm")&&
        hdma_help.contains("--sample-rate")&&hdma_help.contains("--channel-layout")&&
        hdma_help.contains("--channels")&&hdma_help.contains("--bit-depth")&&
        hdma_help.contains("--lossless")&&hdma_help.contains("--xll-hardware-test");
    if(config.remote_encoding){
        if(config.remote_host.trimmed().isEmpty())c.remote_error="Remote encoding is enabled but no SSH host is configured.";
        else if(c.ssh.isEmpty())c.remote_error="Remote encoding is enabled but no SSH client was found.";
        else{
            bool ok=false;
            const auto ffmpeg=remote_program_name(config.remote_ffmpeg,"ffmpeg");
            run_remote_capture(config,c.ssh,ffmpeg,{"-hide_banner","-version"},&c.remote_ffmpeg_ok,12000);
            c.remote_ssh_ok=c.remote_ffmpeg_ok;
            if(c.remote_ffmpeg_ok){
                bool enc_ok=false;const auto enc=run_remote_capture(config,c.ssh,ffmpeg,{"-hide_banner","-encoders"},&enc_ok,15000);
                if(enc_ok){
                    c.remote_ffmpeg_libx264=output_has_token(enc,"libx264");c.remote_ffmpeg_libx265=output_has_token(enc,"libx265");c.remote_ffmpeg_mpeg2=output_has_token(enc,"mpeg2video");
                    for(const auto& encoder:ffmpeg_encoder_names_with_prefix(enc,"h264_")){const auto backend=hardware_backend_from_encoder_name(encoder,"h264_");if(!backend.isEmpty())c.remote_ffmpeg_h264_hardware_encoders.insert(backend,encoder);}
                    for(const auto& encoder:ffmpeg_encoder_names_with_prefix(enc,"hevc_")){const auto backend=hardware_backend_from_encoder_name(encoder,"hevc_");if(!backend.isEmpty())c.remote_ffmpeg_hevc_hardware_encoders.insert(backend,encoder);}
                }
                bool hw_ok=false;const auto hw=run_remote_capture(config,c.ssh,ffmpeg,{"-hide_banner","-hwaccels"},&hw_ok,12000);if(hw_ok)c.remote_ffmpeg_hwaccels=ffmpeg_hwaccel_names(hw);
                bool filters_ok=false;
                const auto filters=run_remote_capture(config,c.ssh,ffmpeg,{"-hide_banner","-filters"},&filters_ok,12000);
                if(filters_ok)c.remote_ffmpeg_video_filters=ffmpeg_video_filter_names(filters);
                supplement_remote_ffmpeg_filters(c.remote_ffmpeg_video_filters,config,c.ssh,ffmpeg);
                bool x264_run=false;const auto x264_help=run_remote_capture(config,c.ssh,remote_program_name(config.remote_x264,"x264"),{"--fullhelp"},&x264_run,12000);c.remote_x264_ok=x264_run&&x264_help.contains("--bluray-compat");
                bool x265_run=false;const auto x265_help=run_remote_capture(config,c.ssh,remote_program_name(config.remote_x265,"x265"),{"--help"},&x265_run,12000);c.remote_x265_ok=x265_run&&x265_help.contains("--uhd-bd")&&x265_help.contains("--output-depth");
                bool vc1_run=false;const auto vc1_help=run_remote_capture(config,c.ssh,remote_program_name(config.remote_vc1enc,"vc1enc"),{"--help"},&vc1_run,12000);c.remote_vc1enc_ok=vc1_run&&vc1_help.contains("--bluray-compat")&&vc1_help.contains("auto|raw|m2ts")&&vc1_help.contains("--fast");
                bool hdma_run=false;const auto hdma_help=run_remote_capture(config,c.ssh,remote_program_name(config.remote_hdmaenc,"hdmaenc"),{"--help"},&hdma_run,12000);c.remote_hdmaenc_ok=hdma_run&&hdma_help.contains("--raw-pcm")&&hdma_help.contains("--sample-rate")&&hdma_help.contains("--channel-layout")&&hdma_help.contains("--channels")&&hdma_help.contains("--bit-depth")&&hdma_help.contains("--lossless");
            }else c.remote_error="Could not connect to the remote server or run its FFmpeg. Verify SSH authentication, host, port, and the remote FFmpeg command.";
        }
        if(!c.remote_error.isEmpty())c.warnings<<c.remote_error;
    }

    // The remaining helpers use differing/non-zero --help conventions, so an executable path is sufficient here; build-time failures still report the exact command.
    c.tsmuxer_ok=!c.tsmuxer.isEmpty();c.dvdauthor_ok=!c.dvdauthor.isEmpty();c.spumux_ok=!c.spumux.isEmpty();c.mplex_ok=!c.mplex.isEmpty();c.mkisofs_ok=!c.mkisofs.isEmpty();
    if(!c.ffmpeg_ok)c.warnings<<"FFmpeg was not found or could not run; encoding and media conversion are unavailable.";
    else {
        QStringList missing;if(!c.ffmpeg_libx264)missing<<"libx264";if(!c.ffmpeg_libx265)missing<<"libx265";if(!c.ffmpeg_mpeg2)missing<<"MPEG-2 video";if(!c.ffmpeg_ac3)missing<<"AC-3";if(!c.ffmpeg_dca)missing<<"DCA";if(!c.ffmpeg_truehd)missing<<"THD";
        if(!missing.isEmpty())c.warnings<<QStringLiteral("The selected FFmpeg has partial encoder support; missing FFmpeg encoders: %1. Standalone x264/x265 may still provide H.264/H.265.").arg(missing.join(", "));
    }
    if(!c.ffprobe_ok)c.warnings<<"ffprobe was not found; source inspection is unavailable.";
    if(!c.tsmuxer_ok)c.warnings<<"tsMuxer was not found; Blu-ray and Ultra HD Blu-ray builds are unavailable.";
    if(!c.vc1enc_ok)c.warnings<<"Local vc1enc was not found or lacks the expected frontend options; Local VC-1 encoding is unavailable (remote workers may still provide it).";
    if(!c.hdmaenc_ok)c.warnings<<"Local hdmaenc was not found or lacks the raw PCM lossless frontend; Local libhdma encoding is unavailable (remote workers may still provide it).";
    const bool remote_active=config.remote_encoding&&c.remote_ffmpeg_ok;const bool active_mpeg2=remote_active?c.remote_ffmpeg_mpeg2:c.ffmpeg_mpeg2;const bool active_x264=remote_active?c.remote_x264_ok:c.x264_ok;const bool active_x265=remote_active?c.remote_x265_ok:c.x265_ok;const bool active_libx264=remote_active?c.remote_ffmpeg_libx264:c.ffmpeg_libx264;const bool active_libx265=remote_active?c.remote_ffmpeg_libx265:c.ffmpeg_libx265;const auto& active_h264_hw=remote_active?c.remote_ffmpeg_h264_hardware_encoders:c.ffmpeg_h264_hardware_encoders;const auto& active_hevc_hw=remote_active?c.remote_ffmpeg_hevc_hardware_encoders:c.ffmpeg_hevc_hardware_encoders;
    if(!active_mpeg2)c.warnings<<(remote_active?"Remote FFmpeg MPEG-2 encoding is unavailable, so DVD-Video authoring is disabled while remote encoding is enabled.":"FFmpeg MPEG-2 encoding is unavailable, so DVD-Video authoring is disabled.");
    if(!c.dvdauthor_ok||!c.spumux_ok||!c.mplex_ok||!c.mkisofs_ok)c.warnings<<"One or more DVD helpers (dvdauthor, spumux, mplex, mkisofs) are missing; DVD-Video authoring is disabled.";
    if(!active_libx265&&!active_x265)c.warnings<<"HEVC/H.265 encoding is unavailable. Ultra HD Blu-ray remains available only for legal 1080p AVC/H.264 projects when H.264 is available.";
    if(config.h264_provider=="ffmpeg"&&!active_libx264&&active_x264)c.warnings<<"FFmpeg was preferred for H.264 but lacks libx264; standalone x264 will be used instead.";
    if(config.h264_provider=="standalone"&&!active_x264&&active_libx264)c.warnings<<"Standalone x264 was preferred but is unavailable; FFmpeg/libx264 will be used instead.";
    if(config.hevc_provider=="ffmpeg"&&!active_libx265&&active_x265)c.warnings<<"FFmpeg was preferred for H.265 but lacks libx265; standalone x265 will be used instead.";
    if(config.hevc_provider=="standalone"&&!active_x265&&active_libx265)c.warnings<<"Standalone x265 was preferred but is unavailable; FFmpeg/libx265 will be used instead.";
    if(config.hardware_encode_backend!="auto"&&!active_h264_hw.contains(config.hardware_encode_backend)&&!active_hevc_hw.contains(config.hardware_encode_backend))c.warnings<<QStringLiteral("The selected hardware encoding API (%1) is not exposed by the %2 FFmpeg build. Hardware codec choices will be unavailable until another API is selected.").arg(hardware_backend_label(config.hardware_encode_backend),remote_active?QStringLiteral("remote"):QStringLiteral("local"));
    const auto& active_hwaccels=remote_active?c.remote_ffmpeg_hwaccels:c.ffmpeg_hwaccels;
    if(config.hardware_decode_backend!="software"&&config.hardware_decode_backend!="auto"&&!active_hwaccels.contains(config.hardware_decode_backend))c.warnings<<QStringLiteral("The selected hardware decoding API (%1) is not exposed by the %2 FFmpeg build. Input decoding will fall back to software.").arg(hardware_backend_label(config.hardware_decode_backend),remote_active?QStringLiteral("remote"):QStringLiteral("local"));
    const auto& active_filters=remote_active?c.remote_ffmpeg_video_filters:c.ffmpeg_video_filters;
    if(config.scaling_filter_backend!="software"&&config.scaling_filter_backend!="native"&&
       !active_filters.contains("scale_"+config.scaling_filter_backend))
        c.warnings<<QStringLiteral("Could not verify scaling filter scale_%1 on the selected %2 FFmpeg executable. It will be attempted at encode time, with software fallback and full FFmpeg diagnostics on failure.").arg(config.scaling_filter_backend,remote_active?QStringLiteral("remote"):QStringLiteral("local"));
    if(config.tonemap_filter_backend=="vaapi"&&!active_filters.contains("tonemap_vaapi"))
        c.warnings<<QStringLiteral("Could not verify tonemap_vaapi on the selected %1 FFmpeg executable. BDMV Author will attempt it and use software fallback only if FFmpeg rejects it.").arg(remote_active?QStringLiteral("remote"):QStringLiteral("local"));
    return c;
}
bool remote_encoding_active(){return g_tool_capabilities.config.remote_encoding&&g_tool_capabilities.remote_ffmpeg_ok;} // legacy first-server status only

bool local_worker_eligible_for_scheduler(){
    const auto& c=g_tool_capabilities.config;
    return c.local_encoding_enabled||c.local_fallback_when_remote_unavailable;
}
bool remote_worker_pool_enabled(){
    const auto& c=g_tool_capabilities.config;
    return std::any_of(c.remote_servers.cbegin(),c.remote_servers.cend(),[](const auto& r){return r.enabled&&!r.host.trimmed().isEmpty();});
}
const QMap<QString,QString>& active_h264_hardware_encoders(){return g_tool_capabilities.ffmpeg_h264_hardware_encoders;}
const QMap<QString,QString>& active_hevc_hardware_encoders(){return g_tool_capabilities.ffmpeg_hevc_hardware_encoders;}
QString resolved_hardware_encoder(bool h264){
    // These fields describe Local only. Remote workers carry their own resolved
    // hardware encoder names in RemoteEncodingServer.
    const auto& map=h264?g_tool_capabilities.ffmpeg_h264_hardware_encoders:g_tool_capabilities.ffmpeg_hevc_hardware_encoders;
    if(map.isEmpty())return {};
    const auto selected=g_tool_capabilities.config.hardware_encode_backend;
    if(selected!="auto")return map.value(selected);
    QStringList preferred;
#if defined(__APPLE__)
    preferred={"videotoolbox","nvenc","qsv","amf","vaapi","vulkan","mf","v4l2m2m"};
#elif defined(_WIN32)
    preferred={"nvenc","qsv","amf","mf","vulkan","vaapi","v4l2m2m","videotoolbox"};
#else
    preferred={"vaapi","nvenc","qsv","vulkan","v4l2m2m","amf","videotoolbox","mf"};
#endif
    for(const auto& backend:preferred)if(map.contains(backend))return map.value(backend);
    return map.first();
}
QString resolved_hardware_decode_backend(){
    // The top-level decode setting is Local's setting. Each remote server has
    // an independent hardware_decoder value.
    const auto selected=g_tool_capabilities.config.hardware_decode_backend;
    const auto& available=g_tool_capabilities.ffmpeg_hwaccels;
    if(selected=="software")return {};
    if(selected=="auto")return available.isEmpty()?QString():QStringLiteral("auto");
    return available.contains(selected)?selected:QString();
}
bool local_h264_available(){return g_tool_capabilities.ffmpeg_libx264||g_tool_capabilities.x264_ok;}
bool local_hevc_available(){return g_tool_capabilities.ffmpeg_libx265||g_tool_capabilities.x265_ok;}
bool local_h264_hardware_available(){return !resolved_hardware_encoder(true).isEmpty();}
bool local_hevc_hardware_available(){return !resolved_hardware_encoder(false).isEmpty();}
bool local_vc1_available(){return g_tool_capabilities.vc1enc_ok;}
bool local_hdma_available(){return g_tool_capabilities.hdmaenc_ok;}
bool local_mpeg2_available(){return g_tool_capabilities.ffmpeg_mpeg2;}

bool any_remote_hardware_encoder_configured(bool h264){
    const auto& c=g_tool_capabilities.config;
    for(const auto& r:c.remote_servers){
        if(!r.enabled||r.host.trimmed().isEmpty())continue;
        if(!(h264?r.h264_hardware_encoder:r.hevc_hardware_encoder).trimmed().isEmpty())return true;
    }
    return false;
}

// GUI availability is scheduler-wide. A codec must not be greyed merely
// because the first/preferred remote worker lacks it: capability preflight
// will choose another worker, and fallback-only Local counts when enabled.
bool h264_available(){return (local_worker_eligible_for_scheduler()&&local_h264_available())||remote_worker_pool_enabled();}
bool hevc_available(){return (local_worker_eligible_for_scheduler()&&local_hevc_available())||remote_worker_pool_enabled();}
bool h264_hardware_available(){return (local_worker_eligible_for_scheduler()&&local_h264_hardware_available())||any_remote_hardware_encoder_configured(true);}
bool hevc_hardware_available(){return (local_worker_eligible_for_scheduler()&&local_hevc_hardware_available())||any_remote_hardware_encoder_configured(false);}
bool vc1_available(){return (local_worker_eligible_for_scheduler()&&local_vc1_available())||remote_worker_pool_enabled();}
bool hdma_available(){return (local_worker_eligible_for_scheduler()&&local_hdma_available())||remote_worker_pool_enabled();}
bool active_mpeg2_available(){return (local_worker_eligible_for_scheduler()&&local_mpeg2_available())||remote_worker_pool_enabled();}

bool video_codec_available(DiscTarget target,VideoCodec codec){
    if(!video_codec_allowed_for_target(target,codec))return false;
    // Local FFmpeg/ffprobe provide source demux/probe even for SSH workers.
    if(!g_tool_capabilities.ffmpeg_ok||!g_tool_capabilities.ffprobe_ok)return false;
    if(codec==VideoCodec::X264)return h264_available();
    if(codec==VideoCodec::H264Hardware)return h264_hardware_available();
    if(codec==VideoCodec::Hevc)return hevc_available();
    if(codec==VideoCodec::HevcHardware)return hevc_hardware_available();
    if(codec==VideoCodec::Vc1)return vc1_available();
    return active_mpeg2_available();
}
bool audio_codec_available(DiscTarget target,AudioCodec codec){
    if(!audio_codec_allowed_for_target(target,codec)||!g_tool_capabilities.ffmpeg_ok)return false;
    const bool local=local_worker_eligible_for_scheduler();
    const bool remote=remote_worker_pool_enabled();
    switch(codec){
        case AudioCodec::Ac3:return (local&&g_tool_capabilities.ffmpeg_ac3)||remote;
        case AudioCodec::Dca:return (local&&g_tool_capabilities.ffmpeg_dca)||remote;
        case AudioCodec::Libhdma:
            return (local&&local_hdma_available()&&g_tool_capabilities.ffmpeg_pcm_s16le&&g_tool_capabilities.ffmpeg_pcm_s24le)||remote;
        case AudioCodec::TrueHdAc3:return (local&&g_tool_capabilities.ffmpeg_truehd&&g_tool_capabilities.ffmpeg_ac3)||remote;
        case AudioCodec::Lpcm:return (local&&(target==DiscTarget::DvdVideo480p?g_tool_capabilities.ffmpeg_pcm_s16be:g_tool_capabilities.ffmpeg_pcm_s16le))||remote;
    }
    return false;
}
bool target_available(DiscTarget target){
    if(!g_tool_capabilities.ffmpeg_ok||!g_tool_capabilities.ffprobe_ok)return false;
    if(target==DiscTarget::DvdVideo480p)return active_mpeg2_available()&&g_tool_capabilities.dvdauthor_ok&&g_tool_capabilities.spumux_ok&&g_tool_capabilities.mplex_ok&&g_tool_capabilities.mkisofs_ok;
    if(!g_tool_capabilities.tsmuxer_ok)return false;
    if(target==DiscTarget::Avchd)return h264_available()||h264_hardware_available();
    if(target==DiscTarget::UltraHdBluRay2160)return hevc_available()||hevc_hardware_available()||h264_available()||h264_hardware_available();
    return h264_available()||h264_hardware_available()||active_mpeg2_available()||vc1_available();
}
VideoEncoderProvider resolved_h264_provider(){
    const bool standalone_first=g_tool_capabilities.config.h264_provider=="standalone";
    const bool standalone=g_tool_capabilities.x264_ok;
    const bool ffmpeg=g_tool_capabilities.ffmpeg_libx264;
    if(standalone_first&&standalone)return VideoEncoderProvider::Standalone;
    if(ffmpeg)return VideoEncoderProvider::Ffmpeg;
    return VideoEncoderProvider::Standalone;
}
VideoEncoderProvider resolved_hevc_provider(){
    const bool standalone_first=g_tool_capabilities.config.hevc_provider=="standalone";
    const bool standalone=g_tool_capabilities.x265_ok;
    const bool ffmpeg=g_tool_capabilities.ffmpeg_libx265;
    if(standalone_first&&standalone)return VideoEncoderProvider::Standalone;
    if(ffmpeg)return VideoEncoderProvider::Ffmpeg;
    return VideoEncoderProvider::Standalone;
}
ToolPaths author_tool_paths(){
    ToolPaths t;t.ffmpeg=g_tool_capabilities.ffmpeg.toStdString();t.ffprobe=g_tool_capabilities.ffprobe.toStdString();t.x264=g_tool_capabilities.x264.toStdString();t.x265=g_tool_capabilities.x265.toStdString();t.vc1enc=g_tool_capabilities.vc1enc.toStdString();t.hdmaenc=g_tool_capabilities.hdmaenc.toStdString();t.tsmuxer=g_tool_capabilities.tsmuxer.toStdString();t.dvdauthor=g_tool_capabilities.dvdauthor.toStdString();t.spumux=g_tool_capabilities.spumux.toStdString();t.bdsup2sub=g_tool_capabilities.config.bdsup2sub.toStdString();t.mplex=g_tool_capabilities.mplex.toStdString();t.mkisofs=g_tool_capabilities.mkisofs.toStdString();t.h264_provider=resolved_h264_provider();t.hevc_provider=resolved_hevc_provider();
    t.h264_hardware_encoder=resolved_hardware_encoder(true).toStdString();t.hevc_hardware_encoder=resolved_hardware_encoder(false).toStdString();t.hardware_decoder=resolved_hardware_decode_backend().toStdString();
    t.scaling_filter_backend=g_tool_capabilities.config.scaling_filter_backend.toStdString();
    t.tonemap_filter_backend=g_tool_capabilities.config.tonemap_filter_backend.toStdString();
    t.filter_hardware_device=g_tool_capabilities.config.filter_hardware_device.toStdString();
    const auto& c=g_tool_capabilities.config;t.remote_encoding=false;t.ssh=g_tool_capabilities.ssh.toStdString();
    t.scheduler.local_enabled=c.local_encoding_enabled;t.scheduler.local_fallback_when_remote_unavailable=c.local_fallback_when_remote_unavailable;t.scheduler.local_parallel_encodes=c.local_parallel_encodes;t.scheduler.retry_delay_seconds=c.retry_delay_seconds;t.scheduler.maximum_retries=c.maximum_retries;t.scheduler.retry_forever=c.retry_forever;t.scheduler.failover_after_consecutive_failures=c.failover_after_failures;
    for(const auto&r:c.remote_servers){if(!r.enabled||r.host.trimmed().isEmpty())continue;RemoteEncodingServer server;server.enabled=true;server.name=(r.name.trimmed().isEmpty()?r.host:r.name).toStdString();server.ssh=(r.ssh.trimmed().isEmpty()?g_tool_capabilities.ssh:r.ssh).toStdString();server.host=r.host.toStdString();server.user=r.user.toStdString();server.port=r.port;server.operating_system=r.operating_system=="windows"?RemoteOperatingSystem::Windows:r.operating_system=="posix"?RemoteOperatingSystem::Posix:RemoteOperatingSystem::Auto;server.identity_file=r.identity_file.toStdString();server.ffmpeg=remote_program_name(r.ffmpeg,"ffmpeg").toStdString();server.x264=remote_program_name(r.x264,"x264").toStdString();server.x265=remote_program_name(r.x265,"x265").toStdString();server.vc1enc=remote_program_name(r.vc1enc,"vc1enc").toStdString();server.hdmaenc=remote_program_name(r.hdmaenc,"hdmaenc").toStdString();server.h264_provider=r.h264_provider=="standalone"?VideoEncoderProvider::Standalone:VideoEncoderProvider::Ffmpeg;server.hevc_provider=r.hevc_provider=="standalone"?VideoEncoderProvider::Standalone:VideoEncoderProvider::Ffmpeg;server.h264_hardware_encoder=r.h264_hardware_encoder.toStdString();server.hevc_hardware_encoder=r.hevc_hardware_encoder.toStdString();server.hardware_decoder=r.hardware_decoder=="software"?std::string{}:r.hardware_decoder.toStdString();server.hardware_device=r.hardware_device.toStdString();server.parallel_encodes=r.parallel_encodes;t.remote_servers.push_back(std::move(server));}
    return t;
}
void set_combo_item_enabled(QComboBox* c,int row,bool enabled,const QString& unavailable_reason={}){
    if(auto* model=qobject_cast<QStandardItemModel*>(c->model()))if(auto* item=model->item(row)){item->setEnabled(enabled);if(!enabled&&!unavailable_reason.isEmpty())item->setToolTip(unavailable_reason);}
}
void populate_video_combo(QComboBox* c, DiscTarget target, VideoCodec selected) {
    const QSignalBlocker blocker(c);
    c->clear();
    if (target == DiscTarget::BluRay1080) {
        c->addItem("x264 / H.264", static_cast<int>(VideoCodec::X264));
        c->addItem("h.264 / AVC (hardware)", static_cast<int>(VideoCodec::H264Hardware));
        c->addItem("FFmpeg MPEG-2", static_cast<int>(VideoCodec::Mpeg2));
        c->addItem("libvc1 VC-1 (Experimental)", static_cast<int>(VideoCodec::Vc1));
    } else if (target == DiscTarget::Avchd) {
        c->addItem("x264 / AVC (AVCHD)", static_cast<int>(VideoCodec::X264));
        c->addItem("h.264 / AVC (hardware)", static_cast<int>(VideoCodec::H264Hardware));
    } else if (target == DiscTarget::UltraHdBluRay2160) {
        c->addItem("HEVC / H.265 (UHD)", static_cast<int>(VideoCodec::Hevc));
        c->addItem("h.265 / HEVC (hardware)", static_cast<int>(VideoCodec::HevcHardware));
        c->addItem("AVC / H.264 (1080p23.976/24 only)", static_cast<int>(VideoCodec::X264));
        c->addItem("h.264 / AVC (hardware)", static_cast<int>(VideoCodec::H264Hardware));
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
    if(target != DiscTarget::DvdVideo480p && target != DiscTarget::Avchd){
        c->addItem("DCA", static_cast<int>(AudioCodec::Dca));
        c->addItem("libhdma (lossless, experimental)", static_cast<int>(AudioCodec::Libhdma));
        c->addItem("THD + AC-3 core (experimental)", static_cast<int>(AudioCodec::TrueHdAc3));
    }
    for(int row=0;row<c->count();++row){const auto codec=static_cast<AudioCodec>(c->itemData(row).toInt());set_combo_item_enabled(c,row,audio_codec_available(target,codec),QStringLiteral("The selected audio encoder or required FFmpeg PCM output is unavailable."));}
    int index=c->findData(static_cast<int>(selected));
    if(index<0)index=0;
    c->setCurrentIndex(index);
}
QComboBox* audio_combo(QWidget* parent, DiscTarget target, AudioCodec selected) {
    auto* c=new QComboBox(parent);populate_audio_combo(c,target,selected);return c;
}
QString encoding_preset(const EncodingProfile& e) {
    if(video_codec_is_hardware(e.video_codec))return QStringLiteral("Hardware default");
    if(e.video_codec==VideoCodec::Hevc)return QString::fromStdString(e.x265_preset);
    if(e.video_codec==VideoCodec::Vc1)return QString::fromStdString(e.vc1_preset);
    return QString::fromStdString(e.x264_preset);
}
void populate_preset_combo(QComboBox* c,VideoCodec codec,const QString& selected={}) {
    const QSignalBlocker blocker(c);c->clear();
    if(video_codec_is_hardware(codec)){c->addItem("Hardware default");c->setToolTip("Hardware encoder quality/speed is controlled by the selected FFmpeg backend and its driver.");}
    else if(codec==VideoCodec::Vc1){for(const char* p:{"normal","fast"})c->addItem(QString::fromLatin1(p));c->setToolTip("vc1enc speed preset (libvc1 VC-1, experimental)");}
    else{for(const char* p:{"ultrafast","superfast","veryfast","faster","fast","medium","slow","slower","veryslow","placebo"})c->addItem(QString::fromLatin1(p));c->setToolTip("Encoder speed preset");}
    const QString wanted=video_codec_is_hardware(codec)?QStringLiteral("Hardware default"):selected.isEmpty()?(codec==VideoCodec::Vc1?QStringLiteral("normal"):QStringLiteral("medium")):selected;
    const int row=c->findText(wanted);c->setCurrentIndex(row>=0?row:(video_codec_is_hardware(codec)?0:(codec==VideoCodec::Vc1?0:c->findText("medium"))));
}
QComboBox* preset_combo(QWidget* parent = nullptr,VideoCodec codec=VideoCodec::X264,const QString& selected={}) {
    auto* c=new QComboBox(parent);populate_preset_combo(c,codec,selected);return c;
}
void configure_pass_control(QCheckBox* box,VideoCodec codec,bool requested){const QSignalBlocker blocker(box);const bool hardware=video_codec_is_hardware(codec);box->setChecked(hardware?false:requested);box->setEnabled(!hardware);box->setToolTip(hardware?QStringLiteral("Hardware encoders use the selected FFmpeg backend's native rate control; BDMV Author two-pass mode is unavailable."):codec==VideoCodec::Vc1?QStringLiteral("vc1enc two-pass: statistics-only analysis followed by a raw VC-1 encode."):QString());}
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
    if(codec==AudioCodec::Lpcm || codec==AudioCodec::Libhdma){spin->setRange(0,0);spin->setValue(0);spin->setSpecialValueText(codec==AudioCodec::Libhdma?"Measured":"Fixed");spin->setEnabled(false);return;}
    spin->setEnabled(true);
    if(allow_exceeding_format_limits){spin->setRange(1,1000000);spin->setSingleStep(32);}
    else if(codec==AudioCodec::Dca){spin->setRange(384,1536);spin->setSingleStep(32);spin->setToolTip(QString());}
    else{
        int maximum=target_max_ac3_bitrate_kbps(target);
#if defined(BDMVAUTHOR_HAVE_EAC371ENC)
        if(target_supports_ac3_71_extension(target))
            maximum=target_max_ac3_71_bitrate_kbps(target);
#endif
        spin->setRange(target==DiscTarget::Avchd?64:192,maximum);spin->setSingleStep(32);
        if(target_supports_ac3_71_extension(target))
            spin->setToolTip("For 7.1 output this is the total bitrate of the 5.1 AC-3 compatibility core plus the dependent E-AC-3 surround extension. For 5.1 or fewer channels it is the ordinary AC-3 bitrate and may not exceed 640 kb/s.");
        else if(target==DiscTarget::DvdVideo480p)
            spin->setToolTip("DVD-Video uses plain AC-3 only; E-AC-3 and the 7.1 dependent extension are not permitted. Maximum AC-3 bitrate: 448 kb/s.");
        else if(target==DiscTarget::Avchd)
            spin->setToolTip("AVCHD uses plain AC-3 only; E-AC-3 and the 7.1 dependent extension are not permitted. Maximum AC-3 bitrate: 640 kb/s.");
        else
            spin->setToolTip(QString());
    }
    spin->setValue(std::clamp(bitrate,spin->minimum(),spin->maximum()));
}
void configure_video_bitrate_spin(QSpinBox* spin, DiscTarget target, VideoCodec codec, int bitrate,bool allow_exceeding_format_limits=false) {
    const QSignalBlocker blocker(spin);
    spin->setRange(allow_exceeding_format_limits?1:1000,allow_exceeding_format_limits?1000000:video_codec_max_bitrate_kbps(target,codec));
    spin->setValue(std::clamp(bitrate,spin->minimum(),spin->maximum()));
}
QSpinBox* video_rate_limit_spin(QWidget* parent=nullptr){auto* s=new QSpinBox(parent);s->setSuffix(" kb/s");s->setSingleStep(1000);return s;}
void configure_video_minrate_spin(QSpinBox* spin,DiscTarget target,VideoCodec codec,int bitrate,bool allow_exceeding_format_limits=false){const QSignalBlocker blocker(spin);if(codec==VideoCodec::Vc1){spin->setRange(0,0);spin->setValue(0);spin->setSpecialValueText("Fixed 0");spin->setEnabled(false);spin->setToolTip("vc1enc does not expose a separate minimum bitrate control.");return;}spin->setSpecialValueText(QString());spin->setEnabled(true);spin->setRange(0,allow_exceeding_format_limits?1000000:video_codec_max_bitrate_kbps(target,codec));spin->setValue(std::clamp(bitrate,spin->minimum(),spin->maximum()));}
void configure_video_maxrate_spin(QSpinBox* spin,DiscTarget target,VideoCodec codec,int bitrate,bool allow_exceeding_format_limits=false){const QSignalBlocker blocker(spin);spin->setRange(1,allow_exceeding_format_limits?1000000:video_codec_max_bitrate_kbps(target,codec));spin->setValue(std::clamp(bitrate,spin->minimum(),spin->maximum()));}
EncodingProfile profile_from_widgets(QComboBox* video,QSpinBox* bitrate,QComboBox* preset,QCheckBox* two_pass,QComboBox* audio,QSpinBox* audio_bitrate,const EncodingProfile* base=nullptr,QSpinBox* keyframe=nullptr,QSpinBox* minrate=nullptr,QSpinBox* maxrate=nullptr) {
    EncodingProfile e = base ? *base : EncodingProfile{}; e.video_codec=static_cast<VideoCodec>(video->currentData().toInt()); e.video_bitrate_kbps=bitrate->value();if(minrate)e.video_min_bitrate_kbps=minrate->value();if(maxrate)e.video_max_bitrate_kbps=maxrate->value(); if(e.video_codec==VideoCodec::Hevc)e.x265_preset=preset->currentText().toStdString();else if(e.video_codec==VideoCodec::Vc1)e.vc1_preset=preset->currentText().toStdString();else if(e.video_codec==VideoCodec::X264)e.x264_preset=preset->currentText().toStdString(); e.two_pass=video_codec_is_hardware(e.video_codec)?false:two_pass->isChecked();
    if (auto* fast=qobject_cast<QCheckBox*>(two_pass->property("fastFirstPassControl").value<QObject*>()))
        e.fast_first_pass=fast->isChecked();
    if(keyframe)e.keyframe_interval_frames=keyframe->value(); e.audio_codec=static_cast<AudioCodec>(audio->currentData().toInt()); if(audio_bitrate&&audio_codec_has_configurable_bitrate(e.audio_codec))set_audio_bitrate_kbps(e,audio_bitrate->value()); return e;
}
QVariantMap encoding_map(const EncodingProfile& e){QVariantMap m;m["videoCodec"]=static_cast<int>(e.video_codec);m["videoBitrate"]=e.video_bitrate_kbps;m["videoMinrate"]=e.video_min_bitrate_kbps;m["videoMaxrate"]=e.video_max_bitrate_kbps;m["x264Preset"]=QString::fromStdString(e.x264_preset);m["x265Preset"]=QString::fromStdString(e.x265_preset);m["vc1Preset"]=QString::fromStdString(e.vc1_preset);m["twoPass"]=e.two_pass;m["fastFirstPass"]=e.fast_first_pass;m["keyframeIntervalFrames"]=e.keyframe_interval_frames;m["audioCodec"]=static_cast<int>(e.audio_codec);m["ac3Bitrate"]=e.ac3_bitrate_kbps;m["dcaBitrate"]=e.dca_bitrate_kbps;m["lpcmSampleRateHz"]=e.lpcm_sample_rate_hz;m["lpcmBitDepth"]=e.lpcm_bit_depth;m["x264AdvancedOptions"]=QString::fromStdString(e.x264_advanced_options);m["x265AdvancedOptions"]=QString::fromStdString(e.x265_advanced_options);m["h264HardwareAdvancedOptions"]=QString::fromStdString(e.h264_hardware_advanced_options);m["hevcHardwareAdvancedOptions"]=QString::fromStdString(e.hevc_hardware_advanced_options);m["mpeg2AdvancedOptions"]=QString::fromStdString(e.mpeg2_advanced_options);m["vc1AdvancedOptions"]=QString::fromStdString(e.vc1_advanced_options);m["ac3AdvancedOptions"]=QString::fromStdString(e.ac3_advanced_options);m["dcaAdvancedOptions"]=QString::fromStdString(e.dca_advanced_options);m["lpcmAdvancedOptions"]=QString::fromStdString(e.lpcm_advanced_options);m["truehdAdvancedOptions"]=QString::fromStdString(e.truehd_advanced_options);m["hdmaAdvancedOptions"]=QString::fromStdString(e.hdma_advanced_options);return m;}
EncodingProfile encoding_from_map(const QVariant& value,const EncodingProfile& fallback){EncodingProfile e=fallback;const auto m=value.toMap();if(m.isEmpty())return e;if(m.contains("videoCodec"))e.video_codec=static_cast<VideoCodec>(m.value("videoCodec").toInt());if(m.contains("videoBitrate"))e.video_bitrate_kbps=m.value("videoBitrate").toInt();if(m.contains("videoMinrate"))e.video_min_bitrate_kbps=m.value("videoMinrate").toInt();if(m.contains("videoMaxrate"))e.video_max_bitrate_kbps=m.value("videoMaxrate").toInt();if(m.contains("x264Preset"))e.x264_preset=m.value("x264Preset").toString().toStdString();if(m.contains("x265Preset"))e.x265_preset=m.value("x265Preset").toString().toStdString();if(m.contains("vc1Preset"))e.vc1_preset=m.value("vc1Preset").toString().toStdString();if(m.contains("twoPass"))e.two_pass=m.value("twoPass").toBool();if(m.contains("fastFirstPass"))e.fast_first_pass=m.value("fastFirstPass").toBool();if(m.contains("keyframeIntervalFrames"))e.keyframe_interval_frames=m.value("keyframeIntervalFrames").toInt();if(m.contains("audioCodec"))e.audio_codec=static_cast<AudioCodec>(m.value("audioCodec").toInt());if(m.contains("ac3Bitrate"))e.ac3_bitrate_kbps=m.value("ac3Bitrate").toInt();if(m.contains("dcaBitrate"))e.dca_bitrate_kbps=m.value("dcaBitrate").toInt();if(m.contains("lpcmSampleRateHz"))e.lpcm_sample_rate_hz=m.value("lpcmSampleRateHz").toInt();if(m.contains("lpcmBitDepth"))e.lpcm_bit_depth=m.value("lpcmBitDepth").toInt();if(m.contains("x264AdvancedOptions"))e.x264_advanced_options=m.value("x264AdvancedOptions").toString().toStdString();if(m.contains("x265AdvancedOptions"))e.x265_advanced_options=m.value("x265AdvancedOptions").toString().toStdString();if(m.contains("h264HardwareAdvancedOptions"))e.h264_hardware_advanced_options=m.value("h264HardwareAdvancedOptions").toString().toStdString();if(m.contains("hevcHardwareAdvancedOptions"))e.hevc_hardware_advanced_options=m.value("hevcHardwareAdvancedOptions").toString().toStdString();if(m.contains("mpeg2AdvancedOptions"))e.mpeg2_advanced_options=m.value("mpeg2AdvancedOptions").toString().toStdString();if(m.contains("vc1AdvancedOptions"))e.vc1_advanced_options=m.value("vc1AdvancedOptions").toString().toStdString();if(m.contains("ac3AdvancedOptions"))e.ac3_advanced_options=m.value("ac3AdvancedOptions").toString().toStdString();if(m.contains("dcaAdvancedOptions"))e.dca_advanced_options=m.value("dcaAdvancedOptions").toString().toStdString();if(m.contains("lpcmAdvancedOptions"))e.lpcm_advanced_options=m.value("lpcmAdvancedOptions").toString().toStdString();if(m.contains("truehdAdvancedOptions"))e.truehd_advanced_options=m.value("truehdAdvancedOptions").toString().toStdString();if(m.contains("hdmaAdvancedOptions"))e.hdma_advanced_options=m.value("hdmaAdvancedOptions").toString().toStdString();return e;}

QString video_codec_display_name(VideoCodec codec){switch(codec){case VideoCodec::X264:return "x264 / H.264";case VideoCodec::H264Hardware:return "h.264 / AVC (hardware)";case VideoCodec::Hevc:return "x265 / HEVC";case VideoCodec::HevcHardware:return "h.265 / HEVC (hardware)";case VideoCodec::Mpeg2:return "FFmpeg MPEG-2";case VideoCodec::Vc1:return "libvc1 VC-1 (Experimental)";}return "Video";}
QString audio_codec_display_name(AudioCodec codec){switch(codec){case AudioCodec::Ac3:return "FFmpeg AC-3";case AudioCodec::Dca:return "FFmpeg DCA";case AudioCodec::Lpcm:return "FFmpeg LPCM";case AudioCodec::TrueHdAc3:return "FFmpeg THD";case AudioCodec::Libhdma:return "libhdma (lossless, experimental)";}return "Audio";}
QString tool_report(const ToolCapabilities& c){
    auto line=[](const QString& name,const QString& path,bool ok){return QStringLiteral("%1: %2").arg(name,ok?(path+"  [OK]"):QStringLiteral("not found / unusable"));};
    QStringList out;out<<line("FFmpeg",c.ffmpeg,c.ffmpeg_ok)<<line("ffprobe",c.ffprobe,c.ffprobe_ok)<<line("x264",c.x264,c.x264_ok)<<line("x265",c.x265,c.x265_ok)
        <<line("vc1enc (experimental)",c.vc1enc,c.vc1enc_ok)
        <<line("hdmaenc (lossless, experimental)",c.hdmaenc,c.hdmaenc_ok)
        <<line("tsMuxer",c.tsmuxer,c.tsmuxer_ok)<<line("dvdauthor",c.dvdauthor,c.dvdauthor_ok)<<line("spumux",c.spumux,c.spumux_ok)<<line("mplex",c.mplex,c.mplex_ok)<<line("mkisofs",c.mkisofs,c.mkisofs_ok);
    if(c.ffmpeg_ok){
        out<<QStringLiteral("FFmpeg encoders: libx264=%1, libx265=%2, mpeg2video=%3, ac3=%4, dca=%5, truehd=%6, pcm_s16le=%7, pcm_s16be=%8, pcm_s24le=%9, pcm_s24be=%10").arg(c.ffmpeg_libx264?"yes":"no",c.ffmpeg_libx265?"yes":"no",c.ffmpeg_mpeg2?"yes":"no",c.ffmpeg_ac3?"yes":"no",c.ffmpeg_dca?"yes":"no",c.ffmpeg_truehd?"yes":"no",c.ffmpeg_pcm_s16le?"yes":"no",c.ffmpeg_pcm_s16be?"yes":"no",c.ffmpeg_pcm_s24le?"yes":"no",c.ffmpeg_pcm_s24be?"yes":"no");
        QStringList h264_hw_names;for(const auto& value:c.ffmpeg_h264_hardware_encoders.values())h264_hw_names<<value;
        QStringList hevc_hw_names;for(const auto& value:c.ffmpeg_hevc_hardware_encoders.values())hevc_hw_names<<value;
        out<<QStringLiteral("FFmpeg H.264 hardware encoders: %1").arg(h264_hw_names.isEmpty()?QStringLiteral("none"):h264_hw_names.join(", "));
        out<<QStringLiteral("FFmpeg H.265 hardware encoders: %1").arg(hevc_hw_names.isEmpty()?QStringLiteral("none"):hevc_hw_names.join(", "));
        out<<QStringLiteral("FFmpeg hardware decoding APIs: %1").arg(c.ffmpeg_hwaccels.isEmpty()?QStringLiteral("none"):c.ffmpeg_hwaccels.join(", "));
        const auto h264hw=resolved_hardware_encoder(true),hevc_hw=resolved_hardware_encoder(false),decode_hw=resolved_hardware_decode_backend();
        out<<QStringLiteral("Selected hardware encoding API: %1 (H.264=%2, H.265=%3)").arg(hardware_backend_label(c.config.hardware_encode_backend),h264hw.isEmpty()?QStringLiteral("unavailable"):h264hw,hevc_hw.isEmpty()?QStringLiteral("unavailable"):hevc_hw);
        out<<QStringLiteral("Selected hardware decoding API: %1%2").arg(hardware_backend_label(c.config.hardware_decode_backend),decode_hw.isEmpty()&&c.config.hardware_decode_backend!="software"?QStringLiteral(" [software fallback]"):QString());
    }
    const bool remote=c.config.remote_encoding&&c.remote_ffmpeg_ok;const bool h264ok=remote?(c.remote_ffmpeg_libx264||c.remote_x264_ok):(c.ffmpeg_libx264||c.x264_ok);const bool hevcok=remote?(c.remote_ffmpeg_libx265||c.remote_x265_ok):(c.ffmpeg_libx265||c.x265_ok);
    const auto hp=h264ok?(resolved_h264_provider()==VideoEncoderProvider::Ffmpeg?"FFmpeg/libx264":"standalone x264"):"unavailable";const auto xp=hevcok?(resolved_hevc_provider()==VideoEncoderProvider::Ffmpeg?"FFmpeg/libx265":"standalone x265"):"unavailable";
    out<<QStringLiteral("Encoding location: %1").arg(remote?QStringLiteral("remote %1:%2 over SSH").arg(c.config.remote_host).arg(c.config.remote_ssh_port):QStringLiteral("local"));
    if(remote){QStringList rh264;for(const auto& v:c.remote_ffmpeg_h264_hardware_encoders.values())rh264<<v;QStringList rhevc;for(const auto& v:c.remote_ffmpeg_hevc_hardware_encoders.values())rhevc<<v;out<<QStringLiteral("Remote FFmpeg H.264 hardware encoders: %1").arg(rh264.isEmpty()?QStringLiteral("none"):rh264.join(", "))<<QStringLiteral("Remote FFmpeg H.265 hardware encoders: %1").arg(rhevc.isEmpty()?QStringLiteral("none"):rhevc.join(", "))<<QStringLiteral("Remote FFmpeg hardware decoding APIs: %1").arg(c.remote_ffmpeg_hwaccels.isEmpty()?QStringLiteral("none"):c.remote_ffmpeg_hwaccels.join(", "))<<QStringLiteral("Remote vc1enc: %1; remote hdmaenc: %2").arg(c.remote_vc1enc_ok?"yes":"no",c.remote_hdmaenc_ok?"yes":"no");}
    out<<QStringLiteral("Active H.264 provider: %1%2").arg(hp,remote?QStringLiteral(" (remote)"):QString())<<QStringLiteral("Active H.265 provider: %1%2").arg(xp,remote?QStringLiteral(" (remote)"):QString());
    out<<QStringLiteral("Experimental libvc1 VC-1 provider: %1").arg(remote?(c.remote_vc1enc_ok?"remote vc1enc":"unavailable"):(c.vc1enc_ok?"local vc1enc":"unavailable"));
    out<<QStringLiteral("Targets: Blu-ray=%1, UHD Blu-ray=%2, AVCHD=%3, DVD-Video=%4").arg(target_available(DiscTarget::BluRay1080)?"enabled":"disabled",target_available(DiscTarget::UltraHdBluRay2160)?"enabled":"disabled",target_available(DiscTarget::Avchd)?"enabled":"disabled",target_available(DiscTarget::DvdVideo480p)?"enabled":"disabled");
    if(!c.warnings.isEmpty()){out<<""<<"Warnings:";for(const auto& w:c.warnings)out<<QStringLiteral("• %1").arg(w);}return out.join('\n');
}
class RemoteServerEditDialog final : public QDialog {
    RemoteServerConfiguration value_;
    QCheckBox* enabled_=nullptr;QLineEdit *name_=nullptr,*ssh_=nullptr,*host_=nullptr,*user_=nullptr,*identity_=nullptr,*ffmpeg_=nullptr,*x264_=nullptr,*x265_=nullptr,*vc1enc_=nullptr,*hdmaenc_=nullptr,*hardware_device_=nullptr;
    QSpinBox *port_=nullptr,*parallel_=nullptr;QComboBox *os_=nullptr,*h264provider_=nullptr,*hevcprovider_=nullptr,*hardware_encode_backend_=nullptr,*hardware_decode_backend_=nullptr;
    QLabel* hardware_status_=nullptr;QMap<QString,QString> detected_h264_,detected_hevc_;QStringList detected_hwaccels_;QString raw_h264_,raw_hevc_,raw_decode_;bool hardware_probed_=false;
    static QString selected_or(const QComboBox* box,const QString& fallback){const auto v=box?box->currentData().toString():QString();return v.isEmpty()?fallback:v;}
    RemoteServerConfiguration form_value()const{
        RemoteServerConfiguration v=value_;v.enabled=enabled_->isChecked();v.name=name_->text().trimmed();v.ssh=ssh_->text().trimmed();v.host=host_->text().trimmed();v.user=user_->text().trimmed();v.port=port_->value();v.identity_file=identity_->text().trimmed();v.operating_system=os_->currentData().toString();
        const bool windows=v.operating_system=="windows";v.ffmpeg=remote_program_name(ffmpeg_->text(),windows?QStringLiteral("ffmpeg.exe"):QStringLiteral("ffmpeg"));v.x264=remote_program_name(x264_->text(),windows?QStringLiteral("x264.exe"):QStringLiteral("x264"));v.x265=remote_program_name(x265_->text(),windows?QStringLiteral("x265.exe"):QStringLiteral("x265"));v.vc1enc=remote_program_name(vc1enc_->text(),windows?QStringLiteral("vc1enc.exe"):QStringLiteral("vc1enc"));v.hdmaenc=remote_program_name(hdmaenc_->text(),windows?QStringLiteral("hdmaenc.exe"):QStringLiteral("hdmaenc"));v.h264_provider=h264provider_->currentData().toString();v.hevc_provider=hevcprovider_->currentData().toString();v.hardware_device=hardware_device_->text().trimmed();v.parallel_encodes=parallel_->value();
        return v;
    }
    void set_combo_data(QComboBox* box,const QString& value){const int i=box->findData(value);if(i>=0)box->setCurrentIndex(i);}
    void populate_saved_hardware_choices(const RemoteServerConfiguration& initial){
        hardware_encode_backend_->clear();hardware_encode_backend_->addItem("Automatic — choose an available FFmpeg hardware encoder","auto");
        QString enc=initial.hardware_encode_backend.trimmed().toLower();if(enc.isEmpty())enc="auto";
        if(enc=="custom")hardware_encode_backend_->addItem("Custom encoder names (Advanced overrides…)","custom");
        else if(enc!="auto")hardware_encode_backend_->addItem(QStringLiteral("%1 — saved; refresh to verify").arg(hardware_backend_label(enc)),enc);
        set_combo_data(hardware_encode_backend_,enc);
        hardware_decode_backend_->clear();hardware_decode_backend_->addItem("Software decoding","software");hardware_decode_backend_->addItem("Automatic hardware decoding (FFmpeg -hwaccel auto)","auto");
        QString dec=initial.hardware_decode_backend.trimmed().toLower();if(dec.isEmpty())dec=initial.hardware_decoder.trimmed().toLower();if(dec.isEmpty())dec="software";
        if(dec=="custom")hardware_decode_backend_->addItem("Custom FFmpeg -hwaccel value (Advanced overrides…)","custom");
        else if(dec!="software"&&dec!="auto")hardware_decode_backend_->addItem(QStringLiteral("%1 — saved; refresh to verify").arg(hardware_backend_label(dec)),dec);
        set_combo_data(hardware_decode_backend_,dec);
    }
    void populate_detected_hardware_choices(const QString& keep_encode,const QString& keep_decode){
        {
            const QSignalBlocker blocker(hardware_encode_backend_);hardware_encode_backend_->clear();hardware_encode_backend_->addItem("Automatic — choose an available FFmpeg hardware encoder","auto");
            QSet<QString> backends;for(auto it=detected_h264_.cbegin();it!=detected_h264_.cend();++it)backends.insert(it.key());for(auto it=detected_hevc_.cbegin();it!=detected_hevc_.cend();++it)backends.insert(it.key());QStringList sorted;for(const auto& b:backends)sorted<<b;sorted.sort(Qt::CaseInsensitive);
            for(const auto& backend:sorted){QStringList codecs;if(detected_h264_.contains(backend))codecs<<"H.264";if(detected_hevc_.contains(backend))codecs<<"H.265";hardware_encode_backend_->addItem(QStringLiteral("%1 — %2").arg(hardware_backend_label(backend),codecs.join(" + ")),backend);}
            if(keep_encode=="custom")hardware_encode_backend_->addItem("Custom encoder names (Advanced overrides…)","custom");else if(keep_encode!="auto"&&hardware_encode_backend_->findData(keep_encode)<0)hardware_encode_backend_->addItem(QStringLiteral("%1 — unavailable on this server").arg(hardware_backend_label(keep_encode)),keep_encode);
            set_combo_data(hardware_encode_backend_,keep_encode);if(hardware_encode_backend_->currentIndex()<0)hardware_encode_backend_->setCurrentIndex(0);
        }
        {
            const QSignalBlocker blocker(hardware_decode_backend_);hardware_decode_backend_->clear();hardware_decode_backend_->addItem("Software decoding","software");hardware_decode_backend_->addItem("Automatic hardware decoding (FFmpeg -hwaccel auto)","auto");auto list=detected_hwaccels_;list.sort(Qt::CaseInsensitive);for(const auto& backend:list)hardware_decode_backend_->addItem(hardware_backend_label(backend),backend);
            if(keep_decode=="custom")hardware_decode_backend_->addItem("Custom FFmpeg -hwaccel value (Advanced overrides…)","custom");else if(keep_decode!="software"&&keep_decode!="auto"&&hardware_decode_backend_->findData(keep_decode)<0)hardware_decode_backend_->addItem(QStringLiteral("%1 — unavailable on this server").arg(hardware_backend_label(keep_decode)),keep_decode);
            set_combo_data(hardware_decode_backend_,keep_decode);if(hardware_decode_backend_->currentIndex()<0)hardware_decode_backend_->setCurrentIndex(0);
        }
    }
    void detect_hardware(){
        auto probe=form_value();const QString keep_encode=selected_or(hardware_encode_backend_,probe.hardware_encode_backend.isEmpty()?QStringLiteral("auto"):probe.hardware_encode_backend);const QString keep_decode=selected_or(hardware_decode_backend_,probe.hardware_decode_backend.isEmpty()?QStringLiteral("software"):probe.hardware_decode_backend);
        hardware_status_->setText("Connecting over SSH and reading FFmpeg capabilities…");QApplication::setOverrideCursor(Qt::WaitCursor);QApplication::processEvents();const auto caps=probe_remote_hardware_capabilities(probe);QApplication::restoreOverrideCursor();
        if(!caps.ffmpeg_ok){hardware_status_->setText(QStringLiteral("Detection failed: %1").arg(caps.error));QMessageBox::warning(this,"Remote hardware detection",caps.error);return;}
        detected_h264_=caps.h264_encoders;detected_hevc_=caps.hevc_encoders;detected_hwaccels_=caps.hwaccels;hardware_probed_=true;populate_detected_hardware_choices(keep_encode,keep_decode);
        QStringList summary;summary<<QStringLiteral("Connected: %1").arg(caps.ffmpeg);summary<<QStringLiteral("H.264 hardware encoders: %1").arg(detected_h264_.isEmpty()?QStringLiteral("none"):QString::number(detected_h264_.size()));summary<<QStringLiteral("H.265 hardware encoders: %1").arg(detected_hevc_.isEmpty()?QStringLiteral("none"):QString::number(detected_hevc_.size()));summary<<QStringLiteral("decode APIs: %1").arg(detected_hwaccels_.isEmpty()?QStringLiteral("none"):QString::number(detected_hwaccels_.size()));hardware_status_->setText(summary.join("; "));
    }
    void edit_advanced_overrides(){
        QDialog d(this);d.setWindowTitle("Remote hardware — Advanced overrides");auto* root=new QVBoxLayout(&d);auto* note=new QLabel("Normally leave these alone. They override FFmpeg encoder names or -hwaccel with exact values for unusual/custom FFmpeg builds. Selecting a normal API in the main dialog replaces the corresponding override.",&d);note->setWordWrap(true);root->addWidget(note);auto* form=new QFormLayout;auto* h264=new QLineEdit(raw_h264_,&d);auto* hevc=new QLineEdit(raw_hevc_,&d);auto* decode=new QLineEdit(raw_decode_,&d);h264->setPlaceholderText("e.g. h264_nvenc");hevc->setPlaceholderText("e.g. hevc_nvenc");decode->setPlaceholderText("e.g. cuda");form->addRow("Exact H.264 encoder",h264);form->addRow("Exact H.265 encoder",hevc);form->addRow("Exact -hwaccel value",decode);root->addLayout(form);auto* buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&d);root->addWidget(buttons);connect(buttons,&QDialogButtonBox::accepted,&d,&QDialog::accept);connect(buttons,&QDialogButtonBox::rejected,&d,&QDialog::reject);if(d.exec()!=QDialog::Accepted)return;raw_h264_=h264->text().trimmed();raw_hevc_=hevc->text().trimmed();raw_decode_=decode->text().trimmed().toLower();if(!raw_h264_.isEmpty()||!raw_hevc_.isEmpty()){if(hardware_encode_backend_->findData("custom")<0)hardware_encode_backend_->addItem("Custom encoder names (Advanced overrides…)","custom");set_combo_data(hardware_encode_backend_,"custom");}if(!raw_decode_.isEmpty()){if(hardware_decode_backend_->findData("custom")<0)hardware_decode_backend_->addItem("Custom FFmpeg -hwaccel value (Advanced overrides…)","custom");set_combo_data(hardware_decode_backend_,"custom");}}
    void apply_hardware_selection(RemoteServerConfiguration& v)const{
        const QString enc=selected_or(hardware_encode_backend_,"auto");v.hardware_encode_backend=enc;
        if(enc=="custom"){v.h264_hardware_encoder=raw_h264_;v.hevc_hardware_encoder=raw_hevc_;}
        else if(!hardware_probed_){v.h264_hardware_encoder=raw_h264_;v.hevc_hardware_encoder=raw_hevc_;}
        else if(enc=="auto"){
            v.h264_hardware_encoder=preferred_remote_hardware_encoder(detected_h264_);v.hevc_hardware_encoder=preferred_remote_hardware_encoder(detected_hevc_);
        }else{
            v.h264_hardware_encoder=detected_h264_.value(enc);v.hevc_hardware_encoder=detected_hevc_.value(enc);
        }
        const QString dec=selected_or(hardware_decode_backend_,"software");v.hardware_decode_backend=dec;if(dec=="custom")v.hardware_decoder=raw_decode_.isEmpty()?QStringLiteral("software"):raw_decode_;else v.hardware_decoder=dec;
    }
public:
    RemoteServerEditDialog(QWidget* parent,const RemoteServerConfiguration& initial):QDialog(parent),value_(initial),raw_h264_(initial.h264_hardware_encoder),raw_hevc_(initial.hevc_hardware_encoder),raw_decode_(initial.hardware_decoder){
        setWindowTitle("Remote encoding server");resize(780,760);auto*root=new QVBoxLayout(this);
        auto*note=new QLabel("Each server is independent. Use Detect / refresh APIs to read the remote FFmpeg hardware encoders and decoders over SSH, then choose friendly API names just like Local. Remote FFmpeg may be ffmpeg, ffmpeg.exe, or an absolute path. Connection, hardware, encoder, and concurrency choices are application settings only. Two-pass software jobs may create only statistics/log sidecars in the remote temporary directory; encoded media is always streamed.",this);note->setWordWrap(true);root->addWidget(note);
        enabled_=new QCheckBox("Enable this server",this);enabled_->setChecked(initial.enabled);root->addWidget(enabled_);auto*form=new QFormLayout;root->addLayout(form);
        name_=new QLineEdit(initial.name,this);name_->setPlaceholderText("Display name (optional)");form->addRow("Name",name_);
        os_=new QComboBox(this);os_->addItem("Auto-detect / infer from tool path","auto");os_->addItem("POSIX (Linux / macOS / BSD)","posix");os_->addItem("Windows OpenSSH","windows");{int i=os_->findData(initial.operating_system);os_->setCurrentIndex(i>=0?i:0);}form->addRow("Remote operating system",os_);
        ssh_=new QLineEdit(initial.ssh,this);ssh_->setPlaceholderText("ssh from local PATH");form->addRow("SSH client",ssh_);host_=new QLineEdit(initial.host,this);form->addRow("Server",host_);user_=new QLineEdit(initial.user,this);form->addRow("User",user_);port_=new QSpinBox(this);port_->setRange(1,65535);port_->setValue(initial.port);form->addRow("SSH port",port_);identity_=new QLineEdit(initial.identity_file,this);form->addRow("Identity file",identity_);
        ffmpeg_=new QLineEdit(initial.ffmpeg,this);ffmpeg_->setPlaceholderText("ffmpeg / ffmpeg.exe / full path");x264_=new QLineEdit(initial.x264,this);x265_=new QLineEdit(initial.x265,this);vc1enc_=new QLineEdit(initial.vc1enc,this);hdmaenc_=new QLineEdit(initial.hdmaenc,this);form->addRow("Remote FFmpeg",ffmpeg_);form->addRow("Remote x264",x264_);form->addRow("Remote x265",x265_);form->addRow("Remote vc1enc",vc1enc_);form->addRow("Remote hdmaenc",hdmaenc_);
        h264provider_=new QComboBox(this);h264provider_->addItem("FFmpeg/libx264","ffmpeg");h264provider_->addItem("Standalone x264","standalone");{int i=h264provider_->findData(initial.h264_provider);h264provider_->setCurrentIndex(i>=0?i:0);}form->addRow("Software H.264 provider",h264provider_);hevcprovider_=new QComboBox(this);hevcprovider_->addItem("FFmpeg/libx265","ffmpeg");hevcprovider_->addItem("Standalone x265","standalone");{int i=hevcprovider_->findData(initial.hevc_provider);hevcprovider_->setCurrentIndex(i>=0?i:0);}form->addRow("Software H.265 provider",hevcprovider_);
        hardware_encode_backend_=new QComboBox(this);hardware_encode_backend_->setToolTip("Hardware encoder API used by this server for the hardware H.264/H.265 codec choices. Click Detect / refresh APIs to populate it from the remote FFmpeg build.");hardware_decode_backend_=new QComboBox(this);hardware_decode_backend_->setToolTip("Remote FFmpeg -hwaccel backend. Unsupported streams automatically fall back to remote software decoding and then local software decode/raw relay.");populate_saved_hardware_choices(initial);form->addRow("Hardware encoding API",hardware_encode_backend_);form->addRow("Hardware decoding API",hardware_decode_backend_);hardware_device_=new QLineEdit(initial.hardware_device,this);hardware_device_->setPlaceholderText("e.g. /dev/dri/renderD128 (optional)");hardware_device_->setToolTip("Optional remote FFmpeg hardware device/render node. QSV uses it as child_device=... and VAAPI uses it as the device specifier. Leave empty for FFmpeg automatic selection.");form->addRow("Hardware device / render node",hardware_device_);
        auto* hwbuttons=new QWidget(this);auto* hwl=new QHBoxLayout(hwbuttons);hwl->setContentsMargins(0,0,0,0);auto* detect=new QPushButton("Detect / refresh APIs",hwbuttons);auto* advanced=new QPushButton("Advanced overrides…",hwbuttons);hwl->addWidget(detect);hwl->addWidget(advanced);hwl->addStretch(1);form->addRow("Hardware setup",hwbuttons);hardware_status_=new QLabel("Not probed in this dialog yet. Existing saved selections are preserved until you refresh.",this);hardware_status_->setWordWrap(true);form->addRow("Server capabilities",hardware_status_);connect(detect,&QPushButton::clicked,this,[this]{detect_hardware();});connect(advanced,&QPushButton::clicked,this,[this]{edit_advanced_overrides();});
        parallel_=new QSpinBox(this);parallel_->setRange(1,64);parallel_->setValue(initial.parallel_encodes);form->addRow("Parallel encodes on this server",parallel_);
        auto*buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,this);root->addWidget(buttons);connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject);connect(buttons,&QDialogButtonBox::accepted,this,[this]{if(enabled_->isChecked()&&host_->text().trimmed().isEmpty()){QMessageBox::warning(this,"Remote encoding server","Enter an SSH server name or address.");return;}auto candidate=form_value();apply_hardware_selection(candidate);value_=std::move(candidate);accept();});
    }
    RemoteServerConfiguration value()const{return value_;}
};

class RemoteServersDialog final : public QDialog {
    ToolConfiguration value_;QListWidget*list_=nullptr;QCheckBox *local_enabled_=nullptr,*local_fallback_=nullptr,*retry_forever_=nullptr;QComboBox* affinity_persistence_=nullptr;QSpinBox *local_parallel_=nullptr,*retry_delay_=nullptr,*max_retries_=nullptr,*failover_=nullptr;
    void refresh(){list_->clear();for(const auto&r:value_.remote_servers){const QString enc=r.hardware_encode_backend.isEmpty()?QStringLiteral("auto"):r.hardware_encode_backend;const QString dec=r.hardware_decode_backend.isEmpty()?QStringLiteral("software"):r.hardware_decode_backend;const QString title=QStringLiteral("%1%2 — %3@%4:%5 — %6 job(s) — HW encode: %7; decode: %8").arg(r.enabled?QString():QStringLiteral("[disabled] "),r.name.isEmpty()?r.host:r.name,r.user.isEmpty()?QStringLiteral("default"):r.user,r.host).arg(r.port).arg(r.parallel_encodes).arg(hardware_backend_label(enc),hardware_backend_label(dec));list_->addItem(title);}}
public:
    RemoteServersDialog(QWidget*parent,const ToolConfiguration&initial):QDialog(parent),value_(initial){setWindowTitle("Encoding workers and retry policy");resize(850,650);auto*root=new QVBoxLayout(this);auto*note=new QLabel("BDMV Author can use Local and multiple SSH servers concurrently. Each remote server keeps its own FFmpeg/encoder/backend settings. Transient network/device failures retry automatically; deterministic errors such as a missing codec, invalid arguments, unsupported backend/profile, or SSH authentication failure stop immediately. After repeated transient failures a job may fail over to another compatible worker.",this);note->setWordWrap(true);root->addWidget(note);
        auto*local=new QGroupBox("Local encoding",this);auto*lf=new QFormLayout(local);local_enabled_=new QCheckBox("Use Local in parallel with remote servers",local);local_enabled_->setChecked(initial.local_encoding_enabled);local_fallback_=new QCheckBox("Allow Local only when the requested encoder is unavailable on every remote worker",local);local_fallback_->setChecked(initial.local_fallback_when_remote_unavailable);local_parallel_=new QSpinBox(local);local_parallel_->setRange(1,64);local_parallel_->setValue(initial.local_parallel_encodes);lf->addRow(local_enabled_);lf->addRow(local_fallback_);lf->addRow("Parallel local encodes",local_parallel_);affinity_persistence_=new QComboBox(local);affinity_persistence_->addItem("Machine-local companion settings (recommended)","local");affinity_persistence_->addItem("Save worker assignments in project file","project");affinity_persistence_->addItem("Session only — do not save worker assignments","session");{int i=affinity_persistence_->findData(initial.worker_affinity_persistence);affinity_persistence_->setCurrentIndex(i>=0?i:0);}affinity_persistence_->setToolTip("Controls only title/menu/audio worker affinity. SSH credentials and server definitions always remain application settings.");lf->addRow("Worker assignment persistence",affinity_persistence_);root->addWidget(local);
        auto*remote=new QGroupBox("Remote servers",this);auto*rv=new QVBoxLayout(remote);list_=new QListWidget(remote);rv->addWidget(list_,1);auto*rb=new QHBoxLayout;auto*add=new QPushButton("Add…",remote);auto*edit=new QPushButton("Edit…",remote);auto*remove=new QPushButton("Remove",remote);rb->addWidget(add);rb->addWidget(edit);rb->addWidget(remove);rb->addStretch(1);rv->addLayout(rb);root->addWidget(remote,1);refresh();connect(add,&QPushButton::clicked,this,[this]{RemoteServerConfiguration r;r.name=QStringLiteral("Server %1").arg(value_.remote_servers.size()+1);RemoteServerEditDialog d(this,r);if(d.exec()==QDialog::Accepted){value_.remote_servers.push_back(d.value());refresh();}});connect(edit,&QPushButton::clicked,this,[this]{const int row=list_->currentRow();if(row<0||row>=value_.remote_servers.size())return;RemoteServerEditDialog d(this,value_.remote_servers[row]);if(d.exec()==QDialog::Accepted){value_.remote_servers[row]=d.value();refresh();list_->setCurrentRow(row);}});connect(remove,&QPushButton::clicked,this,[this]{const int row=list_->currentRow();if(row<0||row>=value_.remote_servers.size())return;value_.remote_servers.removeAt(row);refresh();});
        auto*retry=new QGroupBox("Transient-error retry / failover",this);auto*rf=new QFormLayout(retry);retry_delay_=new QSpinBox(retry);retry_delay_->setRange(0,86400);retry_delay_->setSuffix(" s");retry_delay_->setValue(initial.retry_delay_seconds);max_retries_=new QSpinBox(retry);max_retries_->setRange(0,1000000);max_retries_->setValue(initial.maximum_retries);retry_forever_=new QCheckBox("Retry transient failures forever",retry);retry_forever_->setChecked(initial.retry_forever);failover_=new QSpinBox(retry);failover_->setRange(1,1000000);failover_->setValue(initial.failover_after_failures);rf->addRow("Delay before retry",retry_delay_);rf->addRow("Maximum retries",max_retries_);rf->addRow(retry_forever_);rf->addRow("Fail over after consecutive failures",failover_);root->addWidget(retry);
        auto*buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,this);root->addWidget(buttons);connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject);connect(buttons,&QDialogButtonBox::accepted,this,[this]{value_.local_encoding_enabled=local_enabled_->isChecked();value_.local_fallback_when_remote_unavailable=local_fallback_->isChecked();value_.worker_affinity_persistence=affinity_persistence_->currentData().toString();value_.local_parallel_encodes=local_parallel_->value();value_.retry_delay_seconds=retry_delay_->value();value_.maximum_retries=max_retries_->value();value_.retry_forever=retry_forever_->isChecked();value_.failover_after_failures=failover_->value();if(!value_.local_encoding_enabled&&!value_.local_fallback_when_remote_unavailable&&std::none_of(value_.remote_servers.cbegin(),value_.remote_servers.cend(),[](const auto&r){return r.enabled&&!r.host.isEmpty();})){QMessageBox::warning(this,"Encoding workers","Enable local encoding, Local fallback, or at least one remote server.");return;}QSet<QString> names;for(const auto&r:value_.remote_servers){if(!r.enabled||r.host.trimmed().isEmpty())continue;const QString name=r.name.trimmed().isEmpty()?r.host.trimmed():r.name.trimmed();if(name.compare("Local",Qt::CaseInsensitive)==0||names.contains(name)){QMessageBox::warning(this,"Encoding workers","Enabled worker names must be unique, and 'Local' is reserved for the local machine.");return;}names.insert(name);}accept();});}
    ToolConfiguration value()const{return value_;}
};

class ToolSettingsDialog final : public QDialog {
    ToolConfiguration value_;ToolConfiguration remote_settings_;QPlainTextEdit* report_=nullptr;QComboBox *h264_provider_=nullptr,*hevc_provider_=nullptr,*hardware_encode_backend_=nullptr,*hardware_decode_backend_=nullptr,*scaling_filter_backend_=nullptr,*tonemap_filter_backend_=nullptr;QLineEdit* filter_device_=nullptr;QPushButton* remote_button_=nullptr;
    QLineEdit *ffmpeg_=nullptr,*ffprobe_=nullptr,*x264_=nullptr,*x265_=nullptr,*vc1enc_=nullptr,*hdmaenc_=nullptr,*tsmuxer_=nullptr,*dvdauthor_=nullptr,*spumux_=nullptr,*mplex_=nullptr,*mkisofs_=nullptr,*bdsup2sub_=nullptr;
    static QLineEdit* tool_row(QWidget* parent,QFormLayout* form,const QString& label,const QString& initial){
        auto* row=new QWidget(parent);auto* l=new QHBoxLayout(row);l->setContentsMargins(0,0,0,0);auto* edit=new QLineEdit(initial,row);edit->setPlaceholderText("Auto-detect from PATH");auto* browse=new QPushButton("…",row);browse->setFixedWidth(34);auto* clear=new QPushButton("Auto",row);clear->setFixedWidth(52);l->addWidget(edit,1);l->addWidget(browse);l->addWidget(clear);form->addRow(label,row);
        QObject::connect(browse,&QPushButton::clicked,row,[edit,row]{const auto p=QFileDialog::getOpenFileName(row,"Select executable",edit->text());if(!p.isEmpty())edit->setText(p);});QObject::connect(clear,&QPushButton::clicked,row,[edit]{edit->clear();});return edit;
    }
    ToolConfiguration current()const{ToolConfiguration c=remote_settings_;c.ffmpeg=ffmpeg_->text();c.ffprobe=ffprobe_->text();c.x264=x264_->text();c.x265=x265_->text();c.vc1enc=vc1enc_->text();c.hdmaenc=hdmaenc_->text();
        c.tsmuxer=tsmuxer_->text();c.dvdauthor=dvdauthor_->text();c.spumux=spumux_->text();c.bdsup2sub=bdsup2sub_->text();c.mplex=mplex_->text();c.mkisofs=mkisofs_->text();c.h264_provider=h264_provider_->currentData().toString();c.hevc_provider=hevc_provider_->currentData().toString();c.hardware_encode_backend=hardware_encode_backend_->currentData().toString();c.hardware_decode_backend=hardware_decode_backend_->currentData().toString();
        c.scaling_filter_backend=scaling_filter_backend_->currentData().toString();
        c.tonemap_filter_backend=tonemap_filter_backend_->currentData().toString();
        c.filter_hardware_device=filter_device_->text().trimmed();return c;}
    void populate_hardware_backends(const ToolCapabilities& caps,const ToolConfiguration& keep){
        {
            const QSignalBlocker blocker(hardware_encode_backend_);hardware_encode_backend_->clear();hardware_encode_backend_->addItem("Automatic — choose an available FFmpeg hardware encoder","auto");
            const auto& h264map=caps.ffmpeg_h264_hardware_encoders;const auto& hevcmap=caps.ffmpeg_hevc_hardware_encoders;
            QSet<QString> backends;for(auto it=h264map.cbegin();it!=h264map.cend();++it)backends.insert(it.key());for(auto it=hevcmap.cbegin();it!=hevcmap.cend();++it)backends.insert(it.key());
            QStringList sorted;for(const auto& backend:backends)sorted<<backend;sorted.sort(Qt::CaseInsensitive);for(const auto& backend:sorted){QStringList codecs;if(h264map.contains(backend))codecs<<"H.264";if(hevcmap.contains(backend))codecs<<"H.265";hardware_encode_backend_->addItem(QStringLiteral("%1 — %2").arg(hardware_backend_label(backend),codecs.join(" + ")),backend);}
            if(keep.hardware_encode_backend!="auto"&&hardware_encode_backend_->findData(keep.hardware_encode_backend)<0)hardware_encode_backend_->addItem(QStringLiteral("%1 — unavailable in this FFmpeg build").arg(hardware_backend_label(keep.hardware_encode_backend)),keep.hardware_encode_backend);
            int i=hardware_encode_backend_->findData(keep.hardware_encode_backend);hardware_encode_backend_->setCurrentIndex(i>=0?i:0);
        }
        {
            const QSignalBlocker blocker(hardware_decode_backend_);hardware_decode_backend_->clear();hardware_decode_backend_->addItem("Software decoding","software");hardware_decode_backend_->addItem("Automatic hardware decoding (FFmpeg -hwaccel auto)","auto");
            auto backends=caps.ffmpeg_hwaccels;backends.sort(Qt::CaseInsensitive);for(const auto& backend:backends)hardware_decode_backend_->addItem(hardware_backend_label(backend),backend);
            if(keep.hardware_decode_backend!="software"&&keep.hardware_decode_backend!="auto"&&hardware_decode_backend_->findData(keep.hardware_decode_backend)<0)hardware_decode_backend_->addItem(QStringLiteral("%1 — unavailable in this FFmpeg build").arg(hardware_backend_label(keep.hardware_decode_backend)),keep.hardware_decode_backend);
            int i=hardware_decode_backend_->findData(keep.hardware_decode_backend);hardware_decode_backend_->setCurrentIndex(i>=0?i:0);
        }
        // GUI choices follow the selected FFmpeg -filters output, not a fixed
        // list of filters which might be omitted from the deployed build.
        // In multi-worker installations show the union of local/first-remote
        // capabilities; the target worker still checks before running.
        QStringList available=caps.ffmpeg_video_filters;
        for(const auto& filter:caps.remote_ffmpeg_video_filters)
            if(!available.contains(filter))available.push_back(filter);
        {
            const QSignalBlocker blocker(scaling_filter_backend_);
            scaling_filter_backend_->clear();
            scaling_filter_backend_->addItem("Software (Lanczos + pad, default)","software");
            scaling_filter_backend_->addItem("FFmpeg native (scale + pad, default swscale)","native");
            for(const auto& backend: {"vaapi","qsv","cuda","vulkan"}) {
                const QString value=QString::fromLatin1(backend);
                if(available.contains("scale_"+value))
                    scaling_filter_backend_->addItem(QStringLiteral("%1 (scale_%2)").arg(hardware_backend_label(value),value),value);
            }
            if(keep.scaling_filter_backend!="software"&&keep.scaling_filter_backend!="native"&&
               scaling_filter_backend_->findData(keep.scaling_filter_backend)<0)
                scaling_filter_backend_->addItem(QStringLiteral("%1 — unavailable (software fallback)").arg(keep.scaling_filter_backend),keep.scaling_filter_backend);
            int i=scaling_filter_backend_->findData(keep.scaling_filter_backend);
            scaling_filter_backend_->setCurrentIndex(i>=0?i:0);
        }
        {
            const QSignalBlocker blocker(tonemap_filter_backend_);
            tonemap_filter_backend_->clear();
            tonemap_filter_backend_->addItem("Software (zscale + Hable, quality-first, default)","software");
            tonemap_filter_backend_->addItem("Software fast (downscale HDR before zscale/Hable)","software-fast");
            if(available.contains("tonemap_vaapi"))
                tonemap_filter_backend_->addItem("VAAPI (tonemap_vaapi, HDR10 to SDR)","vaapi");
            if(keep.tonemap_filter_backend=="vaapi"&&tonemap_filter_backend_->findData("vaapi")<0)
                tonemap_filter_backend_->addItem("VAAPI — unavailable (software fallback)","vaapi");
            int i=tonemap_filter_backend_->findData(keep.tonemap_filter_backend);
            tonemap_filter_backend_->setCurrentIndex(i>=0?i:0);
        }
    }
    void rescan(){const auto requested=current();const auto caps=probe_tool_capabilities(requested);populate_hardware_backends(caps,requested);const auto old=g_tool_capabilities;g_tool_capabilities=caps;report_->setPlainText(tool_report(caps));g_tool_capabilities=old;}
public:
    ToolSettingsDialog(QWidget* parent,const ToolConfiguration& initial):QDialog(parent),value_(initial),remote_settings_(initial){setWindowTitle("External tools and encoders");resize(860,780);auto* root=new QVBoxLayout(this);auto* note=new QLabel("Leave a path blank to search PATH automatically. Hardware video encoding and decoding are performed by FFmpeg; the API lists below are populated from the active local or remote FFmpeg build. Encoding, decoding, scaling and tone mapping are four independent application settings, not stored in project files. Hardware filter failures retry using software filtering and retain the FFmpeg error. Hardware decode automatically falls back to software when the selected accelerator cannot decode the source. FFmpeg is preferred for software H.264/H.265 by default; standalone x264/x265 can be selected or used as fallback.",this);note->setWordWrap(true);root->addWidget(note);auto* form=new QFormLayout;root->addLayout(form);
        ffmpeg_=tool_row(this,form,"FFmpeg",initial.ffmpeg);ffprobe_=tool_row(this,form,"ffprobe",initial.ffprobe);x264_=tool_row(this,form,"x264",initial.x264);x265_=tool_row(this,form,"x265",initial.x265);
        vc1enc_=tool_row(this,form,"vc1enc (libvc1 VC-1, Experimental)",initial.vc1enc);vc1enc_->setPlaceholderText("Auto-detect from PATH or bundled sibling");
        hdmaenc_=tool_row(this,form,"hdmaenc (libhdma, lossless, experimental)",initial.hdmaenc);hdmaenc_->setPlaceholderText("Auto-detect from PATH or bundled sibling");
        tsmuxer_=tool_row(this,form,"tsMuxer",initial.tsmuxer);dvdauthor_=tool_row(this,form,"dvdauthor",initial.dvdauthor);spumux_=tool_row(this,form,"spumux",initial.spumux);bdsup2sub_=tool_row(this,form,"bdsup2sub (native bitmap subtitles)",initial.bdsup2sub);bdsup2sub_->setPlaceholderText("Auto-detect from PATH or built sibling executable");mplex_=tool_row(this,form,"mplex",initial.mplex);mkisofs_=tool_row(this,form,"mkisofs",initial.mkisofs);
        h264_provider_=new QComboBox(this);h264_provider_->addItem("FFmpeg/libx264 (preferred)","ffmpeg");h264_provider_->addItem("Standalone x264 (preferred)","standalone");h264_provider_->setCurrentIndex(h264_provider_->findData(initial.h264_provider));form->addRow("Software H.264 provider",h264_provider_);
        hevc_provider_=new QComboBox(this);hevc_provider_->addItem("FFmpeg/libx265 (preferred)","ffmpeg");hevc_provider_->addItem("Standalone x265 (preferred)","standalone");hevc_provider_->setCurrentIndex(hevc_provider_->findData(initial.hevc_provider));form->addRow("Software H.265 provider",hevc_provider_);
        hardware_encode_backend_=new QComboBox(this);hardware_encode_backend_->setToolTip("FFmpeg hardware encoder API used by the H.264/AVC (hardware) and H.265/HEVC (hardware) codec choices. Only APIs exposed by the selected FFmpeg build are listed. VideoToolbox is supported when present for a future macOS port.");form->addRow("Hardware encoding API",hardware_encode_backend_);
        hardware_decode_backend_=new QComboBox(this);hardware_decode_backend_->setToolTip("FFmpeg -hwaccel backend used on the machine doing the encode. For a compressed remote title relay this is the remote FFmpeg; otherwise it is local. If device initialization or a stream/profile is unsupported, BDMV Author retries with software decoding.");form->addRow("Hardware decoding API",hardware_decode_backend_);
        scaling_filter_backend_=new QComboBox(this);
        scaling_filter_backend_->setToolTip("Video scaling only; independent of hardware decoding or encoding. On failure, the software scaler is retried.");
        form->addRow("Video scaling filter",scaling_filter_backend_);
        tonemap_filter_backend_=new QComboBox(this);
        tonemap_filter_backend_->setToolTip("HDR10/PQ BT.2020 to SDR BT.709 only. The optional fast software mode scales HDR frames before linear-light conversion and tone mapping, which can change highlight appearance. Independent of video encoder and decoder.");
        form->addRow("HDR tone-mapping filter",tonemap_filter_backend_);
        filter_device_=new QLineEdit(initial.filter_hardware_device,this);
        filter_device_->setPlaceholderText("Auto (or /dev/dri/renderD128 for VAAPI)");
        form->addRow("Filter hardware device",filter_device_);
        remote_button_=new QPushButton(this);auto update_remote=[this]{int enabled=0;for(const auto&r:remote_settings_.remote_servers)if(r.enabled&&!r.host.trimmed().isEmpty())++enabled;const QString local=remote_settings_.local_encoding_enabled?QStringLiteral("enabled"):(remote_settings_.local_fallback_when_remote_unavailable?QStringLiteral("fallback-only"):QStringLiteral("disabled"));remote_button_->setText(QStringLiteral("%1 remote server(s); Local %2 …").arg(enabled).arg(local));};update_remote();form->addRow("Encoding workers",remote_button_);connect(remote_button_,&QPushButton::clicked,this,[this,update_remote]{RemoteServersDialog d(this,remote_settings_);if(d.exec()==QDialog::Accepted){remote_settings_=d.value();refresh_legacy_remote_fields(remote_settings_);update_remote();rescan();}});
        populate_hardware_backends(g_tool_capabilities,initial);
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
        if(lossless_audio_codec(value_.audio_codec)){auto* format=new QFormLayout;lossless_rate_=new QComboBox(this);if(target_!=DiscTarget::DvdVideo480p)lossless_rate_->addItem("Auto — match source / round up",0);for(int rate:{48000,96000,192000})if(lpcm_sample_rate_allowed_for_target(target_,rate))lossless_rate_->addItem(QString("%1 kHz").arg(rate/1000),rate);int ri=lossless_rate_->findData(value_.lpcm_sample_rate_hz);lossless_rate_->setCurrentIndex(ri>=0?ri:0);lossless_depth_=new QComboBox(this);if(target_!=DiscTarget::DvdVideo480p)lossless_depth_->addItem("Auto — match source / round up",0);lossless_depth_->addItem("16-bit",16);if(target_!=DiscTarget::Avchd)lossless_depth_->addItem("24-bit",24);int di=lossless_depth_->findData(value_.lpcm_bit_depth);lossless_depth_->setCurrentIndex(di>=0?di:0);format->addRow("Lossless sample rate",lossless_rate_);format->addRow("Lossless bit depth",lossless_depth_);root->addLayout(format);}
        root->addWidget(new QLabel(audio_codec_display_name(value_.audio_codec)+" options",this));audio_=new QPlainTextEdit(QString::fromStdString(advanced_audio_options_for_codec(value_)),this);audio_->setPlaceholderText("dialnorm=31");root->addWidget(audio_,1);
        auto* buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,this);root->addWidget(buttons);connect(buttons,&QDialogButtonBox::accepted,this,[this]{auto candidate=value_;advanced_video_options_for_codec(candidate)=video_->toPlainText().toStdString();advanced_audio_options_for_codec(candidate)=audio_->toPlainText().toStdString();if(lossless_rate_)candidate.lpcm_sample_rate_hz=lossless_rate_->currentData().toInt();if(lossless_depth_)candidate.lpcm_bit_depth=lossless_depth_->currentData().toInt();try{validate_advanced_codec_options(candidate,target_);}catch(const std::exception& e){QMessageBox::warning(this,"Invalid advanced codec option",QString::fromStdString(e.what()));return;}value_=std::move(candidate);QDialog::accept();});connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject);
    }
    const EncodingProfile& value()const{return value_;}
};
bool edit_advanced_codec_options(QWidget* parent,DiscTarget target,EncodingProfile& profile){AdvancedCodecOptionsDialog d(parent,target,profile);if(d.exec()!=QDialog::Accepted)return false;profile=d.value();return true;}

QVariant audio_stream_settings_variant(const std::vector<AudioStreamSettings>& settings){
    QVariantList out;for(const auto& stream:settings){QVariantMap m;m["sourceOrdinal"]=stream.source_ordinal;m["language"]=QString::fromStdString(stream.language);m["removed"]=stream.removed;m["overrideEncoding"]=stream.override_encoding;m["outputChannels"]=stream.output_channels;m["encodingWorker"]=QString::fromStdString(stream.encoding_worker);m["encodingWorkerMode"]=static_cast<int>(stream.encoding_worker_mode);m["blurayEncoding"]=encoding_map(stream.encoding);m["uhdEncoding"]=encoding_map(stream.uhd_encoding);m["avchdEncoding"]=encoding_map(stream.avchd_encoding);m["dvdEncoding"]=encoding_map(stream.dvd_encoding);out.push_back(m);}return out;
}
std::vector<AudioStreamSettings> audio_stream_settings_from_variant(const QVariant& value){
    std::vector<AudioStreamSettings> out;for(const auto& v:value.toList()){const auto m=v.toMap();AudioStreamSettings stream;stream.source_ordinal=m.value("sourceOrdinal").toInt();stream.language=m.value("language").toString().toStdString();stream.removed=m.value("removed",false).toBool();stream.override_encoding=m.value("overrideEncoding").toBool();stream.output_channels=m.value("outputChannels").toInt();stream.encoding_worker=m.value("encodingWorker").toString().toStdString();stream.encoding_worker_mode=static_cast<EncodingWorkerAffinityMode>(m.value("encodingWorkerMode",static_cast<int>(EncodingWorkerAffinityMode::Automatic)).toInt());stream.encoding=encoding_from_map(m.value("blurayEncoding"),EncodingProfile{});stream.uhd_encoding=encoding_from_map(m.value("uhdEncoding"),default_uhd_encoding_profile());stream.avchd_encoding=encoding_from_map(m.value("avchdEncoding"),default_avchd_encoding_profile());stream.dvd_encoding=encoding_from_map(m.value("dvdEncoding"),default_dvd_encoding_profile());out.push_back(std::move(stream));}return out;
}
QVariantMap subtitle_style_map(const SubtitleStyle& st){QVariantMap m;m["overrideStyle"]=st.override_style;m["fontFamily"]=QString::fromStdString(st.font_family);m["fontSizeUnits"]=st.font_size_px;m["fontColorR"]=static_cast<int>(st.font_color.r);m["fontColorG"]=static_cast<int>(st.font_color.g);m["fontColorB"]=static_cast<int>(st.font_color.b);m["fontColorA"]=static_cast<int>(st.font_color.a);m["bold"]=st.bold;m["italic"]=st.italic;m["underline"]=st.underline;m["strikeout"]=st.strikeout;m["lineSpacing"]=st.line_spacing;m["borderWidth"]=st.border_width;m["bottomOffsetUnits"]=st.bottom_offset_px;m["fadeInMs"]=st.fade_in_ms;m["fadeOutMs"]=st.fade_out_ms;m["dvdOutlineR"]=static_cast<int>(st.dvd_outline_color.r);m["dvdOutlineG"]=static_cast<int>(st.dvd_outline_color.g);m["dvdOutlineB"]=static_cast<int>(st.dvd_outline_color.b);m["dvdOutlineA"]=static_cast<int>(st.dvd_outline_color.a);m["dvdShadowR"]=static_cast<int>(st.dvd_shadow_color.r);m["dvdShadowG"]=static_cast<int>(st.dvd_shadow_color.g);m["dvdShadowB"]=static_cast<int>(st.dvd_shadow_color.b);m["dvdShadowA"]=static_cast<int>(st.dvd_shadow_color.a);m["dvdShadowOffsetX"]=st.dvd_shadow_offset_x;m["dvdShadowOffsetY"]=st.dvd_shadow_offset_y;m["dvdHorizontalAlignment"]=QString::fromStdString(st.dvd_horizontal_alignment);m["dvdVerticalAlignment"]=QString::fromStdString(st.dvd_vertical_alignment);m["dvdLeftMarginPx"]=st.dvd_left_margin_px;m["dvdRightMarginPx"]=st.dvd_right_margin_px;m["dvdTopMarginPx"]=st.dvd_top_margin_px;m["dvdBottomMarginPx"]=st.dvd_bottom_margin_px;m["dvdForceDisplay"]=st.dvd_force_display;return m;}
SubtitleStyle subtitle_style_from_map(const QVariant& v){const auto m=v.toMap();SubtitleStyle st;st.override_style=m.value("overrideStyle",false).toBool();st.font_family=m.value("fontFamily",QString()).toString().toStdString();st.font_size_px=m.contains("fontSizeUnits")?m.value("fontSizeUnits",80).toInt():m.value("fontSizePx",80).toInt();st.font_color={static_cast<std::uint8_t>(m.value("fontColorR",255).toInt()),static_cast<std::uint8_t>(m.value("fontColorG",255).toInt()),static_cast<std::uint8_t>(m.value("fontColorB",255).toInt()),static_cast<std::uint8_t>(m.value("fontColorA",255).toInt())};st.bold=m.value("bold",false).toBool();st.italic=m.value("italic",false).toBool();st.underline=m.value("underline",false).toBool();st.strikeout=m.value("strikeout",false).toBool();st.line_spacing=m.value("lineSpacing",1.0).toDouble();st.border_width=m.value("borderWidth",2.0).toDouble();st.bottom_offset_px=m.contains("bottomOffsetUnits")?m.value("bottomOffsetUnits",80).toInt():m.value("bottomOffsetPx",80).toInt();st.fade_in_ms=m.value("fadeInMs",0.0).toDouble();st.fade_out_ms=m.value("fadeOutMs",0.0).toDouble();st.dvd_outline_color={static_cast<std::uint8_t>(m.value("dvdOutlineR",0).toInt()),static_cast<std::uint8_t>(m.value("dvdOutlineG",0).toInt()),static_cast<std::uint8_t>(m.value("dvdOutlineB",0).toInt()),static_cast<std::uint8_t>(m.value("dvdOutlineA",255).toInt())};st.dvd_shadow_color={static_cast<std::uint8_t>(m.value("dvdShadowR",0).toInt()),static_cast<std::uint8_t>(m.value("dvdShadowG",0).toInt()),static_cast<std::uint8_t>(m.value("dvdShadowB",0).toInt()),static_cast<std::uint8_t>(m.value("dvdShadowA",255).toInt())};st.dvd_shadow_offset_x=m.value("dvdShadowOffsetX",0).toInt();st.dvd_shadow_offset_y=m.value("dvdShadowOffsetY",0).toInt();st.dvd_horizontal_alignment=m.value("dvdHorizontalAlignment","center").toString().toStdString();st.dvd_vertical_alignment=m.value("dvdVerticalAlignment","bottom").toString().toStdString();st.dvd_left_margin_px=m.value("dvdLeftMarginPx",40).toInt();st.dvd_right_margin_px=m.value("dvdRightMarginPx",40).toInt();st.dvd_top_margin_px=m.value("dvdTopMarginPx",20).toInt();st.dvd_bottom_margin_px=m.value("dvdBottomMarginPx",36).toInt();st.dvd_force_display=m.value("dvdForceDisplay",false).toBool();return st;}
QVariant subtitle_stream_settings_variant(const std::vector<SubtitleStreamSettings>& settings){QVariantList out;for(const auto& stream:settings){QVariantMap m;m["sourceOrdinal"]=stream.source_ordinal;m["language"]=QString::fromStdString(stream.language);m["removed"]=stream.removed;m["style"]=subtitle_style_map(stream.style);out.push_back(m);}return out;}
std::vector<SubtitleStreamSettings> subtitle_stream_settings_from_variant(const QVariant& value){std::vector<SubtitleStreamSettings> out;for(const auto& v:value.toList()){const auto m=v.toMap();SubtitleStreamSettings stream;stream.source_ordinal=m.value("sourceOrdinal").toInt();stream.language=m.value("language").toString().toStdString();stream.removed=m.value("removed",false).toBool();if(m.contains("style"))stream.style=subtitle_style_from_map(m.value("style"));out.push_back(std::move(stream));}return out;}
QVariant external_subtitles_variant(const std::vector<ExternalSubtitle>& subtitles){QVariantList out;for(const auto& sub:subtitles){QVariantMap m;m["source"]=QString::fromStdString(sub.source.string());m["language"]=QString::fromStdString(sub.language);m["style"]=subtitle_style_map(sub.style);out.push_back(m);}return out;}
std::vector<ExternalSubtitle> external_subtitles_from_variant(const QVariant& value){std::vector<ExternalSubtitle> out;for(const auto& v:value.toList()){const auto m=v.toMap();ExternalSubtitle sub;sub.source=m.value("source").toString().toStdString();sub.language=m.value("language","und").toString().toStdString();if(m.contains("style"))sub.style=subtitle_style_from_map(m.value("style"));if(!sub.source.empty())out.push_back(std::move(sub));}return out;}



void clear_menu_worker_affinity(Menu& menu){menu.encoding_worker.clear();menu.encoding_worker_mode=EncodingWorkerAffinityMode::Automatic;for(auto& child:menu.submenus)clear_menu_worker_affinity(child);}
void clear_project_worker_affinities(Project& project){
    clear_menu_worker_affinity(project.menu);
    for(auto& title:project.titles){title.encoding_worker.clear();title.encoding_worker_mode=EncodingWorkerAffinityMode::Automatic;for(auto& audio:title.audio_stream_settings){audio.encoding_worker.clear();audio.encoding_worker_mode=EncodingWorkerAffinityMode::Automatic;}}
}
QString local_worker_affinity_path(const QString& project_path){
    const auto absolute=QFileInfo(project_path).absoluteFilePath().toUtf8();const auto digest=QCryptographicHash::hash(absolute,QCryptographicHash::Sha256).toHex();
    const QString root=QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)+QDir::separator()+"worker-affinity";QDir().mkpath(root);return root+QDir::separator()+QString::fromLatin1(digest)+".json";
}
void append_menu_worker_affinity(QJsonArray& out,const Menu& menu){
    if(!menu.id.empty()&&!menu.encoding_worker.empty()){QJsonObject o;o["id"]=QString::fromStdString(menu.id);o["worker"]=QString::fromStdString(menu.encoding_worker);o["mode"]=static_cast<int>(menu.encoding_worker_mode);out.push_back(o);}for(const auto& child:menu.submenus)append_menu_worker_affinity(out,child);
}
Menu* find_menu_by_id(Menu& menu,const std::string& id){if(menu.id==id)return &menu;for(auto& child:menu.submenus)if(auto* found=find_menu_by_id(child,id))return found;return nullptr;}
bool save_local_worker_affinities(const Project& project,const QString& project_path,QString* error=nullptr){
    QJsonObject root;root["version"]=1;root["projectPath"]=QFileInfo(project_path).absoluteFilePath();QJsonArray titles;
    for(int i=0;i<static_cast<int>(project.titles.size());++i){const auto& t=project.titles[static_cast<std::size_t>(i)];QJsonObject o;o["index"]=i;if(!t.encoding_worker.empty()){o["worker"]=QString::fromStdString(t.encoding_worker);o["mode"]=static_cast<int>(t.encoding_worker_mode);}QJsonArray audio;for(const auto& a:t.audio_stream_settings)if(!a.encoding_worker.empty()){QJsonObject ao;ao["sourceOrdinal"]=a.source_ordinal;ao["worker"]=QString::fromStdString(a.encoding_worker);ao["mode"]=static_cast<int>(a.encoding_worker_mode);audio.push_back(ao);}if(!audio.isEmpty())o["audio"]=audio;if(o.size()>1)titles.push_back(o);}root["titles"]=titles;QJsonArray menus;append_menu_worker_affinity(menus,project.menu);root["menus"]=menus;
    QFile f(local_worker_affinity_path(project_path));if(!f.open(QIODevice::WriteOnly|QIODevice::Truncate)){if(error)*error="Could not write the local worker-assignment companion file.";return false;}if(f.write(QJsonDocument(root).toJson(QJsonDocument::Indented))<0){if(error)*error="Could not write the local worker-assignment companion file.";return false;}return true;
}
void apply_local_worker_affinities(Project& project,const QString& project_path){
    QFile f(local_worker_affinity_path(project_path));if(!f.open(QIODevice::ReadOnly))return;QJsonParseError pe;const auto doc=QJsonDocument::fromJson(f.readAll(),&pe);if(pe.error!=QJsonParseError::NoError||!doc.isObject())return;const auto root=doc.object();
    for(const auto& v:root.value("titles").toArray()){const auto o=v.toObject();const int i=o.value("index").toInt(-1);if(i<0||i>=static_cast<int>(project.titles.size()))continue;auto& t=project.titles[static_cast<std::size_t>(i)];if(o.contains("worker")){t.encoding_worker=o.value("worker").toString().toStdString();t.encoding_worker_mode=static_cast<EncodingWorkerAffinityMode>(o.value("mode").toInt(static_cast<int>(EncodingWorkerAffinityMode::Preferred)));}for(const auto& av:o.value("audio").toArray()){const auto ao=av.toObject();const int ordinal=ao.value("sourceOrdinal").toInt(-1);auto it=std::find_if(t.audio_stream_settings.begin(),t.audio_stream_settings.end(),[&](const AudioStreamSettings&a){return a.source_ordinal==ordinal;});if(it==t.audio_stream_settings.end()){AudioStreamSettings a;a.source_ordinal=ordinal;t.audio_stream_settings.push_back(a);it=std::prev(t.audio_stream_settings.end());}it->encoding_worker=ao.value("worker").toString().toStdString();it->encoding_worker_mode=static_cast<EncodingWorkerAffinityMode>(ao.value("mode").toInt(static_cast<int>(EncodingWorkerAffinityMode::Preferred)));}}
    for(const auto& v:root.value("menus").toArray()){const auto o=v.toObject();if(auto* m=find_menu_by_id(project.menu,o.value("id").toString().toStdString())){m->encoding_worker=o.value("worker").toString().toStdString();m->encoding_worker_mode=static_cast<EncodingWorkerAffinityMode>(o.value("mode").toInt(static_cast<int>(EncodingWorkerAffinityMode::Preferred)));}}
}
void remove_local_worker_affinities(const QString& project_path){QFile::remove(local_worker_affinity_path(project_path));}

void populate_worker_affinity_combo(QComboBox* combo,const std::string& worker,EncodingWorkerAffinityMode mode,const QString& automatic_label="Automatic scheduling"){
    const QSignalBlocker blocker(combo);combo->clear();combo->addItem(automatic_label,QString());combo->setItemData(0,static_cast<int>(EncodingWorkerAffinityMode::Automatic),Qt::UserRole+1);
    const auto cfg=load_tool_configuration();
    auto add_worker=[&](const QString& name,const QString& suffix={}){
        combo->addItem(QStringLiteral("Preferred — %1%2").arg(name,suffix),name);combo->setItemData(combo->count()-1,static_cast<int>(EncodingWorkerAffinityMode::Preferred),Qt::UserRole+1);
        combo->addItem(QStringLiteral("Required — %1%2").arg(name,suffix),name);combo->setItemData(combo->count()-1,static_cast<int>(EncodingWorkerAffinityMode::Required),Qt::UserRole+1);
    };
    if(cfg.local_encoding_enabled||cfg.local_fallback_when_remote_unavailable)add_worker("Local",cfg.local_encoding_enabled?QString():QStringLiteral(" (fallback-only)"));
    QSet<QString> seen;for(const auto& r:cfg.remote_servers){if(!r.enabled||r.host.trimmed().isEmpty())continue;const QString name=r.name.trimmed().isEmpty()?r.host.trimmed():r.name.trimmed();if(seen.contains(name))continue;seen.insert(name);add_worker(name);}
    const QString wanted=QString::fromStdString(worker);int found=-1;for(int i=0;i<combo->count();++i)if(combo->itemData(i).toString()==wanted&&combo->itemData(i,Qt::UserRole+1).toInt()==static_cast<int>(mode)){found=i;break;}
    if(found<0&&!wanted.isEmpty()){combo->addItem(QStringLiteral("%1 — %2 (not currently configured)").arg(mode==EncodingWorkerAffinityMode::Required?QStringLiteral("Required"):QStringLiteral("Preferred"),wanted),wanted);combo->setItemData(combo->count()-1,static_cast<int>(mode),Qt::UserRole+1);found=combo->count()-1;}
    combo->setCurrentIndex(found>=0?found:0);
}
std::string worker_from_affinity_combo(const QComboBox* combo){return combo?combo->currentData().toString().toStdString():std::string{};}
EncodingWorkerAffinityMode mode_from_affinity_combo(const QComboBox* combo){return combo?static_cast<EncodingWorkerAffinityMode>(combo->currentData(Qt::UserRole+1).toInt()):EncodingWorkerAffinityMode::Automatic;}

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
        QCheckBox* remove=nullptr;
        QCheckBox* custom=nullptr;
        QComboBox* codec=nullptr;
        QComboBox* channels=nullptr;
        QSpinBox* bitrate=nullptr;
        QComboBox* rate=nullptr;
        QComboBox* depth=nullptr;
        QComboBox* worker=nullptr;
        QPushButton* advanced=nullptr;
    };
    struct SubtitleRow { GuiSubtitleTrack source; SubtitleStreamSettings value; QLineEdit* language=nullptr; QCheckBox* remove=nullptr; QPushButton* style=nullptr; };
    DiscTarget target_;
    int video_bitrate_kbps_;
    bool allow_exceeding_format_limits_=false;
    EncodingProfile hd_default_,uhd_default_,avchd_default_,dvd_default_;
    SubtitleStyle default_subtitle_style_;
    std::vector<AudioRow> audio_;
    std::vector<SubtitleRow> subtitles_;
    QTableWidget* external_table_=nullptr;
    QComboBox* default_audio_=nullptr;
    QComboBox* default_subtitle_=nullptr;
    QLabel* total_=nullptr;
    QDialogButtonBox* buttons_=nullptr;
    QComboBox* title_worker_=nullptr;

    const EncodingProfile& fallback() const {
        switch(target_){case DiscTarget::UltraHdBluRay2160:return uhd_default_;case DiscTarget::Avchd:return avchd_default_;case DiscTarget::DvdVideo480p:return dvd_default_;case DiscTarget::BluRay1080:return hd_default_;}
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
        if(lossless_audio_codec(e.audio_codec))return lpcm_max_channels_for_target(target_,e.lpcm_sample_rate_hz>0?e.lpcm_sample_rate_hz:48000);
#if defined(BDMVAUTHOR_HAVE_EAC371ENC)
        if(e.audio_codec==AudioCodec::Ac3&&target_supports_ac3_71_extension(target_))return 8;
#endif
        return 6;
    }
    int output_channels(const AudioRow& row,const EncodingProfile& e) const {
        const int requested=row.channels->currentData().toInt();
        if(requested>0)return requested;
        const int maximum=max_channels(e);
#if defined(BDMVAUTHOR_HAVE_EAC371ENC)
        if(e.audio_codec==AudioCodec::Ac3&&maximum>=8)return row.source.channels>=8?8:std::min(std::max(1,row.source.channels),6);
#endif
        return std::min(std::max(1,row.source.channels),maximum);
    }
    bool row_peak_measured_during_authoring(const AudioRow& row) const {
        return !(row.remove&&row.remove->isChecked())&&lossless_audio_codec(profile_from_row(row).audio_codec)&&profile_from_row(row).audio_codec!=AudioCodec::Lpcm;
    }
    int row_fixed_bitrate(const AudioRow& row) const {
        if(row.remove&&row.remove->isChecked())return 0;
        const auto e=profile_from_row(row);const int channels=output_channels(row,e);
        if(e.audio_codec==AudioCodec::Lpcm)return lpcm_bitrate_kbps(e.lpcm_sample_rate_hz,e.lpcm_bit_depth,channels);
        if(e.audio_codec==AudioCodec::Dca)return e.dca_bitrate_kbps;
        if(e.audio_codec==AudioCodec::TrueHdAc3 || e.audio_codec==AudioCodec::Libhdma)return 0;
        return e.ac3_bitrate_kbps;
    }
    void refresh_row(AudioRow& row){
        const bool removed=row.remove&&row.remove->isChecked();const bool custom=!removed&&row.custom->isChecked();row.language->setEnabled(!removed);row.custom->setEnabled(!removed);row.codec->setEnabled(custom);row.channels->setEnabled(!removed);if(row.worker)row.worker->setEnabled(!removed);const auto e=profile_from_row(row);const bool lossless=lossless_audio_codec(e.audio_codec);
        row.rate->setEnabled(custom&&lossless);row.depth->setEnabled(custom&&lossless);row.advanced->setEnabled(custom);
        const int maximum=max_channels(e);
        for(int i=0;i<row.channels->count();++i){const int ch=row.channels->itemData(i).toInt();set_combo_item_enabled(row.channels,i,ch==0||(ch<=row.source.channels&&ch<=maximum),ch>row.source.channels?"Upmixing is not supported.":"The selected codec/target does not support this many channels.");}
        if(e.audio_codec==AudioCodec::Lpcm || e.audio_codec==AudioCodec::Libhdma){const QSignalBlocker blocker(row.bitrate);row.bitrate->setEnabled(!removed);row.bitrate->setReadOnly(true);row.bitrate->setButtonSymbols(QAbstractSpinBox::NoButtons);row.bitrate->setRange(0,40000);row.bitrate->setSuffix(" kb/s");row.bitrate->setValue(row_fixed_bitrate(row));if(e.audio_codec==AudioCodec::Libhdma)row.bitrate->setSpecialValueText("Measured");}
        else{row.bitrate->setReadOnly(false);row.bitrate->setButtonSymbols(QAbstractSpinBox::UpDownArrows);configure_audio_bitrate_spin(row.bitrate,target_,e.audio_codec,audio_bitrate_kbps(e),allow_exceeding_format_limits_);
            if(!allow_exceeding_format_limits_&&e.audio_codec==AudioCodec::Ac3&&output_channels(row,e)<=6&&row.bitrate->maximum()>640){const QSignalBlocker blocker(row.bitrate);row.bitrate->setMaximum(640);row.bitrate->setValue(std::min(row.bitrate->value(),640));}
            row.bitrate->setEnabled(!removed&&custom&&audio_codec_has_configurable_bitrate(e.audio_codec));}
        if(row.depth->findData(24)>=0){const bool depth24ok=target_==DiscTarget::DvdVideo480p?g_tool_capabilities.ffmpeg_pcm_s24be:g_tool_capabilities.ffmpeg_pcm_s24le;set_combo_item_enabled(row.depth,row.depth->findData(24),depth24ok,"The selected FFmpeg lacks a 24-bit PCM encoder.");}
    }
    void refresh_total(){
        int fixed_audio=0;int measured_tracks=0;for(auto& row:audio_){refresh_row(row);if(row_peak_measured_during_authoring(row))++measured_tracks;else fixed_audio+=row_fixed_bitrate(row);}
        const int fixed_combined=video_bitrate_kbps_+fixed_audio;const int maximum=target_max_combined_av_bitrate_kbps(target_);
        if(measured_tracks>0){
            total_->setText(QString("Configured fixed-rate ceiling: video %1 + fixed audio %2 = %3 kb/s (target maximum %4 kb/s). %5 THD track(s): peak bitrate is measured during authoring before video encoding, and the video VBV/maxrate is reduced to fit the remaining transport budget.").arg(video_bitrate_kbps_).arg(fixed_audio).arg(fixed_combined).arg(maximum).arg(measured_tracks));
        }else{
            total_->setText(QString("Configured encode ceiling: video %1 + audio %2 = %3 kb/s (target maximum %4 kb/s)").arg(video_bitrate_kbps_).arg(fixed_audio).arg(fixed_combined).arg(maximum));
        }
        total_->setWordWrap(true);
        const bool over=!allow_exceeding_format_limits_&&fixed_combined>maximum;total_->setStyleSheet(over?"color: #c62828; font-weight: bold;":"");if(buttons_)buttons_->button(QDialogButtonBox::Ok)->setEnabled(!over);
    }
    static bool valid_language(const QString& s){if(s.trimmed().isEmpty())return true;const auto t=s.trimmed();if(t.size()!=3)return false;for(const auto ch:t)if(!ch.isLetter())return false;return true;}
    static bool external_is_text(const QString& path){const auto ext=QFileInfo(path).suffix().toLower();return ext!="sup"&&ext!="pgs";}
    SubtitleStyle style_for_edit(const SubtitleStyle& current) const {if(current.override_style)return current;auto inherited=default_subtitle_style_;inherited.override_style=false;return inherited;}
    static int find_combo_key(const QComboBox* combo,const QString& key){for(int i=0;i<combo->count();++i)if(combo->itemData(i,Qt::UserRole+1).toString()==key)return i;return -1;}
    void refresh_default_audio_choices(int preferred=std::numeric_limits<int>::min(),QString preferred_key={}){
        if(!default_audio_)return;
        if(preferred_key.isEmpty()&&preferred==std::numeric_limits<int>::min())preferred_key=default_audio_->currentData(Qt::UserRole+1).toString();
        const QSignalBlocker blocker(default_audio_);default_audio_->clear();default_audio_->addItem("Player default",0);default_audio_->setItemData(0,"player",Qt::UserRole+1);
        int authored=1;for(const auto& row:audio_){if(row.remove&&row.remove->isChecked())continue;const auto lang=row.language?row.language->text().trimmed():QString{};const auto shown=lang.isEmpty()?row.source.language:lang;default_audio_->addItem(QString("Audio %1 — %2 (%3, %4 ch)").arg(authored).arg(shown.isEmpty()?QStringLiteral("und"):shown).arg(row.source.codec).arg(row.source.channels),authored);default_audio_->setItemData(default_audio_->count()-1,QString("audio:%1").arg(row.source.ordinal),Qt::UserRole+1);++authored;}
        int index=!preferred_key.isEmpty()?find_combo_key(default_audio_,preferred_key):default_audio_->findData(preferred);if(index<0)index=default_audio_->findData(0);default_audio_->setCurrentIndex(std::max(0,index));
    }
    void refresh_default_subtitle_choices(int preferred=std::numeric_limits<int>::min(),QString preferred_key={}){
        if(!default_subtitle_)return;
        if(preferred_key.isEmpty()&&preferred==std::numeric_limits<int>::min())preferred_key=default_subtitle_->currentData(Qt::UserRole+1).toString();
        const QSignalBlocker blocker(default_subtitle_);default_subtitle_->clear();
        default_subtitle_->addItem("Player default",-1);default_subtitle_->setItemData(0,"player",Qt::UserRole+1);default_subtitle_->addItem("Subtitles off",0);default_subtitle_->setItemData(1,"off",Qt::UserRole+1);
        int authored=1;for(const auto& row:subtitles_){if(row.remove&&row.remove->isChecked())continue;const auto lang=row.language?row.language->text().trimmed():QString{};const auto shown=lang.isEmpty()?row.source.language:lang;default_subtitle_->addItem(QString("Subtitle %1 — %2 (%3)").arg(authored).arg(shown.isEmpty()?QStringLiteral("und"):shown).arg(row.source.codec),authored);default_subtitle_->setItemData(default_subtitle_->count()-1,QString("subtitle:%1").arg(row.source.ordinal),Qt::UserRole+1);++authored;}
        if(external_table_)for(int r=0;r<external_table_->rowCount();++r){const auto* path=external_table_->item(r,0);const auto* lang=external_table_->item(r,1);default_subtitle_->addItem(QString("Subtitle %1 — %2 (attached: %3)").arg(authored).arg(lang?lang->text():QStringLiteral("und")).arg(path?QFileInfo(path->text()).fileName():QStringLiteral("file")),authored);default_subtitle_->setItemData(default_subtitle_->count()-1,QString("external:%1").arg(path?path->text():QString::number(r)),Qt::UserRole+1);++authored;}
        int index=!preferred_key.isEmpty()?find_combo_key(default_subtitle_,preferred_key):default_subtitle_->findData(preferred);if(index<0)index=default_subtitle_->findData(-1);default_subtitle_->setCurrentIndex(std::max(0,index));
    }
    void add_external_row(const QString& path,const QString& language,const SubtitleStyle& style={}){
        const int row=external_table_->rowCount();external_table_->insertRow(row);auto* path_item=new QTableWidgetItem(path);path_item->setFlags(path_item->flags()&~Qt::ItemIsEditable);path_item->setData(Qt::UserRole+17,subtitle_style_map(style));external_table_->setItem(row,0,path_item);external_table_->setItem(row,1,new QTableWidgetItem(language.isEmpty()?QStringLiteral("und"):language.toLower()));auto* button=new QPushButton(style.override_style?"Properties…":"Defaults…",external_table_);external_table_->setCellWidget(row,2,button);connect(button,&QPushButton::clicked,this,[this,button]{int r=-1;for(int i=0;i<external_table_->rowCount();++i)if(external_table_->cellWidget(i,2)==button){r=i;break;}if(r<0)return;auto* item=external_table_->item(r,0);if(!item)return;const auto current=subtitle_style_from_map(item->data(Qt::UserRole+17));SubtitleStyleDialog d(this,target_,external_is_text(item->text()),style_for_edit(current));if(d.exec()!=QDialog::Accepted)return;const auto updated=d.value();item->setData(Qt::UserRole+17,subtitle_style_map(updated));button->setText(updated.override_style?"Properties…":"Defaults…");});
    }
public:
    TitleStreamsDialog(QWidget* parent,const QString& source,DiscTarget target,int video_bitrate,const EncodingProfile& hd,const EncodingProfile& uhd,const EncodingProfile& avchd,const EncodingProfile& dvd,const SubtitleStyle& default_subtitle_style,const std::vector<AudioStreamSettings>& current_audio,const std::vector<SubtitleStreamSettings>& current_subtitle,const std::vector<ExternalSubtitle>& current_external,const std::string& title_worker,EncodingWorkerAffinityMode title_worker_mode,int current_default_audio=0,int current_default_subtitle=-1,bool allow_exceeding_format_limits=false)
        :QDialog(parent),target_(target),video_bitrate_kbps_(video_bitrate),allow_exceeding_format_limits_(allow_exceeding_format_limits),hd_default_(hd),uhd_default_(uhd),avchd_default_(avchd),dvd_default_(dvd),default_subtitle_style_(default_subtitle_style){
        setWindowTitle("Title audio and subtitle streams");resize(1450,700);auto* root=new QVBoxLayout(this);
        auto* note=new QLabel("Language overrides preserve the source tag when blank. Check Remove to omit an embedded audio or subtitle stream entirely from the authored title. Custom encoding controls codec/bitrate and LPCM/THD lossless format settings. Channel layout can be reduced independently; selecting fewer channels forces an encode with the title fallback codec unless Custom encoding is also enabled. Upmixing is rejected.",this);note->setWordWrap(true);root->addWidget(note);
        std::vector<GuiAudioTrack> audio_sources;std::vector<GuiSubtitleTrack> subtitle_sources;QString error;if(!probe_gui_title_streams(source,audio_sources,subtitle_sources,&error))root->addWidget(new QLabel(error,this));
        auto* defaults_box=new QGroupBox("Playback defaults",this);auto* defaults_form=new QFormLayout(defaults_box);default_audio_=new QComboBox(defaults_box);default_subtitle_=new QComboBox(defaults_box);defaults_form->addRow("Default audio",default_audio_);defaults_form->addRow("Default subtitles",default_subtitle_);title_worker_=new QComboBox(defaults_box);populate_worker_affinity_combo(title_worker_,title_worker,title_worker_mode,"Automatic scheduling");title_worker_->setToolTip("Preferred tries this worker first and falls through to another compatible worker. Required pins title video to this worker.");defaults_form->addRow("Title video worker",title_worker_);auto* defaults_note=new QLabel("Player default preserves the player's normal language selection. Subtitles off explicitly starts this title with subtitle display disabled. Explicit choices use the authored one-based stream order.",defaults_box);defaults_note->setWordWrap(true);defaults_form->addRow(defaults_note);root->addWidget(defaults_box);
        auto* tabs=new QTabWidget(this);root->addWidget(tabs,1);
        auto* audio_page=new QWidget;auto* ag=new QGridLayout(audio_page);audio_.reserve(audio_sources.size());const QStringList ah={"#","Source","Ch","Language override","Remove","Custom encoding","Codec","Output channels","Bitrate","Lossless rate","Lossless depth","Encoding worker","Advanced"};for(qsizetype c=0;c<ah.size();++c)ag->addWidget(new QLabel(ah[c],audio_page),0,static_cast<int>(c));
        int ar=1;for(const auto& src:audio_sources){
            AudioStreamSettings value;value.source_ordinal=src.ordinal;value.encoding=hd;value.uhd_encoding=uhd;value.avchd_encoding=avchd;value.dvd_encoding=dvd;if(const auto it=std::find_if(current_audio.begin(),current_audio.end(),[&](const AudioStreamSettings& x){return x.source_ordinal==src.ordinal;});it!=current_audio.end())value=*it;
            AudioRow row;row.source=src;row.value=value;ag->addWidget(new QLabel(QString::number(src.ordinal+1),audio_page),ar,0);ag->addWidget(new QLabel(src.codec,audio_page),ar,1);ag->addWidget(new QLabel(QString::number(src.channels),audio_page),ar,2);
            row.language=new QLineEdit(QString::fromStdString(value.language),audio_page);row.language->setPlaceholderText(src.language+" (source)");ag->addWidget(row.language,ar,3);
            row.remove=new QCheckBox(audio_page);row.remove->setChecked(value.removed);row.remove->setToolTip("Omit this audio stream from the authored title.");ag->addWidget(row.remove,ar,4);
            row.custom=new QCheckBox(audio_page);row.custom->setChecked(value.override_encoding);ag->addWidget(row.custom,ar,5);const auto active=value.override_encoding?encoding_for_target(value,target):fallback();row.codec=audio_combo(audio_page,target,active.audio_codec);ag->addWidget(row.codec,ar,6);
            row.channels=new QComboBox(audio_page);row.channels->addItem(QString("Source (%1 ch)").arg(src.channels),0);if(src.channels>=8)row.channels->addItem("7.1 (8 ch)",8);if(src.channels>=6)row.channels->addItem("5.1 (6 ch)",6);if(src.channels>=4)row.channels->addItem("Quad (4 ch)",4);if(src.channels>=2)row.channels->addItem("Stereo (2 ch)",2);row.channels->addItem("Mono (1 ch)",1);row.channels->setCurrentIndex(std::max(0,row.channels->findData(value.output_channels)));ag->addWidget(row.channels,ar,7);
            row.bitrate=audio_bitrate_spin(audio_page);configure_audio_bitrate_spin(row.bitrate,target_,active.audio_codec,audio_bitrate_kbps(active),allow_exceeding_format_limits_);ag->addWidget(row.bitrate,ar,8);row.rate=new QComboBox(audio_page);if(target!=DiscTarget::DvdVideo480p)row.rate->addItem("Auto",0);for(int rate:{48000,96000,192000})if(lpcm_sample_rate_allowed_for_target(target,rate))row.rate->addItem(QString("%1 kHz").arg(rate/1000),rate);row.rate->setCurrentIndex(std::max(0,row.rate->findData(active.lpcm_sample_rate_hz)));ag->addWidget(row.rate,ar,9);row.depth=new QComboBox(audio_page);if(target!=DiscTarget::DvdVideo480p)row.depth->addItem("Auto",0);row.depth->addItem("16-bit",16);row.depth->addItem("24-bit",24);row.depth->setCurrentIndex(std::max(0,row.depth->findData(active.lpcm_bit_depth)));ag->addWidget(row.depth,ar,10);row.worker=new QComboBox(audio_page);populate_worker_affinity_combo(row.worker,value.encoding_worker,value.encoding_worker_mode,"Inherit title assignment");row.worker->setToolTip("Choose a Preferred or Required worker for this audio track, or inherit the title assignment.");ag->addWidget(row.worker,ar,11);row.advanced=new QPushButton("Advanced…",audio_page);ag->addWidget(row.advanced,ar,12);
            audio_.push_back(row);auto* rp=&audio_.back();connect(rp->remove,&QCheckBox::toggled,this,[this]{const auto key=default_audio_->currentData(Qt::UserRole+1).toString();refresh_total();refresh_default_audio_choices(std::numeric_limits<int>::min(),key);});connect(rp->custom,&QCheckBox::toggled,this,[this]{refresh_total();});connect(rp->codec,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this]{refresh_total();});connect(rp->channels,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this]{refresh_total();});connect(rp->bitrate,QOverload<int>::of(&QSpinBox::valueChanged),this,[this]{refresh_total();});connect(rp->rate,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this]{refresh_total();});connect(rp->depth,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this]{refresh_total();});connect(rp->advanced,&QPushButton::clicked,this,[this,rp]{auto e=profile_from_row(*rp);AudioAdvancedOptionsOnlyDialog d(this,target_,e);if(d.exec()!=QDialog::Accepted)return;auto& target_profile=encoding_for_target(rp->value,target_);copy_audio_encoding_settings(target_profile,d.value());refresh_total();});++ar;
        }ag->setColumnStretch(3,1);refresh_default_audio_choices(current_default_audio);tabs->addTab(audio_page,QString("Audio (%1)").arg(audio_sources.size()));

        auto* sub_page=new QWidget;auto* sub_root=new QVBoxLayout(sub_page);auto* embedded=new QWidget(sub_page);auto* sg=new QGridLayout(embedded);subtitles_.reserve(subtitle_sources.size());sg->addWidget(new QLabel("#",embedded),0,0);sg->addWidget(new QLabel("Source codec",embedded),0,1);sg->addWidget(new QLabel("Source language",embedded),0,2);sg->addWidget(new QLabel("Language override (ISO 639-2, blank = source)",embedded),0,3);sg->addWidget(new QLabel("Remove",embedded),0,4);sg->addWidget(new QLabel("Properties",embedded),0,5);int sr=1;
        for(const auto& src:subtitle_sources){SubtitleStreamSettings value;value.source_ordinal=src.ordinal;if(const auto it=std::find_if(current_subtitle.begin(),current_subtitle.end(),[&](const SubtitleStreamSettings& x){return x.source_ordinal==src.ordinal;});it!=current_subtitle.end())value=*it;SubtitleRow row;row.source=src;row.value=value;sg->addWidget(new QLabel(QString::number(src.ordinal+1),embedded),sr,0);sg->addWidget(new QLabel(src.codec,embedded),sr,1);sg->addWidget(new QLabel(src.language,embedded),sr,2);row.language=new QLineEdit(QString::fromStdString(value.language),embedded);row.language->setPlaceholderText(src.language+" (source)");sg->addWidget(row.language,sr,3);row.remove=new QCheckBox(embedded);row.remove->setChecked(value.removed);row.remove->setToolTip("Omit this subtitle stream from the authored title.");sg->addWidget(row.remove,sr,4);row.style=new QPushButton(value.style.override_style?"Properties…":"Defaults…",embedded);sg->addWidget(row.style,sr,5);subtitles_.push_back(row);auto* rp=&subtitles_.back();connect(rp->remove,&QCheckBox::toggled,this,[this,rp]{const auto key=default_subtitle_->currentData(Qt::UserRole+1).toString();const bool removed=rp->remove->isChecked();rp->language->setEnabled(!removed);rp->style->setEnabled(!removed);refresh_default_subtitle_choices(std::numeric_limits<int>::min(),key);});connect(rp->style,&QPushButton::clicked,this,[this,rp]{SubtitleStyleDialog d(this,target_,rp->source.codec!="hdmv_pgs_subtitle",style_for_edit(rp->value.style));if(d.exec()!=QDialog::Accepted)return;rp->value.style=d.value();rp->style->setText(rp->value.style.override_style?"Properties…":"Defaults…");});rp->language->setEnabled(!rp->remove->isChecked());rp->style->setEnabled(!rp->remove->isChecked());++sr;}sg->setColumnStretch(3,1);sub_root->addWidget(embedded);
        auto* attached_label=new QLabel("Attached subtitle files",sub_page);attached_label->setStyleSheet("font-weight: bold;");sub_root->addWidget(attached_label);external_table_=new QTableWidget(0,3,sub_page);external_table_->setHorizontalHeaderLabels({"File","Language (ISO 639-2)","Properties"});external_table_->horizontalHeader()->setSectionResizeMode(0,QHeaderView::Stretch);external_table_->horizontalHeader()->setSectionResizeMode(1,QHeaderView::ResizeToContents);external_table_->setSelectionBehavior(QAbstractItemView::SelectRows);sub_root->addWidget(external_table_,1);for(const auto& sub:current_external)add_external_row(QString::fromStdString(sub.source.string()),QString::fromStdString(sub.language),sub.style);refresh_default_subtitle_choices(current_default_subtitle);auto* sub_buttons=new QHBoxLayout;auto* attach=new QPushButton("Attach subtitle file…",sub_page);auto* remove=new QPushButton("Remove selected",sub_page);sub_buttons->addWidget(attach);sub_buttons->addWidget(remove);sub_buttons->addStretch(1);sub_root->addLayout(sub_buttons);connect(attach,&QPushButton::clicked,this,[this]{const auto previous_key=default_subtitle_->currentData(Qt::UserRole+1).toString();const auto files=QFileDialog::getOpenFileNames(this,"Attach subtitle files",{},"Subtitle files (*.srt *.ass *.ssa *.vtt *.sup *.pgs *.idx *.sub);;All files (*)");for(const auto& f:files)if(!f.isEmpty())add_external_row(f,"und",{});refresh_default_subtitle_choices(std::numeric_limits<int>::min(),previous_key);});connect(remove,&QPushButton::clicked,this,[this]{const auto previous_key=default_subtitle_->currentData(Qt::UserRole+1).toString();QSet<int> rows;for(const auto* item:external_table_->selectedItems())rows.insert(item->row());QList<int> ordered=rows.values();std::sort(ordered.begin(),ordered.end(),std::greater<int>());for(int r:ordered)external_table_->removeRow(r);refresh_default_subtitle_choices(std::numeric_limits<int>::min(),previous_key);});tabs->addTab(sub_page,QString("Subtitles (%1 + %2 attached)").arg(subtitle_sources.size()).arg(current_external.size()));

        total_=new QLabel(this);root->addWidget(total_);buttons_=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,this);root->addWidget(buttons_);connect(buttons_,&QDialogButtonBox::rejected,this,&QDialog::reject);connect(buttons_,&QDialogButtonBox::accepted,this,[this]{
            for(auto& row:audio_){row.value.removed=row.remove->isChecked();row.value.language=row.language->text().trimmed().toLower().toStdString();row.value.override_encoding=row.custom->isChecked();row.value.output_channels=row.channels->currentData().toInt();row.value.encoding_worker=worker_from_affinity_combo(row.worker);row.value.encoding_worker_mode=mode_from_affinity_combo(row.worker);if(row.value.removed)continue;if(!valid_language(row.language->text())){QMessageBox::warning(this,"Invalid language","Audio language overrides must be blank or exactly three letters (ISO 639-2).");return;}const auto e=profile_from_row(row);const int maximum=max_channels(e);if(row.value.output_channels>row.source.channels){QMessageBox::warning(this,"Invalid downmix","An audio stream cannot be upmixed to more channels than its source.");return;}if(row.value.output_channels>maximum){QMessageBox::warning(this,"Invalid channel layout","The selected codec/target cannot carry the requested channel count.");return;}if(row.value.override_encoding){copy_audio_encoding_settings(encoding_for_target(row.value,target_),e);try{AuthorEngine::validate_encoding_profile_for_target(encoding_for_target(row.value,target_),target_,"audio stream",allow_exceeding_format_limits_);}catch(const std::exception& ex){QMessageBox::warning(this,"Invalid audio stream settings",QString::fromStdString(ex.what()));return;}}}
            for(auto& row:subtitles_){row.value.removed=row.remove->isChecked();row.value.language=row.language->text().trimmed().toLower().toStdString();if(row.value.removed)continue;if(!valid_language(row.language->text())){QMessageBox::warning(this,"Invalid language","Subtitle language overrides must be blank or exactly three letters (ISO 639-2).");return;}}
            for(int r=0;r<external_table_->rowCount();++r){const auto* path=external_table_->item(r,0);const auto* lang=external_table_->item(r,1);if(!path||path->text().trimmed().isEmpty()){QMessageBox::warning(this,"Invalid subtitle","Attached subtitle path cannot be empty.");return;}if(!lang||!valid_language(lang->text())||lang->text().trimmed().isEmpty()){QMessageBox::warning(this,"Invalid language","Attached subtitle language must be exactly three letters (ISO 639-2).");return;}}
            int fixed_sum=video_bitrate_kbps_;for(const auto& row:audio_)if(!(row.remove&&row.remove->isChecked())&&!row_peak_measured_during_authoring(row))fixed_sum+=row_fixed_bitrate(row);if(!allow_exceeding_format_limits_&&fixed_sum>target_max_combined_av_bitrate_kbps(target_)){QMessageBox::warning(this,"Bitrate limit exceeded",QString("Configured video+fixed-rate audio bitrate %1 kb/s exceeds the target maximum of %2 kb/s. THD peak bitrate, when present, is measured during authoring before video encoding.").arg(fixed_sum).arg(target_max_combined_av_bitrate_kbps(target_)));return;}accept();});refresh_total();
    }
    std::string title_encoding_worker() const{return worker_from_affinity_combo(title_worker_);}
    EncodingWorkerAffinityMode title_encoding_worker_mode() const{return mode_from_affinity_combo(title_worker_);}
    std::vector<AudioStreamSettings> audio_settings() const {std::vector<AudioStreamSettings> out;for(const auto& row:audio_)if(row.value.removed||row.value.override_encoding||row.value.output_channels>0||!row.value.language.empty()||!row.value.encoding_worker.empty())out.push_back(row.value);return out;}
    std::vector<SubtitleStreamSettings> subtitle_settings() const {std::vector<SubtitleStreamSettings> out;for(const auto& row:subtitles_)if(row.value.removed||!row.value.language.empty()||row.value.style.override_style)out.push_back(row.value);return out;}
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
    e.vc1_preset=settings.value(prefix+"/vc1Preset",QString::fromStdString(e.vc1_preset)).toString().toStdString();
    e.two_pass=settings.value(prefix+"/twoPass",e.two_pass).toBool();
    e.fast_first_pass=settings.value(prefix+"/fastFirstPass",e.fast_first_pass).toBool();
    e.keyframe_interval_frames=settings.value(prefix+"/keyframeIntervalFrames",e.keyframe_interval_frames).toInt();
    e.audio_codec=static_cast<AudioCodec>(settings.value(prefix+"/audioCodec",static_cast<int>(e.audio_codec)).toInt());
    e.ac3_bitrate_kbps=settings.value(prefix+"/ac3BitrateKbps",e.ac3_bitrate_kbps).toInt();
    e.dca_bitrate_kbps=settings.value(prefix+"/dcaBitrateKbps",e.dca_bitrate_kbps).toInt();
    e.lpcm_sample_rate_hz=settings.value(prefix+"/lpcmSampleRateHz",e.lpcm_sample_rate_hz).toInt();
    e.lpcm_bit_depth=settings.value(prefix+"/lpcmBitDepth",e.lpcm_bit_depth).toInt();
    e.x264_advanced_options=settings.value(prefix+"/x264AdvancedOptions",QString::fromStdString(e.x264_advanced_options)).toString().toStdString();
    e.x265_advanced_options=settings.value(prefix+"/x265AdvancedOptions",QString::fromStdString(e.x265_advanced_options)).toString().toStdString();
    e.mpeg2_advanced_options=settings.value(prefix+"/mpeg2AdvancedOptions",QString::fromStdString(e.mpeg2_advanced_options)).toString().toStdString();
    e.vc1_advanced_options=settings.value(prefix+"/vc1AdvancedOptions",QString::fromStdString(e.vc1_advanced_options)).toString().toStdString();
    e.ac3_advanced_options=settings.value(prefix+"/ac3AdvancedOptions",QString::fromStdString(e.ac3_advanced_options)).toString().toStdString();
    e.dca_advanced_options=settings.value(prefix+"/dcaAdvancedOptions",QString::fromStdString(e.dca_advanced_options)).toString().toStdString();
    e.lpcm_advanced_options=settings.value(prefix+"/lpcmAdvancedOptions",QString::fromStdString(e.lpcm_advanced_options)).toString().toStdString();
    e.truehd_advanced_options=settings.value(prefix+"/truehdAdvancedOptions",QString::fromStdString(e.truehd_advanced_options)).toString().toStdString();
    e.hdma_advanced_options=settings.value(prefix+"/hdmaAdvancedOptions",QString::fromStdString(e.hdma_advanced_options)).toString().toStdString();
    return e;
}
void save_encoding_defaults(QSettings& settings,const QString& prefix,const EncodingProfile& e) {
    settings.setValue(prefix+"/videoCodec",static_cast<int>(e.video_codec));
    settings.setValue(prefix+"/videoBitrateKbps",e.video_bitrate_kbps);
    settings.setValue(prefix+"/videoMinrateKbps",e.video_min_bitrate_kbps);
    settings.setValue(prefix+"/videoMaxrateKbps",e.video_max_bitrate_kbps);
    settings.setValue(prefix+"/x264Preset",QString::fromStdString(e.x264_preset));
    settings.setValue(prefix+"/x265Preset",QString::fromStdString(e.x265_preset));
    settings.setValue(prefix+"/vc1Preset",QString::fromStdString(e.vc1_preset));
    settings.setValue(prefix+"/twoPass",e.two_pass);settings.setValue(prefix+"/fastFirstPass",e.fast_first_pass);settings.setValue(prefix+"/keyframeIntervalFrames",e.keyframe_interval_frames);
    settings.setValue(prefix+"/audioCodec",static_cast<int>(e.audio_codec));
    settings.setValue(prefix+"/ac3BitrateKbps",e.ac3_bitrate_kbps);
    settings.setValue(prefix+"/dcaBitrateKbps",e.dca_bitrate_kbps);
    settings.setValue(prefix+"/lpcmSampleRateHz",e.lpcm_sample_rate_hz);
    settings.setValue(prefix+"/lpcmBitDepth",e.lpcm_bit_depth);
    settings.setValue(prefix+"/x264AdvancedOptions",QString::fromStdString(e.x264_advanced_options));
    settings.setValue(prefix+"/x265AdvancedOptions",QString::fromStdString(e.x265_advanced_options));
    settings.setValue(prefix+"/mpeg2AdvancedOptions",QString::fromStdString(e.mpeg2_advanced_options));
    settings.setValue(prefix+"/vc1AdvancedOptions",QString::fromStdString(e.vc1_advanced_options));
    settings.setValue(prefix+"/ac3AdvancedOptions",QString::fromStdString(e.ac3_advanced_options));
    settings.setValue(prefix+"/dcaAdvancedOptions",QString::fromStdString(e.dca_advanced_options));
    settings.setValue(prefix+"/lpcmAdvancedOptions",QString::fromStdString(e.lpcm_advanced_options));
    settings.setValue(prefix+"/truehdAdvancedOptions",QString::fromStdString(e.truehd_advanced_options));
    settings.setValue(prefix+"/hdmaAdvancedOptions",QString::fromStdString(e.hdma_advanced_options));
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
    d.disc_avchd_capacity_bytes=settings.value("defaults/project/avchdDiscCapacityBytes",QVariant::fromValue<qulonglong>(d.disc_avchd_capacity_bytes)).toULongLong();
    d.disc_dvd_capacity_bytes=settings.value("defaults/project/dvdDiscCapacityBytes",QVariant::fromValue<qulonglong>(d.disc_dvd_capacity_bytes)).toULongLong();
    if(!disc_size_is_preset(DiscTarget::BluRay1080,d.disc_capacity_bytes))d.disc_capacity_bytes=default_disc_capacity_bytes(DiscTarget::BluRay1080);
    if(!disc_size_is_preset(DiscTarget::UltraHdBluRay2160,d.disc_uhd_capacity_bytes))d.disc_uhd_capacity_bytes=default_disc_capacity_bytes(DiscTarget::UltraHdBluRay2160);
    if(!disc_size_is_preset(DiscTarget::Avchd,d.disc_avchd_capacity_bytes))d.disc_avchd_capacity_bytes=default_disc_capacity_bytes(DiscTarget::Avchd);
    if(!disc_size_is_preset(DiscTarget::DvdVideo480p,d.disc_dvd_capacity_bytes))d.disc_dvd_capacity_bytes=default_disc_capacity_bytes(DiscTarget::DvdVideo480p);
    d.use_encode_cache=settings.value("defaults/project/useEncodeCache",d.use_encode_cache).toBool();
    d.use_compliance_cache=settings.value("defaults/project/useComplianceCache",d.use_compliance_cache).toBool();
    d.force_reencode_new_titles=settings.value("defaults/title/forceReencode",d.force_reencode_new_titles).toBool();
    d.title_encoding=load_encoding_defaults(settings,"defaults/titleEncoding",d.title_encoding);
    d.title_uhd_encoding=load_encoding_defaults(settings,"defaults/titleUhdEncoding",d.title_uhd_encoding);
    d.title_avchd_encoding=load_encoding_defaults(settings,"defaults/titleAvchdEncoding",d.title_avchd_encoding);
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
    QSettings settings;settings.setValue("defaults/project/volumeLabel",QString::fromStdString(d.volume_label));settings.remove("defaults/project/outputImage");settings.setValue("defaults/project/target",static_cast<int>(d.target));settings.setValue("defaults/project/blurayDiscCapacityBytes",QVariant::fromValue<qulonglong>(d.disc_capacity_bytes));settings.setValue("defaults/project/uhdDiscCapacityBytes",QVariant::fromValue<qulonglong>(d.disc_uhd_capacity_bytes));settings.setValue("defaults/project/avchdDiscCapacityBytes",QVariant::fromValue<qulonglong>(d.disc_avchd_capacity_bytes));settings.setValue("defaults/project/dvdDiscCapacityBytes",QVariant::fromValue<qulonglong>(d.disc_dvd_capacity_bytes));settings.setValue("defaults/project/useEncodeCache",d.use_encode_cache);settings.setValue("defaults/project/useComplianceCache",d.use_compliance_cache);settings.setValue("defaults/title/forceReencode",d.force_reencode_new_titles);settings.setValue("defaults/designCanvasVersion",2);
    save_encoding_defaults(settings,"defaults/titleEncoding",d.title_encoding);save_encoding_defaults(settings,"defaults/titleUhdEncoding",d.title_uhd_encoding);save_encoding_defaults(settings,"defaults/titleAvchdEncoding",d.title_avchd_encoding);save_encoding_defaults(settings,"defaults/titleDvdEncoding",d.title_dvd_encoding);save_encoding_defaults(settings,"defaults/menuEncoding",d.menu_encoding);save_encoding_defaults(settings,"defaults/menuUhdEncoding",d.menu_uhd_encoding);save_encoding_defaults(settings,"defaults/menuDvdEncoding",d.menu_dvd_encoding);settings.setValue("defaults/menu/durationSeconds",d.menu_duration_seconds);settings.setValue("defaults/menu/blurayResolution",QString::fromStdString(d.menu_resolution));settings.setValue("defaults/menu/uhdResolution",QString::fromStdString(d.menu_uhd_resolution));settings.setValue("defaults/menu/dvdResolution",QString::fromStdString(d.menu_dvd_resolution));save_button_style_defaults(settings,d.button_style);
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
    }else if(target==DiscTarget::BluRay1080||target==DiscTarget::Avchd){
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
    if(target==DiscTarget::Avchd)return {
        {"4.7 GB (single-layer DVD)",4700000000ULL},
        {"8.5 GB (dual-layer DVD)",8500000000ULL},
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
    QComboBox *bluray_size_default_=nullptr,*uhd_size_default_=nullptr,*avchd_size_default_=nullptr,*dvd_size_default_=nullptr;
    QComboBox *bluray_menu_resolution_default_=nullptr,*uhd_menu_resolution_default_=nullptr,*dvd_menu_resolution_default_=nullptr;
    QCheckBox *cache_=nullptr,*compliance_cache_=nullptr,*force_reencode_titles_=nullptr;
    QComboBox *title_video_=nullptr,*title_preset_=nullptr,*title_audio_=nullptr,*menu_video_=nullptr,*menu_preset_=nullptr,*menu_audio_=nullptr;
    QComboBox *title_uhd_video_=nullptr,*title_uhd_preset_=nullptr,*title_uhd_audio_=nullptr,*menu_uhd_video_=nullptr,*menu_uhd_preset_=nullptr,*menu_uhd_audio_=nullptr;
    QComboBox *title_avchd_video_=nullptr,*title_avchd_preset_=nullptr,*title_avchd_audio_=nullptr;
    QComboBox *title_dvd_video_=nullptr,*title_dvd_preset_=nullptr,*title_dvd_audio_=nullptr,*menu_dvd_video_=nullptr,*menu_dvd_preset_=nullptr,*menu_dvd_audio_=nullptr;
    QSpinBox *title_video_bitrate_=nullptr,*title_video_minrate_=nullptr,*title_video_maxrate_=nullptr,*title_audio_bitrate_=nullptr,*menu_video_bitrate_=nullptr,*menu_video_minrate_=nullptr,*menu_video_maxrate_=nullptr,*menu_audio_bitrate_=nullptr,*button_width_=nullptr,*button_height_=nullptr,*label_size_=nullptr;
    QSpinBox *title_uhd_video_bitrate_=nullptr,*title_uhd_video_minrate_=nullptr,*title_uhd_video_maxrate_=nullptr,*title_uhd_audio_bitrate_=nullptr,*menu_uhd_video_bitrate_=nullptr,*menu_uhd_video_minrate_=nullptr,*menu_uhd_video_maxrate_=nullptr,*menu_uhd_audio_bitrate_=nullptr;
    QSpinBox *title_avchd_video_bitrate_=nullptr,*title_avchd_video_minrate_=nullptr,*title_avchd_video_maxrate_=nullptr,*title_avchd_audio_bitrate_=nullptr;
    QSpinBox *title_dvd_video_bitrate_=nullptr,*title_dvd_video_minrate_=nullptr,*title_dvd_video_maxrate_=nullptr,*title_dvd_audio_bitrate_=nullptr,*menu_dvd_video_bitrate_=nullptr,*menu_dvd_video_minrate_=nullptr,*menu_dvd_video_maxrate_=nullptr,*menu_dvd_audio_bitrate_=nullptr;
    QSpinBox *title_keyframe_=nullptr,*menu_keyframe_=nullptr,*title_uhd_keyframe_=nullptr,*menu_uhd_keyframe_=nullptr,*title_avchd_keyframe_=nullptr,*title_dvd_keyframe_=nullptr,*menu_dvd_keyframe_=nullptr;
    QCheckBox *title_two_pass_=nullptr,*menu_two_pass_=nullptr,*title_uhd_two_pass_=nullptr,*menu_uhd_two_pass_=nullptr,*title_avchd_two_pass_=nullptr,*title_dvd_two_pass_=nullptr,*menu_dvd_two_pass_=nullptr,*label_bold_=nullptr,*label_italic_=nullptr;
    QDoubleSpinBox* menu_duration_=nullptr;
    QFontComboBox* label_font_=nullptr;
    QPushButton *button_style_button_=nullptr,*subtitle_style_button_=nullptr;
    EncodingProfile title_base_,menu_base_,title_uhd_base_,menu_uhd_base_,title_avchd_base_,title_dvd_base_,menu_dvd_base_;
    ButtonStyle button_style_;
    SubtitleStyle subtitle_style_;
    bool restore_defaults_=false;
    void bind_audio(QComboBox* codec,QSpinBox* bitrate,EncodingProfile* base,DiscTarget target) {
        connect(bitrate,QOverload<int>::of(&QSpinBox::valueChanged),this,[codec,base](int v){base->audio_codec=static_cast<AudioCodec>(codec->currentData().toInt());if(audio_codec_has_configurable_bitrate(base->audio_codec))set_audio_bitrate_kbps(*base,v);});
        connect(codec,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[codec,bitrate,base,target](int){base->audio_codec=static_cast<AudioCodec>(codec->currentData().toInt());configure_audio_bitrate_spin(bitrate,target,base->audio_codec,audio_bitrate_kbps(*base));});
    }
public:
    DefaultsDialog(QWidget* parent,const NewProjectDefaults& initial):QDialog(parent),value_(initial),title_base_(initial.title_encoding),menu_base_(initial.menu_encoding),title_uhd_base_(initial.title_uhd_encoding),menu_uhd_base_(initial.menu_uhd_encoding),title_avchd_base_(initial.title_avchd_encoding),title_dvd_base_(initial.title_dvd_encoding),menu_dvd_base_(initial.menu_dvd_encoding),button_style_(initial.button_style),subtitle_style_(initial.subtitle_style) {
        setWindowTitle("Defaults for new projects");resize(700,620);auto* root=new QVBoxLayout(this);auto* tabs=new QTabWidget;root->addWidget(tabs,1);
        auto* project=new QWidget;auto* pf=new QFormLayout(project);volume_=new QLineEdit(QString::fromStdString(initial.volume_label));target_=new QComboBox(project);target_->addItem("Blu-ray 1080p",static_cast<int>(DiscTarget::BluRay1080));target_->addItem("Ultra HD Blu-ray 2160p",static_cast<int>(DiscTarget::UltraHdBluRay2160));target_->addItem("AVCHD (DVD media)",static_cast<int>(DiscTarget::Avchd));target_->addItem("DVD-Video",static_cast<int>(DiscTarget::DvdVideo480p));target_->setCurrentIndex(target_->findData(static_cast<int>(initial.target)));cache_=new QCheckBox("Use encoded-media cache");cache_->setChecked(initial.use_encode_cache);compliance_cache_=new QCheckBox("Use cached compliance analysis");compliance_cache_->setChecked(initial.use_compliance_cache);bluray_size_default_=new QComboBox(project);uhd_size_default_=new QComboBox(project);avchd_size_default_=new QComboBox(project);dvd_size_default_=new QComboBox(project);populate_disc_size_combo(bluray_size_default_,DiscTarget::BluRay1080,initial.disc_capacity_bytes);populate_disc_size_combo(uhd_size_default_,DiscTarget::UltraHdBluRay2160,initial.disc_uhd_capacity_bytes);populate_disc_size_combo(avchd_size_default_,DiscTarget::Avchd,initial.disc_avchd_capacity_bytes);populate_disc_size_combo(dvd_size_default_,DiscTarget::DvdVideo480p,initial.disc_dvd_capacity_bytes);for(auto* combo:{bluray_size_default_,uhd_size_default_,avchd_size_default_,dvd_size_default_}){combo->setMinimumContentsLength(34);combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);}pf->addRow("Default output target",target_);pf->addRow("Default UDF Disc label",volume_);pf->addRow("Default Blu-ray size",bluray_size_default_);pf->addRow("Default UHD size",uhd_size_default_);pf->addRow("Default AVCHD size",avchd_size_default_);pf->addRow("Default DVD size",dvd_size_default_);auto* size_note=new QLabel("AVCHD offers 4.7 GB, 8.5 GB, and Unlimited targets and uses the conservative 18 Mb/s DVD-media transport ceiling. Other capacity choices are size targets only; they do not change codec compliance rules.",project);size_note->setWordWrap(true);pf->addRow(size_note);auto* output_note=new QLabel("Disc image files are chosen separately for each project.",project);output_note->setWordWrap(true);pf->addRow("Disc image",output_note);pf->addRow(cache_);pf->addRow(compliance_cache_);tabs->addTab(project,"Project");
        auto make_encoding_page=[&](DiscTarget target,const EncodingProfile& e,EncodingProfile* base,QComboBox*& video,QSpinBox*& vbit,QSpinBox*& vmin,QSpinBox*& vmax,QComboBox*& preset,QCheckBox*& two,QComboBox*& audio,QSpinBox*& abit,QSpinBox*& keyframe){auto* page=new QWidget;auto* f=new QFormLayout(page);video=video_combo(page,target,e.video_codec);vbit=bitrate_spin(page);configure_video_bitrate_spin(vbit,target,e.video_codec,e.video_bitrate_kbps);vmin=video_rate_limit_spin(page);configure_video_minrate_spin(vmin,target,e.video_codec,e.video_min_bitrate_kbps);vmax=video_rate_limit_spin(page);configure_video_maxrate_spin(vmax,target,e.video_codec,e.video_max_bitrate_kbps);preset=preset_combo(page,e.video_codec,encoding_preset(e));two=new QCheckBox("Enable two-pass encoding",page);configure_pass_control(two,e.video_codec,e.two_pass);
        auto* fast=new QCheckBox("Fast first pass (x264 default; x265 turbo)",page);
        fast->setChecked(e.fast_first_pass);
        fast->setToolTip("Applies only to two-pass x264/x265. Disable to retain full analysis in pass 1.");
        two->setProperty("fastFirstPassControl",QVariant::fromValue<QObject*>(fast));
        fast->setEnabled(two->isChecked()&&(e.video_codec==VideoCodec::X264||e.video_codec==VideoCodec::Hevc));
        connect(two,&QCheckBox::toggled,page,[fast,video](bool checked){const auto c=static_cast<VideoCodec>(video->currentData().toInt());fast->setEnabled(checked&&(c==VideoCodec::X264||c==VideoCodec::Hevc));});
        keyframe=keyframe_interval_spin(page,e.keyframe_interval_frames);configure_keyframe_interval_spin(keyframe,target,e.video_codec,"auto",e.keyframe_interval_frames);audio=audio_combo(page,target,e.audio_codec);abit=audio_bitrate_spin(page);configure_audio_bitrate_spin(abit,target,e.audio_codec,audio_bitrate_kbps(e));auto* advanced=new QPushButton("Advanced…",page);advanced->setToolTip("Set codec-private video and audio options. Disc-spec-critical options remain controlled by BDMV Author.");f->addRow("Video codec",video);f->addRow("Video bitrate",vbit);f->addRow("Video minrate",vmin);f->addRow("Video maxrate",vmax);f->addRow("Encoder speed preset",preset);auto* passesRow=new QWidget(page);auto* passesLayout=new QHBoxLayout(passesRow);
        passesLayout->setContentsMargins(0,0,0,0);passesLayout->addWidget(two);passesLayout->addWidget(fast);
        f->addRow("Passes",passesRow);f->addRow("GOP / keyframe interval",keyframe);f->addRow("Audio codec",audio);f->addRow("Audio bitrate",abit);f->addRow("Advanced codec options",advanced);connect(video,QOverload<int>::of(&QComboBox::currentIndexChanged),page,[video,vbit,vmin,vmax,preset,two,keyframe,target](int){const auto codec=static_cast<VideoCodec>(video->currentData().toInt());configure_video_bitrate_spin(vbit,target,codec,vbit->value());configure_video_minrate_spin(vmin,target,codec,vmin->value());configure_video_maxrate_spin(vmax,target,codec,vmax->value());populate_preset_combo(preset,codec);configure_pass_control(two,codec,two->isChecked());auto* fast=qobject_cast<QCheckBox*>(two->property("fastFirstPassControl").value<QObject*>());if(fast)fast->setEnabled(two->isChecked()&&(codec==VideoCodec::X264||codec==VideoCodec::Hevc));configure_keyframe_interval_spin(keyframe,target,codec,"auto");});connect(advanced,&QPushButton::clicked,page,[page,target,base,video,audio](bool){base->video_codec=static_cast<VideoCodec>(video->currentData().toInt());base->audio_codec=static_cast<AudioCodec>(audio->currentData().toInt());edit_advanced_codec_options(page,target,*base);});return page;};
        auto* title_page=make_encoding_page(DiscTarget::BluRay1080,initial.title_encoding,&title_base_,title_video_,title_video_bitrate_,title_video_minrate_,title_video_maxrate_,title_preset_,title_two_pass_,title_audio_,title_audio_bitrate_,title_keyframe_);auto* title_layout=qobject_cast<QFormLayout*>(title_page->layout());force_reencode_titles_=new QCheckBox("Always re-encode new titles",title_page);force_reencode_titles_->setChecked(initial.force_reencode_new_titles);force_reencode_titles_->setToolTip("When disabled, BDMV Author remuxes already-compliant Blu-ray video/audio streams without re-encoding them.");title_layout->addRow("Passthrough policy",force_reencode_titles_);tabs->addTab(title_page,"New titles — Blu-ray");bind_audio(title_audio_,title_audio_bitrate_,&title_base_,DiscTarget::BluRay1080);
        auto* menu_page=make_encoding_page(DiscTarget::BluRay1080,initial.menu_encoding,&menu_base_,menu_video_,menu_video_bitrate_,menu_video_minrate_,menu_video_maxrate_,menu_preset_,menu_two_pass_,menu_audio_,menu_audio_bitrate_,menu_keyframe_);auto* menu_layout=qobject_cast<QFormLayout*>(menu_page->layout());bluray_menu_resolution_default_=new QComboBox(menu_page);populate_default_menu_resolution_combo(bluray_menu_resolution_default_,DiscTarget::BluRay1080,initial.menu_resolution);bluray_menu_resolution_default_->setToolTip("Default menu raster for newly created standard Blu-ray projects. This setting is independent from the UHD and DVD defaults.");menu_layout->addRow("Default menu resolution",bluray_menu_resolution_default_);menu_duration_=new QDoubleSpinBox(menu_page);menu_duration_->setRange(10.0,36000.0);menu_duration_->setDecimals(1);menu_duration_->setSuffix(" s");menu_duration_->setValue(initial.menu_duration_seconds);menu_layout->addRow("Menu duration",menu_duration_);tabs->addTab(menu_page,"Menus — Blu-ray");bind_audio(menu_audio_,menu_audio_bitrate_,&menu_base_,DiscTarget::BluRay1080);
        auto* title_uhd_page=make_encoding_page(DiscTarget::UltraHdBluRay2160,initial.title_uhd_encoding,&title_uhd_base_,title_uhd_video_,title_uhd_video_bitrate_,title_uhd_video_minrate_,title_uhd_video_maxrate_,title_uhd_preset_,title_uhd_two_pass_,title_uhd_audio_,title_uhd_audio_bitrate_,title_uhd_keyframe_);tabs->addTab(title_uhd_page,"New titles — UHD");bind_audio(title_uhd_audio_,title_uhd_audio_bitrate_,&title_uhd_base_,DiscTarget::UltraHdBluRay2160);
        auto* menu_uhd_page=make_encoding_page(DiscTarget::UltraHdBluRay2160,initial.menu_uhd_encoding,&menu_uhd_base_,menu_uhd_video_,menu_uhd_video_bitrate_,menu_uhd_video_minrate_,menu_uhd_video_maxrate_,menu_uhd_preset_,menu_uhd_two_pass_,menu_uhd_audio_,menu_uhd_audio_bitrate_,menu_uhd_keyframe_);auto* menu_uhd_layout=qobject_cast<QFormLayout*>(menu_uhd_page->layout());uhd_menu_resolution_default_=new QComboBox(menu_uhd_page);populate_default_menu_resolution_combo(uhd_menu_resolution_default_,DiscTarget::UltraHdBluRay2160,initial.menu_uhd_resolution);uhd_menu_resolution_default_->setToolTip("1920×1080 is the default workaround for VLC's UHD Blu-ray mouse-coordinate bug. Hardware players may use 3840×2160 menus correctly.");menu_uhd_layout->addRow("Default menu resolution",uhd_menu_resolution_default_);connect(uhd_menu_resolution_default_,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this](int){if(uhd_menu_resolution_default_->currentData().toString()!=QStringLiteral("1920x1080"))warn_uhd_4k_menu_vlc_mouse_bug(this);});tabs->addTab(menu_uhd_page,"Menus — UHD");bind_audio(menu_uhd_audio_,menu_uhd_audio_bitrate_,&menu_uhd_base_,DiscTarget::UltraHdBluRay2160);
        auto* title_avchd_page=make_encoding_page(DiscTarget::Avchd,initial.title_avchd_encoding,&title_avchd_base_,title_avchd_video_,title_avchd_video_bitrate_,title_avchd_video_minrate_,title_avchd_video_maxrate_,title_avchd_preset_,title_avchd_two_pass_,title_avchd_audio_,title_avchd_audio_bitrate_,title_avchd_keyframe_);auto* avchd_note=new QLabel("AVCHD export is intentionally limited to one menuless title. Video is AVC/H.264; audio may be Dolby Digital AC-3 or Linear PCM. Optical-disc output uses the 18 Mb/s system-rate ceiling.",title_avchd_page);avchd_note->setWordWrap(true);qobject_cast<QFormLayout*>(title_avchd_page->layout())->addRow(avchd_note);tabs->addTab(title_avchd_page,"New titles — AVCHD");bind_audio(title_avchd_audio_,title_avchd_audio_bitrate_,&title_avchd_base_,DiscTarget::Avchd);
        auto* title_dvd_page=make_encoding_page(DiscTarget::DvdVideo480p,initial.title_dvd_encoding,&title_dvd_base_,title_dvd_video_,title_dvd_video_bitrate_,title_dvd_video_minrate_,title_dvd_video_maxrate_,title_dvd_preset_,title_dvd_two_pass_,title_dvd_audio_,title_dvd_audio_bitrate_,title_dvd_keyframe_);tabs->addTab(title_dvd_page,"New titles — DVD");bind_audio(title_dvd_audio_,title_dvd_audio_bitrate_,&title_dvd_base_,DiscTarget::DvdVideo480p);
        auto* menu_dvd_page=make_encoding_page(DiscTarget::DvdVideo480p,initial.menu_dvd_encoding,&menu_dvd_base_,menu_dvd_video_,menu_dvd_video_bitrate_,menu_dvd_video_minrate_,menu_dvd_video_maxrate_,menu_dvd_preset_,menu_dvd_two_pass_,menu_dvd_audio_,menu_dvd_audio_bitrate_,menu_dvd_keyframe_);auto* menu_dvd_layout=qobject_cast<QFormLayout*>(menu_dvd_page->layout());dvd_menu_resolution_default_=new QComboBox(menu_dvd_page);populate_default_menu_resolution_combo(dvd_menu_resolution_default_,DiscTarget::DvdVideo480p,initial.menu_dvd_resolution);dvd_menu_resolution_default_->setToolTip("Default menu raster for newly created DVD-Video projects. PAL and NTSC families must still obey the project's frame-rate family constraints.");menu_dvd_layout->addRow("Default menu resolution",dvd_menu_resolution_default_);tabs->addTab(menu_dvd_page,"Menus — DVD");bind_audio(menu_dvd_audio_,menu_dvd_audio_bitrate_,&menu_dvd_base_,DiscTarget::DvdVideo480p);
        auto* subtitles_page=new QWidget;auto* subtitles_layout=new QVBoxLayout(subtitles_page);auto* subtitle_note=new QLabel("These defaults are copied into each new title. Individual subtitle streams can override them later. The editor separates Common, Blu-ray/UHD, and DVD properties; leaving the override disabled keeps backend rendering defaults.",subtitles_page);subtitle_note->setWordWrap(true);subtitles_layout->addWidget(subtitle_note);subtitle_style_button_=new QPushButton("Edit default subtitle properties…",subtitles_page);subtitles_layout->addWidget(subtitle_style_button_);subtitles_layout->addStretch(1);tabs->addTab(subtitles_page,"Subtitles");connect(subtitle_style_button_,&QPushButton::clicked,this,[this]{SubtitleStyleDialog d(this,DiscTarget::BluRay1080,true,subtitle_style_,true);if(d.exec()==QDialog::Accepted)subtitle_style_=d.value();});
        auto* appearance=new QWidget;auto* af=new QFormLayout(appearance);button_style_button_=new QPushButton("Edit default text-button style…",appearance);button_width_=new QSpinBox(appearance);button_width_->setRange(160,3600);button_width_->setSuffix(" px");button_width_->setValue(initial.button_width);button_height_=new QSpinBox(appearance);button_height_->setRange(48,600);button_height_->setSuffix(" px");button_height_->setValue(initial.button_height);label_font_=new QFontComboBox(appearance);label_font_->setCurrentFont(QFont(QString::fromStdString(initial.label_font_family)));label_size_=new QSpinBox(appearance);label_size_->setRange(12,1024);label_size_->setSuffix(" px");label_size_->setValue(initial.label_font_size_px);label_bold_=new QCheckBox(appearance);label_bold_->setChecked(initial.label_bold);label_italic_=new QCheckBox(appearance);label_italic_->setChecked(initial.label_italic);af->addRow("Button appearance",button_style_button_);af->addRow("New button width",button_width_);af->addRow("New button height",button_height_);af->addRow("Text-label font",label_font_);af->addRow("Text-label size",label_size_);af->addRow("Text labels bold",label_bold_);af->addRow("Text labels italic",label_italic_);tabs->addTab(appearance,"Buttons & text");
        connect(button_style_button_,&QPushButton::clicked,this,[this]{ButtonStyleDialog d(this,button_style_);if(d.exec()==QDialog::Accepted)button_style_=d.value();});
        auto* buttons=new QDialogButtonBox(QDialogButtonBox::RestoreDefaults|QDialogButtonBox::Ok|QDialogButtonBox::Cancel);root->addWidget(buttons);connect(buttons,&QDialogButtonBox::accepted,this,&QDialog::accept);connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject);connect(buttons->button(QDialogButtonBox::RestoreDefaults),&QPushButton::clicked,this,[this]{restore_defaults_=true;accept();});
    }
    NewProjectDefaults value() const {
        if(restore_defaults_)return NewProjectDefaults{};
        NewProjectDefaults d=value_;d.volume_label=volume_->text().toStdString();d.target=static_cast<DiscTarget>(target_->currentData().toInt());d.disc_capacity_bytes=static_cast<std::uint64_t>(bluray_size_default_->currentData().toULongLong());d.disc_uhd_capacity_bytes=static_cast<std::uint64_t>(uhd_size_default_->currentData().toULongLong());d.disc_avchd_capacity_bytes=static_cast<std::uint64_t>(avchd_size_default_->currentData().toULongLong());d.disc_dvd_capacity_bytes=static_cast<std::uint64_t>(dvd_size_default_->currentData().toULongLong());d.use_encode_cache=cache_->isChecked();d.use_compliance_cache=compliance_cache_->isChecked();d.force_reencode_new_titles=force_reencode_titles_->isChecked();
        d.title_encoding=profile_from_widgets(title_video_,title_video_bitrate_,title_preset_,title_two_pass_,title_audio_,title_audio_bitrate_,&title_base_,title_keyframe_,title_video_minrate_,title_video_maxrate_);d.title_uhd_encoding=profile_from_widgets(title_uhd_video_,title_uhd_video_bitrate_,title_uhd_preset_,title_uhd_two_pass_,title_uhd_audio_,title_uhd_audio_bitrate_,&title_uhd_base_,title_uhd_keyframe_,title_uhd_video_minrate_,title_uhd_video_maxrate_);d.title_avchd_encoding=profile_from_widgets(title_avchd_video_,title_avchd_video_bitrate_,title_avchd_preset_,title_avchd_two_pass_,title_avchd_audio_,title_avchd_audio_bitrate_,&title_avchd_base_,title_avchd_keyframe_,title_avchd_video_minrate_,title_avchd_video_maxrate_);d.title_dvd_encoding=profile_from_widgets(title_dvd_video_,title_dvd_video_bitrate_,title_dvd_preset_,title_dvd_two_pass_,title_dvd_audio_,title_dvd_audio_bitrate_,&title_dvd_base_,title_dvd_keyframe_,title_dvd_video_minrate_,title_dvd_video_maxrate_);d.menu_encoding=profile_from_widgets(menu_video_,menu_video_bitrate_,menu_preset_,menu_two_pass_,menu_audio_,menu_audio_bitrate_,&menu_base_,menu_keyframe_,menu_video_minrate_,menu_video_maxrate_);d.menu_uhd_encoding=profile_from_widgets(menu_uhd_video_,menu_uhd_video_bitrate_,menu_uhd_preset_,menu_uhd_two_pass_,menu_uhd_audio_,menu_uhd_audio_bitrate_,&menu_uhd_base_,menu_uhd_keyframe_,menu_uhd_video_minrate_,menu_uhd_video_maxrate_);d.menu_dvd_encoding=profile_from_widgets(menu_dvd_video_,menu_dvd_video_bitrate_,menu_dvd_preset_,menu_dvd_two_pass_,menu_dvd_audio_,menu_dvd_audio_bitrate_,&menu_dvd_base_,menu_dvd_keyframe_,menu_dvd_video_minrate_,menu_dvd_video_maxrate_);d.menu_duration_seconds=menu_duration_->value();d.menu_resolution=bluray_menu_resolution_default_->currentData().toString().toStdString();d.menu_uhd_resolution=uhd_menu_resolution_default_->currentData().toString().toStdString();d.menu_dvd_resolution=dvd_menu_resolution_default_->currentData().toString().toStdString();d.button_style=button_style_;d.button_width=button_width_->value();d.button_height=button_height_->value();d.label_font_family=label_font_->currentFont().family().toStdString();d.label_font_size_px=label_size_->value();d.label_bold=label_bold_->isChecked();d.label_italic=label_italic_->isChecked();d.subtitle_style=subtitle_style_;return d;
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

std::vector<std::string> authored_audio_stream_keys(const std::vector<GuiAudioTrack>& tracks,
                                                    const std::vector<AudioStreamSettings>& settings){
    std::vector<std::string> out;
    auto removed=[&](int ordinal){const auto it=std::find_if(settings.begin(),settings.end(),[&](const AudioStreamSettings& x){return x.source_ordinal==ordinal;});return it!=settings.end()&&it->removed;};
    for(const auto& track:tracks)if(!removed(track.ordinal))out.push_back("audio:"+std::to_string(track.ordinal));
    return out;
}
std::vector<std::string> authored_subtitle_stream_keys(const std::vector<GuiSubtitleTrack>& tracks,
                                                       const std::vector<SubtitleStreamSettings>& settings,
                                                       const std::vector<ExternalSubtitle>& external){
    std::vector<std::string> out;
    auto removed=[&](int ordinal){const auto it=std::find_if(settings.begin(),settings.end(),[&](const SubtitleStreamSettings& x){return x.source_ordinal==ordinal;});return it!=settings.end()&&it->removed;};
    for(const auto& track:tracks)if(!removed(track.ordinal))out.push_back("subtitle:"+std::to_string(track.ordinal));
    std::unordered_map<std::string,int> occurrences;
    for(const auto& sub:external){
        const std::string base="external:"+sub.source.lexically_normal().generic_string()+"\n"+sub.language;
        const int occurrence=occurrences[base]++;
        out.push_back(base+"#"+std::to_string(occurrence));
    }
    return out;
}
void cleanup_empty_repeat_groups(std::vector<NavigationAction>& actions){
    for(std::size_t i=0;i+1U<actions.size();){
        if(actions[i].kind==NavigationActionKind::RepeatBegin&&actions[i+1U].kind==NavigationActionKind::RepeatEnd){
            actions.erase(actions.begin()+static_cast<std::ptrdiff_t>(i),actions.begin()+static_cast<std::ptrdiff_t>(i+2U));
            if(i>0U)--i;
        }else ++i;
    }
}
void remap_stream_actions(std::vector<NavigationAction>& actions,std::uint16_t title,
                          const std::vector<std::string>& old_audio,const std::vector<std::string>& new_audio,
                          const std::vector<std::string>& old_subtitles,const std::vector<std::string>& new_subtitles){
    actions.erase(std::remove_if(actions.begin(),actions.end(),[&](NavigationAction& action){
        if(action.target_title!=title)return false;
        const std::vector<std::string>* old_order=nullptr;
        const std::vector<std::string>* new_order=nullptr;
        if(action.kind==NavigationActionKind::AudioTrack){old_order=&old_audio;new_order=&new_audio;}
        else if(action.kind==NavigationActionKind::SubtitleTrack){old_order=&old_subtitles;new_order=&new_subtitles;}
        else return false;
        if(action.target_stream==0U||action.target_stream>old_order->size())return false;
        const auto& key=(*old_order)[static_cast<std::size_t>(action.target_stream-1U)];
        const auto it=std::find(new_order->begin(),new_order->end(),key);
        if(it==new_order->end())return true;
        action.target_stream=static_cast<std::uint16_t>(std::distance(new_order->begin(),it)+1);
        return false;
    }),actions.end());
    cleanup_empty_repeat_groups(actions);
}
void remap_stream_actions(Menu& menu,std::uint16_t title,
                          const std::vector<std::string>& old_audio,const std::vector<std::string>& new_audio,
                          const std::vector<std::string>& old_subtitles,const std::vector<std::string>& new_subtitles){
    menu.buttons.erase(std::remove_if(menu.buttons.begin(),menu.buttons.end(),[&](MenuButton& button){
        auto actions=button_action_sequence(button);
        remap_stream_actions(actions,title,old_audio,new_audio,old_subtitles,new_subtitles);
        if(actions.empty())return true;
        set_button_action_sequence(button,std::move(actions));
        return false;
    }),menu.buttons.end());
    if(!menu.back_button.actions.empty()){
        auto actions=button_action_sequence(menu.back_button);
        remap_stream_actions(actions,title,old_audio,new_audio,old_subtitles,new_subtitles);
        if(!actions.empty())set_button_action_sequence(menu.back_button,std::move(actions));
    }
    for(auto& child:menu.submenus)remap_stream_actions(child,title,old_audio,new_audio,old_subtitles,new_subtitles);
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

class EncodeProgressDialog final : public QDialog {
    QTreeWidget* tree_=nullptr;
    QPlainTextEdit* diagnostics_=nullptr;
    QMap<QString,QTreeWidgetItem*> groups_;
    QMap<QString,QTreeWidgetItem*> rows_;
    QTreeWidgetItem* group(const QString& worker){
        if(auto it=groups_.find(worker);it!=groups_.end())return it.value();
        auto* item=new QTreeWidgetItem(tree_);item->setText(0,worker);item->setFirstColumnSpanned(true);
        QFont f=item->font(0);f.setBold(true);item->setFont(0,f);item->setExpanded(true);groups_.insert(worker,item);return item;
    }
public:
    explicit EncodeProgressDialog(QWidget* parent=nullptr):QDialog(parent){
        setWindowTitle("Encoding progress");setModal(false);resize(1050,520);
        auto* root=new QVBoxLayout(this);auto* note=new QLabel("Each active encode is shown under the worker performing it. Transient and permanent failures retain their exact cause below, including complete FFmpeg stderr. Closing this window does not stop encoding; use Encoding details… in the main window to reopen it.",this);note->setWordWrap(true);root->addWidget(note);
        tree_=new QTreeWidget(this);tree_->setColumnCount(9);tree_->setHeaderLabels({"Encode","Codec","Pass","Progress","FPS","Speed","Status","Attempt","Cause"});tree_->header()->setSectionResizeMode(0,QHeaderView::Stretch);tree_->header()->setSectionResizeMode(8,QHeaderView::Stretch);for(int c=1;c<8;++c)tree_->header()->setSectionResizeMode(c,QHeaderView::ResizeToContents);root->addWidget(tree_,1);
        auto* diagnostic_label=new QLabel("Error history / complete tool output",this);root->addWidget(diagnostic_label);
        diagnostics_=new QPlainTextEdit(this);diagnostics_->setReadOnly(true);diagnostics_->setLineWrapMode(QPlainTextEdit::NoWrap);diagnostics_->setMinimumHeight(180);root->addWidget(diagnostics_,1);
        auto* close=new QPushButton("Close",this);connect(close,&QPushButton::clicked,this,&QDialog::hide);auto* row=new QHBoxLayout;row->addStretch(1);row->addWidget(close);root->addLayout(row);
    }
    void reset(){tree_->clear();diagnostics_->clear();groups_.clear();rows_.clear();setWindowTitle("Encoding progress");}
    void update_encode(const EncodeJobProgress& p){
        const QString worker=QString::fromStdString(p.worker.empty()?std::string("Unassigned"):p.worker);
        const QString job=QString::fromStdString(p.job);const QString key=worker+QChar(0x1f)+job;
        QTreeWidgetItem* row=rows_.value(key,nullptr);if(!row){row=new QTreeWidgetItem(group(worker));rows_.insert(key,row);row->setText(0,job);}
        row->setText(1,QString::fromStdString(p.codec));
        row->setText(2,p.pass>0?QStringLiteral("%1/%2").arg(p.pass).arg(std::max(1,p.passes)):QStringLiteral("—"));
        row->setText(3,QString::number(std::clamp(p.fraction,0.0,1.0)*100.0,'f',1)+"%");
        row->setText(4,p.fps>0.0?QString::number(p.fps,'f',1):QStringLiteral("—"));
        row->setText(5,p.speed>0.0?QString::number(p.speed,'f',2)+"x":QStringLiteral("—"));
        row->setText(6,QString::fromStdString(p.status));
        row->setText(7,p.maximum_attempts>0?QStringLiteral("%1/%2").arg(p.attempt).arg(p.maximum_attempts):QStringLiteral("%1/∞").arg(p.attempt));
        if(!p.diagnostic.empty()){
            const QString full=QString::fromStdString(p.diagnostic);
            QString cause=full.section('\n',0,0).trimmed();
            if(cause.size()>180)cause=cause.left(177)+QStringLiteral("...");
            row->setText(8,QStringLiteral("%1: %2").arg(QString::fromStdString(p.failure_type),cause));
            diagnostics_->appendPlainText(QStringLiteral("[%1] %2 — %3 — attempt %4\n%5\n")
                .arg(QString::fromStdString(p.failure_type)).arg(worker).arg(job).arg(p.attempt).arg(full));
            diagnostics_->verticalScrollBar()->setValue(diagnostics_->verticalScrollBar()->maximum());
        }
        if(auto* parent=row->parent())parent->setExpanded(true);
    }
    void finish(const QString& suffix){if(!suffix.isEmpty())setWindowTitle(QStringLiteral("Encoding progress — %1").arg(suffix));}
};

struct BuildResult {
    QStringList filter_warnings;
    bool filter_warning_shown_during_build = false;
    QString error;
    bool cancelled = false;
};

class Window : public QMainWindow {
    static constexpr std::size_t BackIndex = std::numeric_limits<std::size_t>::max();
    QTableWidget* titles_=nullptr;
    QTreeWidget* menu_tree_=nullptr;
    QListWidget* button_list_=nullptr;
    QListWidget* overlay_list_=nullptr;
    QComboBox* layout_scope_=nullptr; QSpinBox* layout_gap_=nullptr;
    QGraphicsScene* scene_=nullptr;QGraphicsView* view_=nullptr;MenuCanvasHost* canvas_host_=nullptr;
    QDialog *layout_dialog_=nullptr,*objects_dialog_=nullptr,*menu_settings_dialog_=nullptr;
    QLineEdit* volume_=nullptr;QLineEdit* output_=nullptr;QLineEdit* menu_name_=nullptr;QLineEdit* bg_=nullptr;QComboBox* bg_type_=nullptr;QLineEdit* menu_audio_path_=nullptr;
    ColorButton* bg_color_=nullptr;
    QCheckBox *inherit_bg_=nullptr,*inherit_bg_color_=nullptr,*inherit_style_=nullptr,*inherit_audio_=nullptr,*inherit_duration_=nullptr,*inherit_encoding_=nullptr,*loop_media_=nullptr,*auto_back_=nullptr;
    QPushButton *default_style_button_=nullptr,*edit_back_button_=nullptr,*add_menu_button_=nullptr,*remove_menu_button_=nullptr;
    QDoubleSpinBox* duration_=nullptr;
    QComboBox* menu_video_=nullptr;QComboBox* menu_worker_=nullptr;QSpinBox* menu_bitrate_=nullptr;QSpinBox* menu_minrate_=nullptr;QSpinBox* menu_maxrate_=nullptr;QComboBox* menu_preset_=nullptr;QCheckBox* menu_two_pass_=nullptr;QCheckBox* menu_fast_first_=nullptr;QSpinBox* menu_keyframe_=nullptr;QComboBox* menu_audio_codec_=nullptr;QSpinBox* menu_audio_bitrate_=nullptr;QPushButton* menu_advanced_=nullptr;
    QProgressBar* progress_=nullptr;QPushButton* build_=nullptr;QPushButton* cancel_=nullptr;QPushButton* encoding_details_=nullptr;QComboBox* size_target_=nullptr;QCheckBox* automatic_bitrate_=nullptr;QComboBox* target_=nullptr;QComboBox* frame_rate_=nullptr;QComboBox* menu_resolution_=nullptr;QComboBox* menu_aspect_=nullptr;
    bool cache_enabled_state_=true;
    bool compliance_cache_enabled_state_=true;
    bool refresh_encode_cache_state_=false;
    bool refresh_compliance_cache_state_=false;
    std::shared_ptr<std::atomic_bool> cancel_requested_;
    bool close_after_render_=false;
    std::unique_ptr<QFutureWatcher<BuildResult>> watcher_;
    std::unique_ptr<EncodeProgressDialog> encode_progress_dialog_;
    std::vector<NavigationAction> first_play_actions_;
    Menu root_menu_;
    std::string current_menu_id_="top";
    int next_menu_id_=1;
    bool loading_menu_=false;
    bool debug_mode_=false;
    AuthoringLimits authoring_limits_{};
    bool project_modified_=false;
    bool suppress_modified_=true;
    struct HistoryEntry { Project project; std::string menu_id; };
    std::vector<HistoryEntry> history_;
    int history_index_=-1, saved_history_index_=-1;
    bool restoring_history_=false;
    QTimer history_timer_;
    QAction *undo_action_=nullptr,*redo_action_=nullptr;
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
    static constexpr int RoleTitleEncodingWorker = Qt::UserRole + 46;
    static constexpr int RoleTitleEncodingWorkerMode = Qt::UserRole + 47;
    static constexpr int RoleAvchdProfile = Qt::UserRole + 48;
    static constexpr int RoleAvchdFrameRate = Qt::UserRole + 49;
    static constexpr int RoleAvchdResolution = Qt::UserRole + 50;
    static constexpr int RoleAvchdAspect = Qt::UserRole + 51;
    static int profile_role(DiscTarget target){switch(target){case DiscTarget::UltraHdBluRay2160:return RoleUhdProfile;case DiscTarget::Avchd:return RoleAvchdProfile;case DiscTarget::DvdVideo480p:return RoleDvdProfile;case DiscTarget::BluRay1080:return RoleHdProfile;}return RoleHdProfile;}
    static int frame_rate_role(DiscTarget target){switch(target){case DiscTarget::UltraHdBluRay2160:return RoleUhdFrameRate;case DiscTarget::Avchd:return RoleAvchdFrameRate;case DiscTarget::DvdVideo480p:return RoleDvdFrameRate;case DiscTarget::BluRay1080:return RoleHdFrameRate;}return RoleHdFrameRate;}
    static int resolution_role(DiscTarget target){switch(target){case DiscTarget::UltraHdBluRay2160:return RoleUhdResolution;case DiscTarget::Avchd:return RoleAvchdResolution;case DiscTarget::DvdVideo480p:return RoleDvdResolution;case DiscTarget::BluRay1080:return RoleHdResolution;}return RoleHdResolution;}
    static int aspect_role(DiscTarget target){switch(target){case DiscTarget::UltraHdBluRay2160:return RoleUhdAspect;case DiscTarget::Avchd:return RoleAvchdAspect;case DiscTarget::DvdVideo480p:return RoleDvdAspect;case DiscTarget::BluRay1080:return RoleHdAspect;}return RoleHdAspect;}
    static EncodingProfile default_profile_for_target(DiscTarget target){switch(target){case DiscTarget::UltraHdBluRay2160:return default_uhd_encoding_profile();case DiscTarget::Avchd:return default_avchd_encoding_profile();case DiscTarget::DvdVideo480p:return default_dvd_encoding_profile();case DiscTarget::BluRay1080:return EncodingProfile{};}return EncodingProfile{};}
    static QString output_target_name(DiscTarget target){switch(target){case DiscTarget::DvdVideo480p:return QStringLiteral("DVD");case DiscTarget::Avchd:return QStringLiteral("AVCHD");case DiscTarget::UltraHdBluRay2160:return QStringLiteral("Ultra HD Blu-ray");case DiscTarget::BluRay1080:return QStringLiteral("Blu-ray");}return QStringLiteral("Blu-ray");}
    std::string& stored_menu_resolution(DiscTarget target){switch(target){case DiscTarget::UltraHdBluRay2160:return menu_uhd_resolution_;case DiscTarget::Avchd:return menu_hd_resolution_;case DiscTarget::DvdVideo480p:return menu_dvd_resolution_;case DiscTarget::BluRay1080:return menu_hd_resolution_;}return menu_hd_resolution_;}
    const std::string& stored_menu_resolution(DiscTarget target)const{return const_cast<Window*>(this)->stored_menu_resolution(target);}
    std::string& stored_menu_aspect(DiscTarget target){switch(target){case DiscTarget::UltraHdBluRay2160:return menu_uhd_aspect_;case DiscTarget::Avchd:return menu_hd_aspect_;case DiscTarget::DvdVideo480p:return menu_dvd_aspect_;case DiscTarget::BluRay1080:return menu_hd_aspect_;}return menu_hd_aspect_;}
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
    void update_history_actions(){
        if(undo_action_)undo_action_->setEnabled(history_index_>0);
        if(redo_action_)redo_action_->setEnabled(history_index_>=0&&history_index_+1<static_cast<int>(history_.size()));
    }
    void reset_history(){
        history_timer_.stop();history_.clear();history_index_=0;saved_history_index_=0;
        history_.push_back({snapshot_project(),current_menu_id_});update_history_actions();
    }
    void commit_history(){
        if(restoring_history_||history_index_<0)return;
        history_timer_.stop();
        // New edits replace the redo branch. Keep a bounded history to avoid
        // retaining arbitrarily many full title/menu project snapshots.
        history_.resize(static_cast<std::size_t>(history_index_+1));
        if(saved_history_index_>history_index_)saved_history_index_=-1;
        history_.push_back({snapshot_project(),current_menu_id_});++history_index_;
        if(history_.size()>80){history_.erase(history_.begin());--history_index_;if(saved_history_index_>=0)--saved_history_index_;}
        update_history_actions();
    }
    void restore_history(int index){
        if(index<0||index>=static_cast<int>(history_.size())||render_active())return;
        history_timer_.stop();restoring_history_=true;
        const auto entry=history_[static_cast<std::size_t>(index)];
        populate_project(entry.project);
        current_menu_id_=entry.menu_id;
        if(!current_menu())current_menu_id_=root_menu_.id;
        load_menu_controls();refresh_all();
        history_index_=index;project_modified_=history_index_!=saved_history_index_;
        update_window_title();update_history_actions();restoring_history_=false;
        statusBar()->showMessage("Project edit restored.",2500);
    }
    void undo_edit(){if(render_active())return;if(history_timer_.isActive())commit_history();restore_history(history_index_-1);}
    void redo_edit(){if(render_active())return;if(history_timer_.isActive())commit_history();restore_history(history_index_+1);}
    void mark_modified(){
        if(suppress_modified_||restoring_history_)return;
        project_modified_=true;update_window_title();
        if(history_index_>=0)history_timer_.start(350); // One entry for a drag or a short typing burst.
    }
    void mark_clean(){
        if(history_timer_.isActive())commit_history();
        if(history_index_>=0)saved_history_index_=history_index_;
        project_modified_=false;update_window_title();
    }
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
        auto* avchd=new QSpinBox(&d);avchd->setRange(1000,1000000);avchd->setSuffix(" kb/s");avchd->setSingleStep(1000);avchd->setValue(authoring_limits_.avchd_transport_bitrate_kbps);
        auto* dvd=new QSpinBox(&d);dvd->setRange(1000,1000000);dvd->setSuffix(" kb/s");dvd->setSingleStep(1000);dvd->setValue(authoring_limits_.dvd_transport_bitrate_kbps);
        form->addRow("Blu-ray combined transport limit",bd);form->addRow("UHD Blu-ray combined transport limit",uhd);form->addRow("AVCHD combined transport limit",avchd);form->addRow("DVD program-stream mux limit",dvd);layout->addLayout(form);
        auto* buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&d);connect(buttons,&QDialogButtonBox::accepted,&d,&QDialog::accept);connect(buttons,&QDialogButtonBox::rejected,&d,&QDialog::reject);layout->addWidget(buttons);
        if(d.exec()!=QDialog::Accepted)return;
        authoring_limits_.bluray_transport_bitrate_kbps=bd->value();
        authoring_limits_.uhd_transport_bitrate_kbps=uhd->value();
        authoring_limits_.avchd_transport_bitrate_kbps=avchd->value();
        authoring_limits_.dvd_transport_bitrate_kbps=dvd->value();
        statusBar()->showMessage(QString("Debug transport limits for this session: Blu-ray %1 kb/s, UHD %2 kb/s, AVCHD %3 kb/s, DVD %4 kb/s%5").arg(bd->value()).arg(uhd->value()).arg(avchd->value()).arg(dvd->value()).arg(authoring_limits_.allow_exceeding_format_limits?QStringLiteral(" (over-limit values enabled)"):QStringLiteral(" (standard ceilings still enforced)")),7000);
    }
public:
    explicit Window(bool debug_mode=false):debug_mode_(debug_mode){
        Project initial_project;apply_new_project_defaults(initial_project,project_defaults_);root_menu_=initial_project.menu;
        if(target_is_avchd(initial_project.target)){root_menu_.id.clear();root_menu_.name.clear();}
        else{root_menu_.id="top";root_menu_.name="Top Menu";root_menu_.inherit_background_image=false;root_menu_.inherit_background_color=false;root_menu_.inherit_button_style=false;root_menu_.inherit_encoding=false;root_menu_.inherit_audio=false;root_menu_.inherit_duration=false;}
        resize(1780,980);
        auto* fileMenu=menuBar()->addMenu("&File");
        auto* newProject=fileMenu->addAction("&New project");
        auto* openProject=fileMenu->addAction("&Open project…");
        auto* saveProject=fileMenu->addAction("&Save project");
        auto* saveProjectAs=fileMenu->addAction("Save project &as…");
        fileMenu->addSeparator();auto* quitAction=fileMenu->addAction("&Quit");
        auto* editMenu=menuBar()->addMenu("&Edit");
        undo_action_=editMenu->addAction("&Undo");redo_action_=editMenu->addAction("&Redo");
        undo_action_->setShortcut(QKeySequence::Undo);redo_action_->setShortcut(QKeySequence::Redo);
        undo_action_->setEnabled(false);redo_action_->setEnabled(false);
        connect(undo_action_,&QAction::triggered,this,[this]{undo_edit();});
        connect(redo_action_,&QAction::triggered,this,[this]{redo_edit();});
        history_timer_.setSingleShot(true);
        connect(&history_timer_,&QTimer::timeout,this,[this]{commit_history();});
        auto* settingsMenu=menuBar()->addMenu("&Settings");auto* defaultsAction=settingsMenu->addAction("Defaults for &new projects…");auto* toolsAction=settingsMenu->addAction("External &tools and encoders…");auto* workersAction=settingsMenu->addAction("Encoding &workers and servers…");auto* storageAction=settingsMenu->addAction("Storage &locations…");auto* cacheManagerAction=settingsMenu->addAction("&Cache settings…");
        QAction* debugLimitsAction=nullptr;QAction* debugAllowExceedAction=nullptr;
        if(debug_mode_){auto* debugMenu=menuBar()->addMenu("&Debug");debugAllowExceedAction=debugMenu->addAction("Allow exceeding format limits");debugAllowExceedAction->setCheckable(true);debugAllowExceedAction->setChecked(false);debugAllowExceedAction->setToolTip("Session-only: allow audio/video bitrate and mux/transport settings above disc-format ceilings. This can create noncompliant or unplayable output.");debugMenu->addSeparator();debugLimitsAction=debugMenu->addAction("Transport bitrate &limits…");}
        auto* helpMenu=menuBar()->addMenu("&Help");auto* helpTopics=helpMenu->addMenu("&Help Topics");
        auto* gettingStarted=helpTopics->addAction("&Getting Started");auto* titleHelp=helpTopics->addAction("&Titles and Video Modes");auto* chapterHelp=helpTopics->addAction("&Chapters and Navigation");auto* menuHelp=helpTopics->addAction("&Menus and Buttons");auto* buildHelp=helpTopics->addAction("&Building Disc Images");
        helpMenu->addSeparator();auto* aboutAction=helpMenu->addAction("&About BDMV Author");
        newProject->setShortcut(QKeySequence::New);saveProject->setShortcut(QKeySequence::Save);openProject->setShortcut(QKeySequence::Open);quitAction->setShortcut(QKeySequence::Quit);
        connect(newProject,&QAction::triggered,this,[this]{new_project();});
        connect(defaultsAction,&QAction::triggered,this,[this]{edit_new_project_defaults();});
        connect(toolsAction,&QAction::triggered,this,[this]{edit_tool_settings();});connect(workersAction,&QAction::triggered,this,[this]{edit_encoding_workers();});connect(storageAction,&QAction::triggered,this,[this]{edit_storage_settings();});connect(cacheManagerAction,&QAction::triggered,this,[this]{edit_cache_settings();});
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
            "The 3840×2160 design canvas is shared by all targets. The Buttons and labels, Align and space, and Menu settings buttons open separate windows so the canvas can stay large. Use Edit → Undo (Ctrl+Z) or Redo (Ctrl+Shift+Z) to restore project edits. Menus may contain title/chapter buttons, audio/subtitle controls, links to submenus, text labels, and images. Menu video resolution and aspect are selected separately from title modes; 4:3 is available where the disc standard permits it. UHD menus default to 1920×1080 as a compatibility workaround for a VLC UHD Blu-ray mouse-coordinate bug; 3840×2160 remains available for other players, and BDMV Author warns when it is selected. New-project default menu resolutions and disc-size targets for Blu-ray, UHD, and DVD are independently configurable in Settings → Defaults for new projects.\n\nMenu duration is automatic whenever motion video or menu audio is present. With both, the longer source determines the duration and the shorter source loops. A separate Menu audio file overrides audio embedded in the background video; otherwise the video's first audio stream is used. Duration remains manually adjustable for still-image menus with no audio. Loop menu audio/video repeats the media continuously; turn it off to play the media once and leave the menu on its final frame.\n\nThe main menu itself may be removed. A menu-less disc defaults to playing every title in order once and then stopping, while Startup sequence… remains fully editable. Add video normally creates a menu button automatically; clear its checkbox when a title should not receive one.");});
        connect(buildHelp,&QAction::triggered,this,[this]{show_help_topic("Building Disc Images",
            "The Build button creates an image for the selected target. Blu-ray and Ultra HD Blu-ray use the BDMV authoring path and UDF image writer; DVD-Video uses dvdauthor/spumux and mkisofs DVD-Video output. Size target selects the intended image/disc capacity; Automatic video bitrate is enabled by default and chooses the highest video bitrate that fits after audio, subtitles, muxing/filesystem overhead, and a safety margin. Unlimited removes the capacity limit but not disc-format bitrate limits. Manual bitrate mode is rejected before encoding if the configured project cannot fit. Encoding progress appears in the progress bar, and Cancel stops active helper processes and removes temporary render files. Cache controls and the cache manager are under Settings → Cache settings…. Save the .bdmvproject separately if you want to edit the authoring project later.");});
        connect(aboutAction,&QAction::triggered,this,[this]{show_about();});
        auto* c=new QWidget;setCentralWidget(c);auto* root=new QVBoxLayout(c);
        auto* project=new QHBoxLayout;volume_=new QLineEdit(QString::fromStdString(project_defaults_.volume_label));output_=new QLineEdit;output_->setPlaceholderText("Choose a disc image file…");target_=new QComboBox;target_->addItem("Blu-ray",static_cast<int>(DiscTarget::BluRay1080));target_->addItem("Ultra HD Blu-ray",static_cast<int>(DiscTarget::UltraHdBluRay2160));target_->addItem("AVCHD",static_cast<int>(DiscTarget::Avchd));target_->addItem("DVD-Video",static_cast<int>(DiscTarget::DvdVideo480p));load_menu_mode_state(initial_project);displayed_target_=initial_project.target;target_->setCurrentIndex(target_->findData(static_cast<int>(displayed_target_)));frame_rate_=new QComboBox;populate_frame_rate_choices(initial_project.frame_rate);menu_resolution_=new QComboBox;populate_resolution_choices(menu_resolution_,displayed_target_,stored_menu_resolution(displayed_target_),true);menu_aspect_=new QComboBox;populate_aspect_choices(menu_aspect_,displayed_target_,stored_menu_aspect(displayed_target_),false,stored_menu_resolution(displayed_target_));auto* outBrowse=new QPushButton("Browse…");auto* startupSequence=new QPushButton("Startup sequence…");project->addWidget(new QLabel("Output target"));project->addWidget(target_);project->addWidget(new QLabel("Menu resolution"));project->addWidget(menu_resolution_);project->addWidget(new QLabel("Menu aspect"));project->addWidget(menu_aspect_);project->addWidget(new QLabel("Menu/default frame rate"));project->addWidget(frame_rate_);project->addWidget(startupSequence);project->addSpacing(12);project->addWidget(new QLabel("UDF Disc label"));project->addWidget(volume_);project->addSpacing(12);project->addWidget(new QLabel("Disc image"));project->addWidget(output_,1);project->addWidget(outBrowse);root->addLayout(project);
        connect(outBrowse,&QPushButton::clicked,this,[this]{choose_output_image();});
        connect(menu_resolution_,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this](int){const auto previous_aspect=stored_menu_aspect(displayed_target_);stored_menu_resolution(displayed_target_)=menu_resolution_->currentData().toString().toStdString();if(displayed_target_==DiscTarget::UltraHdBluRay2160&&stored_menu_resolution(displayed_target_)=="3840x2160")warn_uhd_4k_menu_vlc_mouse_bug(this);const auto keep=menu_aspect_->currentData().toString().toStdString();populate_aspect_choices(menu_aspect_,displayed_target_,keep,false,stored_menu_resolution(displayed_target_));stored_menu_aspect(displayed_target_)=menu_aspect_->currentData().toString().toStdString();apply_editor_aspect_transition(previous_aspect,stored_menu_aspect(displayed_target_));});
        connect(menu_aspect_,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this](int){const auto previous=stored_menu_aspect(displayed_target_);stored_menu_aspect(displayed_target_)=menu_aspect_->currentData().toString().toStdString();apply_editor_aspect_transition(previous,stored_menu_aspect(displayed_target_));});

        auto* split=new QSplitter;root->addWidget(split,1);
        auto* left=new QWidget;auto* lv=new QVBoxLayout(left);
        titles_=new QTableWidget(0,20);titles_->setSelectionBehavior(QAbstractItemView::SelectRows);titles_->setSelectionMode(QAbstractItemView::ExtendedSelection);titles_->setHorizontalHeaderLabels({"Title","Source","Video","Bitrate","Encoder preset","2-pass","Default audio","Default audio bitrate","Chapters","Frame rate","Force re-encode","Resolution","Aspect","GOP / keyframe","Advanced","Navigation / UOP","Streams","Minrate","Maxrate","Fast pass 1"});titles_->horizontalHeader()->setSectionResizeMode(0,QHeaderView::ResizeToContents);titles_->horizontalHeader()->setSectionResizeMode(1,QHeaderView::Stretch);for(int col=2;col<20;++col)titles_->horizontalHeader()->setSectionResizeMode(col,QHeaderView::ResizeToContents);titles_->horizontalHeader()->moveSection(19,6); // Logical column 19 (fast first pass) is visually beside logical column 5 (2-pass).
        lv->addWidget(new QLabel("Titles — a title may be linked from any number of menu pages"));
        titles_->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
#ifdef Q_OS_MACOS
        // macOS may render QAbstractScrollArea scroll bars as transient overlays, which can make
        // the wide per-title settings table appear to have no horizontal navigation at all.
        // Keep the table's internal scroll state, but expose a dedicated, always-present bar
        // immediately below it so every title setting remains reachable.
        titles_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        lv->addWidget(titles_,2);
        auto* titleSettingsScroll=new QScrollBar(Qt::Horizontal,left);
        titleSettingsScroll->setObjectName(QStringLiteral("titleSettingsHorizontalScrollBar"));
        titleSettingsScroll->setToolTip(QStringLiteral("Scroll horizontally to reach all per-title video and audio settings."));
        titleSettingsScroll->setMinimumHeight(16);
        auto* persistentTitleScrollStyle=new PersistentScrollBarStyle;
        persistentTitleScrollStyle->setParent(titleSettingsScroll);
        titleSettingsScroll->setStyle(persistentTitleScrollStyle);
        lv->addWidget(titleSettingsScroll);
        auto* tableHorizontalScroll=titles_->horizontalScrollBar();
        const auto syncTitleScrollRange=[tableHorizontalScroll,titleSettingsScroll](int minimum,int maximum){
            titleSettingsScroll->setRange(minimum,maximum);
            titleSettingsScroll->setPageStep(tableHorizontalScroll->pageStep());
            titleSettingsScroll->setSingleStep(tableHorizontalScroll->singleStep());
            titleSettingsScroll->setEnabled(maximum>minimum);
        };
        connect(tableHorizontalScroll,&QScrollBar::rangeChanged,titleSettingsScroll,syncTitleScrollRange);
        connect(tableHorizontalScroll,&QScrollBar::valueChanged,titleSettingsScroll,&QScrollBar::setValue);
        connect(titleSettingsScroll,&QScrollBar::valueChanged,tableHorizontalScroll,&QScrollBar::setValue);
        syncTitleScrollRange(tableHorizontalScroll->minimum(),tableHorizontalScroll->maximum());
#else
        titles_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        lv->addWidget(titles_,2);
#endif
        auto* titleBtns=new QHBoxLayout;auto* addTitle=new QPushButton("Add video…");auto* removeTitle=new QPushButton("Remove title");titleBtns->addWidget(addTitle);titleBtns->addWidget(removeTitle);lv->addLayout(titleBtns);
        lv->addWidget(new QLabel("Menus"));menu_tree_=new QTreeWidget;menu_tree_->setHeaderHidden(true);lv->addWidget(menu_tree_,1);
        auto* menuBtns=new QHBoxLayout;add_menu_button_=new QPushButton("Add menu");remove_menu_button_=new QPushButton("Remove menu");menuBtns->addWidget(add_menu_button_);menuBtns->addWidget(remove_menu_button_);lv->addLayout(menuBtns);split->addWidget(left);

        auto* right=new QWidget;auto* rv=new QVBoxLayout(right);
        scene_=new QGraphicsScene(0,0,kProjectDesignWidth,kProjectDesignHeight);canvas_host_=new MenuCanvasHost(scene_,right);view_=canvas_host_->view();rv->addWidget(new QLabel("Current menu — 3840×2160 design space; 4:3 menus use the centered 2880×2160 aperture"));rv->addWidget(canvas_host_,1);
        auto* editorTools=new QHBoxLayout;
        auto* objectsButton=new QPushButton("Buttons and labels…",right);
        auto* layoutButton=new QPushButton("Align and space…",right);
        auto* settingsButton=new QPushButton("Menu settings…",right);
        editorTools->addWidget(objectsButton);editorTools->addWidget(layoutButton);
        editorTools->addWidget(settingsButton);editorTools->addStretch(1);
        rv->addLayout(editorTools);
        auto* layoutGroup=new QGroupBox("Align and space menu objects",right);
        auto* layoutRows=new QVBoxLayout(layoutGroup);
        auto* layoutTop=new QHBoxLayout;
        layout_scope_=new QComboBox(layoutGroup);
        layout_scope_->addItems({"Selected on canvas (Ctrl-click)","All buttons","All labels","All buttons and labels"});
        layout_scope_->setToolTip("Select multiple buttons/labels with Ctrl-click on the canvas, or apply to every item of a type.");
        layout_gap_=new QSpinBox(layoutGroup);layout_gap_->setRange(0,kProjectDesignWidth);layout_gap_->setValue(48);layout_gap_->setSuffix(" px");
        layout_gap_->setToolTip("Exact edge-to-edge distance in 3840×2160 project design pixels.");
        layoutTop->addWidget(new QLabel("Objects:"));layoutTop->addWidget(layout_scope_,1);
        layoutTop->addWidget(new QLabel("Gap:"));layoutTop->addWidget(layout_gap_);layoutRows->addLayout(layoutTop);
        auto* layoutActions=new QHBoxLayout;
        for(const auto& spec : std::initializer_list<std::pair<const char*,MenuAlign>>{
             {"Left",MenuAlign::Left},{"Center X",MenuAlign::HCenter},{"Right",MenuAlign::Right},
             {"Top",MenuAlign::Top},{"Center Y",MenuAlign::VCenter},{"Bottom",MenuAlign::Bottom}}){
            auto* action=new QPushButton(spec.first,layoutGroup);layoutActions->addWidget(action);
            connect(action,&QPushButton::clicked,this,[this,alignment=spec.second]{apply_menu_alignment(alignment);});
        }
        for(const auto& spec : std::initializer_list<std::pair<const char*,MenuAxis>>{
            {"Space ↔",MenuAxis::Horizontal},{"Space ↕",MenuAxis::Vertical}}){
            auto* action=new QPushButton(spec.first,layoutGroup);layoutActions->addWidget(action);
            connect(action,&QPushButton::clicked,this,[this,axis=spec.second]{apply_menu_spacing(axis);});
        }
        layoutRows->addLayout(layoutActions);layout_dialog_=new QDialog(this);
        layout_dialog_->setWindowTitle("Align and space menu objects");layout_dialog_->resize(990,180);
        auto* layoutDialogBody=new QVBoxLayout(layout_dialog_);
        layoutDialogBody->addWidget(layoutGroup);
        auto* layoutClose=new QDialogButtonBox(QDialogButtonBox::Close,layout_dialog_);
        layoutDialogBody->addWidget(layoutClose);
        connect(layoutClose,&QDialogButtonBox::rejected,layout_dialog_,&QDialog::hide);
        connect(layoutButton,&QPushButton::clicked,layout_dialog_,[this]{layout_dialog_->show();layout_dialog_->raise();layout_dialog_->activateWindow();});
        objects_dialog_=new QDialog(this);objects_dialog_->setWindowTitle("Menu buttons and labels");objects_dialog_->resize(840,640);
        auto* objectsDialogBody=new QVBoxLayout(objects_dialog_);
        menu_settings_dialog_=new QDialog(this);menu_settings_dialog_->setWindowTitle("Menu page and encoding settings");menu_settings_dialog_->resize(750,760);
        auto* settingsDialogBody=new QVBoxLayout(menu_settings_dialog_);
        connect(objectsButton,&QPushButton::clicked,objects_dialog_,[this]{objects_dialog_->show();objects_dialog_->raise();objects_dialog_->activateWindow();});
        connect(settingsButton,&QPushButton::clicked,menu_settings_dialog_,[this]{menu_settings_dialog_->show();menu_settings_dialog_->raise();menu_settings_dialog_->activateWindow();});
        auto* buttonPanel=new QWidget;auto* bp=new QVBoxLayout(buttonPanel);bp->addWidget(new QLabel("Buttons on this menu"));button_list_=new QListWidget;bp->addWidget(button_list_,1);auto* b1=new QHBoxLayout;auto* addTitleButton=new QPushButton("Add title/chapter button…");auto* addStreamButton=new QPushButton("Add audio/subtitle button…");auto* addMenuButton=new QPushButton("Add menu link…");b1->addWidget(addTitleButton);b1->addWidget(addStreamButton);b1->addWidget(addMenuButton);bp->addLayout(b1);auto* b2=new QHBoxLayout;auto* editButton=new QPushButton("Edit selected…");auto* delButton=new QPushButton("Remove selected");b2->addWidget(editButton);b2->addWidget(delButton);bp->addLayout(b2);
        bp->addSpacing(8);bp->addWidget(new QLabel("Labels burned into menu video"));overlay_list_=new QListWidget;bp->addWidget(overlay_list_,1);auto* l1=new QHBoxLayout;auto* addTextLabel=new QPushButton("Add text…");auto* addImageLabel=new QPushButton("Add image…");l1->addWidget(addTextLabel);l1->addWidget(addImageLabel);bp->addLayout(l1);auto* l2=new QHBoxLayout;auto* editLabel=new QPushButton("Edit label…");auto* delLabel=new QPushButton("Remove label");l2->addWidget(editLabel);l2->addWidget(delLabel);bp->addLayout(l2);objectsDialogBody->addWidget(buttonPanel,1);
        auto* objectsClose=new QDialogButtonBox(QDialogButtonBox::Close,objects_dialog_);
        objectsDialogBody->addWidget(objectsClose);
        connect(objectsClose,&QDialogButtonBox::rejected,objects_dialog_,&QDialog::hide);

        auto* settingsScroll=new QScrollArea;settingsScroll->setWidgetResizable(true);auto* settings=new QWidget;settingsScroll->setWidget(settings);auto* sv=new QVBoxLayout(settings);
        auto* menuGroup=new QGroupBox("Menu page settings");auto* mf=new QFormLayout(menuGroup);menu_name_=new QLineEdit;mf->addRow("Name",menu_name_);
        inherit_bg_=new QCheckBox("Inherit from parent");bg_type_=new QComboBox;bg_type_->addItems({"Still image","Video"});bg_=new QLineEdit;auto* bgrow=new QWidget;auto* bgl=new QHBoxLayout(bgrow);bgl->setContentsMargins(0,0,0,0);bgl->addWidget(inherit_bg_);bgl->addWidget(bg_type_);bgl->addWidget(bg_,1);auto* bgBrowse=new QPushButton("Browse…");bgl->addWidget(bgBrowse);mf->addRow("Background",bgrow);
        inherit_bg_color_=new QCheckBox("Inherit from parent");bg_color_=new ColorButton({0,0,0,255});auto* colorrow=new QWidget;auto* colorl=new QHBoxLayout(colorrow);colorl->setContentsMargins(0,0,0,0);colorl->addWidget(inherit_bg_color_);colorl->addWidget(bg_color_,1);mf->addRow("Background color",colorrow);
        inherit_style_=new QCheckBox("Inherit from parent");default_style_button_=new QPushButton("Edit default text-button style…");auto* stylerow=new QWidget;auto* stylel=new QHBoxLayout(stylerow);stylel->setContentsMargins(0,0,0,0);stylel->addWidget(inherit_style_);stylel->addWidget(default_style_button_,1);mf->addRow("Button style",stylerow);
        inherit_audio_=new QCheckBox("Inherit from parent");menu_audio_path_=new QLineEdit;auto* ar=new QWidget;auto* al=new QHBoxLayout(ar);al->setContentsMargins(0,0,0,0);al->addWidget(inherit_audio_);al->addWidget(menu_audio_path_,1);auto* audioBrowse=new QPushButton("Browse…");al->addWidget(audioBrowse);mf->addRow("Menu audio",ar);
        inherit_duration_=new QCheckBox("Inherit from parent");duration_=new QDoubleSpinBox;duration_->setRange(0.001,86400.0);duration_->setDecimals(3);duration_->setSuffix(" s");auto* dr=new QWidget;auto* dl=new QHBoxLayout(dr);dl->setContentsMargins(0,0,0,0);dl->addWidget(inherit_duration_);dl->addWidget(duration_,1);mf->addRow("Duration",dr);loop_media_=new QCheckBox("Loop menu audio/video");loop_media_->setChecked(true);loop_media_->setToolTip("When enabled, menu media repeats continuously. When disabled, media plays once and the menu remains on its final frame.");mf->addRow("Media playback",loop_media_);
        auto_back_=new QCheckBox("Automatically add Back button");edit_back_button_=new QPushButton("Edit Back button…");auto* br=new QWidget;auto* bl=new QHBoxLayout(br);bl->setContentsMargins(0,0,0,0);bl->addWidget(auto_back_);bl->addWidget(edit_back_button_);mf->addRow("Submenu back",br);sv->addWidget(menuGroup);
        auto* encGroup=new QGroupBox("Menu encoding");auto* encForm=new QFormLayout(encGroup);inherit_encoding_=new QCheckBox("Inherit encoding from parent");encForm->addRow(inherit_encoding_);{const auto& initial_menu_encoding=encoding_for_target(root_menu_,displayed_target_);menu_video_=video_combo(encGroup,displayed_target_,initial_menu_encoding.video_codec);menu_bitrate_=bitrate_spin(encGroup);configure_video_bitrate_spin(menu_bitrate_,displayed_target_,initial_menu_encoding.video_codec,initial_menu_encoding.video_bitrate_kbps,authoring_limits_.allow_exceeding_format_limits);menu_minrate_=video_rate_limit_spin(encGroup);configure_video_minrate_spin(menu_minrate_,displayed_target_,initial_menu_encoding.video_codec,initial_menu_encoding.video_min_bitrate_kbps,authoring_limits_.allow_exceeding_format_limits);menu_maxrate_=video_rate_limit_spin(encGroup);configure_video_maxrate_spin(menu_maxrate_,displayed_target_,initial_menu_encoding.video_codec,initial_menu_encoding.video_max_bitrate_kbps,authoring_limits_.allow_exceeding_format_limits);menu_preset_=preset_combo(encGroup,initial_menu_encoding.video_codec,encoding_preset(initial_menu_encoding));menu_two_pass_=new QCheckBox("Enable two-pass encoding",encGroup);configure_pass_control(menu_two_pass_,initial_menu_encoding.video_codec,initial_menu_encoding.two_pass);
        menu_fast_first_=new QCheckBox("Fast first pass (x264 default; x265 turbo)",encGroup);
        menu_fast_first_->setChecked(initial_menu_encoding.fast_first_pass);
        menu_fast_first_->setToolTip("Only applies to two-pass x264/x265. Disable to use full pass-1 analysis.");
        menu_two_pass_->setProperty("fastFirstPassControl",QVariant::fromValue<QObject*>(menu_fast_first_));menu_keyframe_=keyframe_interval_spin(encGroup,initial_menu_encoding.keyframe_interval_frames);configure_keyframe_interval_spin(menu_keyframe_,displayed_target_,initial_menu_encoding.video_codec,frame_rate_->currentData().toString().toStdString(),initial_menu_encoding.keyframe_interval_frames);menu_audio_codec_=audio_combo(encGroup,displayed_target_,initial_menu_encoding.audio_codec);menu_audio_bitrate_=audio_bitrate_spin(encGroup);configure_audio_bitrate_spin(menu_audio_bitrate_,displayed_target_,initial_menu_encoding.audio_codec,audio_bitrate_kbps(initial_menu_encoding),authoring_limits_.allow_exceeding_format_limits);menu_advanced_=new QPushButton("Advanced…",encGroup);menu_worker_=new QComboBox(encGroup);populate_worker_affinity_combo(menu_worker_,root_menu_.encoding_worker,root_menu_.encoding_worker_mode,"Automatic scheduling");menu_worker_->setToolTip("Preferred tries this worker first and can fall through to another compatible worker. Required pins menu video/audio to this worker.");}encForm->addRow("Encoding worker",menu_worker_);encForm->addRow("Video codec",menu_video_);encForm->addRow("Video bitrate",menu_bitrate_);encForm->addRow("Video minrate",menu_minrate_);encForm->addRow("Video maxrate",menu_maxrate_);encForm->addRow("Encoder speed preset",menu_preset_);auto* passesRow=new QWidget(encGroup);
        auto* passesLayout=new QHBoxLayout(passesRow);passesLayout->setContentsMargins(0,0,0,0);
        passesLayout->addWidget(menu_two_pass_);passesLayout->addWidget(menu_fast_first_);passesLayout->addStretch(1);
        encForm->addRow("Passes",passesRow);encForm->addRow("GOP / keyframe interval",menu_keyframe_);encForm->addRow("Audio codec",menu_audio_codec_);encForm->addRow("Audio bitrate",menu_audio_bitrate_);encForm->addRow("Advanced codec options",menu_advanced_);sv->addWidget(encGroup);sv->addStretch(1);settingsDialogBody->addWidget(settingsScroll,1);
        auto* settingsClose=new QDialogButtonBox(QDialogButtonBox::Close,menu_settings_dialog_);
        settingsDialogBody->addWidget(settingsClose);
        connect(settingsClose,&QDialogButtonBox::rejected,menu_settings_dialog_,&QDialog::hide);
        split->addWidget(right);split->setStretchFactor(0,1);split->setStretchFactor(1,4);
        split->setSizes({300,1250});

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
        auto* bottom=new QHBoxLayout;progress_=new QProgressBar;progress_->setRange(0,10000);progress_->setFormat("0.00%");encoding_details_=new QPushButton("Encoding details…");encoding_details_->setEnabled(false);build_=new QPushButton;cancel_=new QPushButton("Cancel");cancel_->setEnabled(false);update_build_button_text();bottom->addWidget(progress_,1);bottom->addWidget(encoding_details_);bottom->addWidget(cancel_);bottom->addWidget(build_);root->addLayout(bottom);statusBar()->showMessage("Native nested HDMV menus; no Java/BD-J is generated");
        connect(encoding_details_,&QPushButton::clicked,this,[this]{if(!encode_progress_dialog_)return;encode_progress_dialog_->show();encode_progress_dialog_->raise();encode_progress_dialog_->activateWindow();});

        connect(addTitle,&QPushButton::clicked,this,[this]{add_videos();});
        connect(removeTitle,&QPushButton::clicked,this,[this]{remove_selected_titles();});
        connect(add_menu_button_,&QPushButton::clicked,this,[this]{add_submenu();});connect(remove_menu_button_,&QPushButton::clicked,this,[this]{remove_current_menu();});
        connect(addTitleButton,&QPushButton::clicked,this,[this]{add_title_button();});connect(addStreamButton,&QPushButton::clicked,this,[this]{add_stream_button();});connect(addMenuButton,&QPushButton::clicked,this,[this]{add_menu_link();});connect(editButton,&QPushButton::clicked,this,[this]{edit_selected_button();});connect(delButton,&QPushButton::clicked,this,[this]{remove_selected_button();});
        connect(addTextLabel,&QPushButton::clicked,this,[this]{add_text_label();});connect(addImageLabel,&QPushButton::clicked,this,[this]{add_image_label();});connect(editLabel,&QPushButton::clicked,this,[this]{edit_selected_label();});connect(delLabel,&QPushButton::clicked,this,[this]{remove_selected_label();});
        connect(button_list_,&QListWidget::itemDoubleClicked,this,[this](QListWidgetItem*){edit_selected_button();});connect(overlay_list_,&QListWidget::itemDoubleClicked,this,[this](QListWidgetItem*){edit_selected_label();});
        connect(menu_tree_,&QTreeWidget::currentItemChanged,this,[this](QTreeWidgetItem* item,QTreeWidgetItem*){if(!item)return;current_menu_id_=item->data(0,Qt::UserRole).toString().toStdString();load_menu_controls();refresh_canvas();refresh_button_list();refresh_overlay_list();});
        connect(bgBrowse,&QPushButton::clicked,this,[this]{const bool video=bg_type_->currentIndex()==1;const auto filter=video?QString("Video files (*.mp4 *.mkv *.mov *.m2ts *.mts *.ts *.mpeg *.mpg *.avi *.webm);;All files (*)"):QString("Image files (*.png *.jpg *.jpeg *.webp *.bmp *.tif *.tiff);;All files (*)");const auto f=QFileDialog::getOpenFileName(this,video?"Menu background video":"Menu background image",bg_->text(),filter);if(!f.isEmpty()){bg_->setText(f);mark_modified();commit_history();load_menu_controls();}});
        connect(audioBrowse,&QPushButton::clicked,this,[this]{const auto f=QFileDialog::getOpenFileName(this,"Menu audio",menu_audio_path_->text());if(!f.isEmpty()){menu_audio_path_->setText(f);mark_modified();commit_history();load_menu_controls();}});
        connect(default_style_button_,&QPushButton::clicked,this,[this]{auto* m=current_menu();if(!m)return;ButtonStyleDialog d(this,m->default_button_style);if(d.exec()==QDialog::Accepted){m->default_button_style=d.value();mark_modified();commit_history();refresh_canvas();}});
        connect(edit_back_button_,&QPushButton::clicked,this,[this]{edit_back_button();});
        connect(menu_name_,&QLineEdit::editingFinished,this,[this]{if(loading_menu_)return;if(auto*m=current_menu()){m->name=menu_name_->text().toStdString();mark_modified();if(auto* parent=parent_of(root_menu_,m->id))for(auto& b:parent->buttons)if(b.auto_submenu_link&&b.target_menu_id==m->id){b.label=m->name;for(auto& a:b.actions)if(a.kind==NavigationActionKind::Menu&&a.target_menu_id==m->id)a.target_menu_id=m->id;}rebuild_menu_tree();refresh_button_list();}});
        connect(bg_,&QLineEdit::textChanged,this,[this](const QString& v){if(loading_menu_)return;if(auto*m=current_menu()){if(bg_type_->currentIndex()==1){m->background_video=v.toStdString();m->background_image.clear();}else{m->background_image=v.toStdString();m->background_video.clear();}mark_modified();}update_enabled();refresh_canvas();});
        connect(bg_type_,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this](int index){if(loading_menu_)return;if(auto*m=current_menu()){const auto path=bg_->text().toStdString();if(index==1){m->background_video=path;m->background_image.clear();}else{m->background_image=path;m->background_video.clear();}mark_modified();}load_menu_controls();refresh_canvas();});
        connect(menu_audio_path_,&QLineEdit::textChanged,this,[this](const QString& v){if(loading_menu_)return;if(auto*m=current_menu()){m->audio_source=v.toStdString();mark_modified();}update_enabled();});
        connect(bg_,&QLineEdit::editingFinished,this,[this]{if(!loading_menu_)load_menu_controls();});
        connect(menu_audio_path_,&QLineEdit::editingFinished,this,[this]{if(!loading_menu_)load_menu_controls();});
        connect(bg_color_,&QPushButton::clicked,this,[this]{if(loading_menu_)return;if(auto*m=current_menu()){const auto chosen=bg_color_->value();if(!(m->background_color==chosen)){m->background_color=chosen;mark_modified();commit_history();refresh_canvas();}}});
        connect(duration_,QOverload<double>::of(&QDoubleSpinBox::valueChanged),this,[this](double v){if(loading_menu_)return;if(auto*m=current_menu()){m->duration_seconds=v;mark_modified();}});connect(loop_media_,&QCheckBox::toggled,this,[this](bool v){if(loading_menu_)return;if(auto*m=current_menu()){m->loop_media=v;mark_modified();}});
        connect(auto_back_,&QCheckBox::toggled,this,[this](bool v){if(loading_menu_)return;if(auto*m=current_menu()){m->auto_back_button=v;mark_modified();edit_back_button_->setEnabled(v);refresh_canvas();}});
        auto encoding_changed=[this]{if(loading_menu_)return;if(auto*m=current_menu()){auto& active=encoding_for_target(*m,displayed_target_);const auto base=active;active=profile_from_widgets(menu_video_,menu_bitrate_,menu_preset_,menu_two_pass_,menu_audio_codec_,menu_audio_bitrate_,&base,menu_keyframe_,menu_minrate_,menu_maxrate_);mark_modified();}};
        connect(menu_worker_,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this](int){if(loading_menu_)return;if(auto*m=current_menu()){m->encoding_worker=worker_from_affinity_combo(menu_worker_);m->encoding_worker_mode=mode_from_affinity_combo(menu_worker_);mark_modified();commit_history();}});
        connect(menu_video_,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this,encoding_changed](int){const auto codec=static_cast<VideoCodec>(menu_video_->currentData().toInt());configure_video_bitrate_spin(menu_bitrate_,displayed_target_,codec,menu_bitrate_->value(),authoring_limits_.allow_exceeding_format_limits);configure_video_minrate_spin(menu_minrate_,displayed_target_,codec,menu_minrate_->value(),authoring_limits_.allow_exceeding_format_limits);configure_video_maxrate_spin(menu_maxrate_,displayed_target_,codec,menu_maxrate_->value(),authoring_limits_.allow_exceeding_format_limits);populate_preset_combo(menu_preset_,codec);configure_pass_control(menu_two_pass_,codec,menu_two_pass_->isChecked());update_fast_menu_pass_enabled();configure_keyframe_interval_spin(menu_keyframe_,displayed_target_,codec,frame_rate_->currentData().toString().toStdString());encoding_changed();});connect(menu_bitrate_,QOverload<int>::of(&QSpinBox::valueChanged),this,[encoding_changed](int){encoding_changed();});connect(menu_minrate_,QOverload<int>::of(&QSpinBox::valueChanged),this,[encoding_changed](int){encoding_changed();});connect(menu_maxrate_,QOverload<int>::of(&QSpinBox::valueChanged),this,[encoding_changed](int){encoding_changed();});connect(menu_preset_,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[encoding_changed](int){encoding_changed();});connect(menu_two_pass_,&QCheckBox::toggled,this,[this,encoding_changed](bool){update_fast_menu_pass_enabled();encoding_changed();});connect(menu_fast_first_,&QCheckBox::toggled,this,[encoding_changed](bool){encoding_changed();});connect(menu_keyframe_,QOverload<int>::of(&QSpinBox::valueChanged),this,[encoding_changed](int){encoding_changed();});connect(menu_audio_bitrate_,QOverload<int>::of(&QSpinBox::valueChanged),this,[encoding_changed](int){encoding_changed();});connect(menu_advanced_,&QPushButton::clicked,this,[this,encoding_changed]{if(auto*m=current_menu()){auto& active=encoding_for_target(*m,displayed_target_);const auto base=active;active=profile_from_widgets(menu_video_,menu_bitrate_,menu_preset_,menu_two_pass_,menu_audio_codec_,menu_audio_bitrate_,&base,menu_keyframe_,menu_minrate_,menu_maxrate_);if(edit_advanced_codec_options(this,displayed_target_,active))encoding_changed();else active=base;}});
        connect(menu_audio_codec_,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this,encoding_changed](int){if(loading_menu_)return;if(auto*m=current_menu()){const auto codec=static_cast<AudioCodec>(menu_audio_codec_->currentData().toInt());const auto& active=encoding_for_target(*m,displayed_target_);const int bitrate=codec==AudioCodec::Dca?active.dca_bitrate_kbps:active.ac3_bitrate_kbps;configure_audio_bitrate_spin(menu_audio_bitrate_,displayed_target_,codec,bitrate,authoring_limits_.allow_exceeding_format_limits);}encoding_changed();});
        connect(inherit_bg_,&QCheckBox::toggled,this,[this](bool v){set_inherit_background_image(v);});connect(inherit_bg_color_,&QCheckBox::toggled,this,[this](bool v){set_inherit_background_color(v);});connect(inherit_style_,&QCheckBox::toggled,this,[this](bool v){set_inherit_style(v);});connect(inherit_audio_,&QCheckBox::toggled,this,[this](bool v){set_inherit_audio(v);});connect(inherit_duration_,&QCheckBox::toggled,this,[this](bool v){set_inherit_duration(v);});connect(inherit_encoding_,&QCheckBox::toggled,this,[this](bool v){set_inherit_encoding(v);});
        connect(scene_,&QGraphicsScene::selectionChanged,this,[this]{for(auto* item:scene_->selectedItems()){if(auto* b=dynamic_cast<ButtonPreviewItem*>(item)){if(b->index()!=BackIndex&&b->index()<static_cast<std::size_t>(button_list_->count()))button_list_->setCurrentRow(static_cast<int>(b->index()));break;}if(auto* o=dynamic_cast<OverlayPreviewItem*>(item)){if(o->index()<static_cast<std::size_t>(overlay_list_->count()))overlay_list_->setCurrentRow(static_cast<int>(o->index()));break;}}});
        connect(target_,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this](int){const auto previous_rate=frame_rate_->currentData().toString().toStdString();const auto previous_aspect=stored_menu_aspect(displayed_target_);const auto previous_target=displayed_target_;const auto previous_capacity=selected_disc_capacity_bytes();const auto next=static_cast<DiscTarget>(target_->currentData().toInt());store_visible_title_profiles(displayed_target_);store_visible_menu_mode();displayed_target_=next;const auto target_default=project_file_.isEmpty()?new_project_disc_capacity_for_target(project_defaults_,next):default_disc_capacity_bytes(next);const auto previous_default=project_file_.isEmpty()?new_project_disc_capacity_for_target(project_defaults_,previous_target):default_disc_capacity_bytes(previous_target);const auto next_capacity=previous_capacity==previous_default?target_default:((disc_capacity_is_unlimited(previous_capacity)||target_has_capacity_preset(next,previous_capacity))?previous_capacity:target_default);populate_size_target_choices(next,next_capacity);populate_frame_rate_choices(previous_rate);populate_resolution_choices(menu_resolution_,displayed_target_,stored_menu_resolution(displayed_target_),true);populate_aspect_choices(menu_aspect_,displayed_target_,stored_menu_aspect(displayed_target_),false,stored_menu_resolution(displayed_target_));stored_menu_resolution(displayed_target_)=menu_resolution_->currentData().toString().toStdString();stored_menu_aspect(displayed_target_)=menu_aspect_->currentData().toString().toStdString();apply_editor_aspect_transition(previous_aspect,stored_menu_aspect(displayed_target_));load_visible_title_profiles(displayed_target_);load_menu_controls();menu_resolution_->setEnabled(next!=DiscTarget::Avchd);menu_aspect_->setEnabled(next!=DiscTarget::Avchd);if(add_menu_button_)add_menu_button_->setEnabled(next!=DiscTarget::Avchd);update_build_button_text();const QString target_message=next==DiscTarget::Avchd?QStringLiteral("AVCHD export is limited to one menuless title; AVC/H.264 video and AC-3 or LPCM audio are supported."):next==DiscTarget::DvdVideo480p?QStringLiteral("DVD-Video menu resolution is independent and defaults to 720×480 NTSC; PAL cannot be mixed with the NTSC family."):QStringLiteral("Each output target keeps its own menu resolution/aspect settings.");statusBar()->showMessage(target_message,7000);});
        connect(frame_rate_,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this](int){const auto project_rate=frame_rate_->currentData().toString().toStdString();if(menu_video_&&menu_keyframe_)configure_keyframe_interval_spin(menu_keyframe_,displayed_target_,static_cast<VideoCodec>(menu_video_->currentData().toInt()),project_rate);for(int r=0;r<titles_->rowCount();++r){auto* rate=qobject_cast<QComboBox*>(titles_->cellWidget(r,9));auto* video=qobject_cast<QComboBox*>(titles_->cellWidget(r,2));auto* keyframe=qobject_cast<QSpinBox*>(titles_->cellWidget(r,13));if(rate&&video&&keyframe&&rate->currentData().toString()==QStringLiteral("inherit"))configure_keyframe_interval_spin(keyframe,displayed_target_,static_cast<VideoCodec>(video->currentData().toInt()),project_rate);}});
        connect(startupSequence,&QPushButton::clicked,this,[this]{edit_startup_sequence();});
        connect(build_,&QPushButton::clicked,this,[this]{build_project();});connect(cancel_,&QPushButton::clicked,this,[this]{cancel_render();});connect(automatic_bitrate_,&QCheckBox::toggled,this,[this](bool){update_enabled();});
        connect(titles_,&QTableWidget::itemChanged,this,[this](QTableWidgetItem*){mark_modified();});
        for(auto* editor:findChildren<QWidget*>())watch_editor(editor);
        rebuild_menu_tree();load_menu_controls();refresh_all();menu_resolution_->setEnabled(displayed_target_!=DiscTarget::Avchd);menu_aspect_->setEnabled(displayed_target_!=DiscTarget::Avchd);suppress_modified_=false;reset_history();mark_clean();refresh_external_tool_availability();QTimer::singleShot(0,this,[]{maybe_run_scheduled_cache_cleanup();});QTimer::singleShot(0,this,[this]{show_tool_warnings();});
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
    void edit_storage_settings(){
        QDialog d(this);d.setWindowTitle("Storage locations");d.resize(760,220);auto* root=new QVBoxLayout(&d);
        auto* note=new QLabel("These locations are application settings for this computer. They are not stored in .bdmvproject files. Cache-staging files stay inside the cache tree so completed cache entries can be published atomically.",&d);note->setWordWrap(true);root->addWidget(note);
        QSettings settings;
        auto* form=new QFormLayout;
        auto make_path_row=[&](const char* key,const fs::path& fallback,const QString& title){
            auto* line=new QLineEdit(&d);const auto saved=settings.value(key).toString().trimmed();line->setText(saved);line->setPlaceholderText(QStringLiteral("Default: %1").arg(QString::fromStdString(fallback.string())));
            auto* row=new QWidget(&d);auto* hl=new QHBoxLayout(row);hl->setContentsMargins(0,0,0,0);hl->addWidget(line,1);auto* browse=new QPushButton("Browse…",row);auto* reset=new QPushButton("Use default",row);hl->addWidget(browse);hl->addWidget(reset);
            connect(browse,&QPushButton::clicked,&d,[line,fallback,&d]{const QString start=line->text().trimmed().isEmpty()?QString::fromStdString(fallback.string()):line->text().trimmed();const QString chosen=QFileDialog::getExistingDirectory(&d,"Choose directory",start);if(!chosen.isEmpty())line->setText(chosen);});
            connect(reset,&QPushButton::clicked,line,&QLineEdit::clear);form->addRow(title,row);return line;
        };
        auto* cache=make_path_row("storage/cacheDirectory",default_gui_cache_root(),"Cache location");
        auto* temporary=make_path_row("storage/temporaryDirectory",default_gui_temporary_root(),"Temporary files location");
        root->addLayout(form);
        auto* buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&d);root->addWidget(buttons);connect(buttons,&QDialogButtonBox::accepted,&d,&QDialog::accept);connect(buttons,&QDialogButtonBox::rejected,&d,&QDialog::reject);
        if(d.exec()!=QDialog::Accepted)return;
        const auto resolve=[&](QLineEdit* line,const fs::path& fallback){const auto t=line->text().trimmed();return t.isEmpty()?fallback:fs::path(t.toStdString());};
        const auto validate=[&](const fs::path& path,const char* label)->bool{if(path.empty()){QMessageBox::warning(this,"Invalid storage location",QString("%1 could not be determined.").arg(label));return false;}std::error_code ec;fs::create_directories(path,ec);if(ec){QMessageBox::warning(this,"Invalid storage location",QString("Cannot create %1:\n%2").arg(label,QString::fromStdString(path.string())));return false;}const auto probe=path/(".bdmvauthor-write-test-"+std::to_string(QDateTime::currentMSecsSinceEpoch()));{std::ofstream f(probe,std::ios::binary);if(!f){QMessageBox::warning(this,"Invalid storage location",QString("%1 is not writable:\n%2").arg(label,QString::fromStdString(path.string())));return false;}f<<"test";}fs::remove(probe,ec);return true;};
        if(!validate(resolve(cache,default_gui_cache_root()),"Cache location")||!validate(resolve(temporary,default_gui_temporary_root()),"Temporary files location"))return;
        if(cache->text().trimmed().isEmpty())settings.remove("storage/cacheDirectory");else settings.setValue("storage/cacheDirectory",cache->text().trimmed());
        if(temporary->text().trimmed().isEmpty())settings.remove("storage/temporaryDirectory");else settings.setValue("storage/temporaryDirectory",temporary->text().trimmed());
        settings.sync();statusBar()->showMessage(QStringLiteral("Storage locations updated. New builds will use cache %1 and temporary files %2.").arg(QString::fromStdString(gui_cache_root().string()),QString::fromStdString(gui_temporary_root().string())),7000);
    }
    void edit_cache_settings(){
        QDialog d(this);d.setWindowTitle("Cache settings");auto* root=new QVBoxLayout(&d);
        auto* app_policy=new QComboBox(&d);app_policy->addItem("Enabled for software and hardware encodes","enabled");app_policy->addItem("Disable cache for hardware video encodes only","hardware-bypass");app_policy->addItem("Disable all caches entirely","disabled");
        QSettings app_settings;const auto saved_policy=app_settings.value("cache/encodedMediaPolicy",QStringLiteral("enabled")).toString();int app_policy_index=app_policy->findData(saved_policy);app_policy->setCurrentIndex(app_policy_index>=0?app_policy_index:0);app_policy->setToolTip("Application setting. This is not saved in the project. The hardware-only mode bypasses encoded-video cache lookup and publication only for the H.264/AVC (hardware) and H.265/HEVC (hardware) codec choices; software encodes and audio can still use the cache. The disabled mode disables both encoded-media and compliance caches application-wide.");
        auto* app_form=new QFormLayout;app_form->addRow("Application cache policy",app_policy);root->addLayout(app_form);
        auto* encode=new QCheckBox("Use encoded-media cache for this project",&d);encode->setChecked(cache_enabled_state_);
        auto* compliance=new QCheckBox("Use cached compliance analysis for this project",&d);compliance->setChecked(compliance_cache_enabled_state_);
        auto* refresh_encode=new QCheckBox("Refresh encoded-media cache this build",&d);refresh_encode->setChecked(refresh_encode_cache_state_);
        auto* refresh_compliance=new QCheckBox("Re-run compliance analysis this build",&d);refresh_compliance->setChecked(refresh_compliance_cache_state_);
        auto sync=[=]{const bool app_cache_enabled=app_policy->currentData().toString()!="disabled";refresh_encode->setEnabled(encode->isChecked()&&app_cache_enabled);refresh_compliance->setEnabled(compliance->isChecked()&&app_cache_enabled);if(!encode->isChecked()||!app_cache_enabled)refresh_encode->setChecked(false);if(!compliance->isChecked()||!app_cache_enabled)refresh_compliance->setChecked(false);};
        connect(encode,&QCheckBox::toggled,&d,[=](bool){sync();});connect(compliance,&QCheckBox::toggled,&d,[=](bool){sync();});connect(app_policy,&QComboBox::currentIndexChanged,&d,[=](int){sync();});sync();
        root->addWidget(encode);root->addWidget(compliance);root->addSpacing(8);root->addWidget(refresh_encode);root->addWidget(refresh_compliance);
        auto* note=new QLabel("The application policy above is saved in BDMV Author settings, not in .bdmvproject files. The project checkboxes remain project-specific. Disabling encoded-media cache entirely at the application level overrides the project encoded-media cache checkbox without changing its saved value.",&d);note->setWordWrap(true);root->addWidget(note);
        auto* manager=new QPushButton("Cache manager…",&d);root->addWidget(manager);connect(manager,&QPushButton::clicked,&d,[this]{CacheManagerDialog(this).exec();});
        auto* buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&d);root->addWidget(buttons);connect(buttons,&QDialogButtonBox::accepted,&d,&QDialog::accept);connect(buttons,&QDialogButtonBox::rejected,&d,&QDialog::reject);
        if(d.exec()!=QDialog::Accepted)return;
        app_settings.setValue("cache/encodedMediaPolicy",app_policy->currentData().toString());app_settings.sync();
        const bool persistent_changed=cache_enabled_state_!=encode->isChecked()||compliance_cache_enabled_state_!=compliance->isChecked();
        cache_enabled_state_=encode->isChecked();compliance_cache_enabled_state_=compliance->isChecked();refresh_encode_cache_state_=refresh_encode->isChecked();refresh_compliance_cache_state_=refresh_compliance->isChecked();
        if(persistent_changed)mark_modified();
    }
    void set_target_availability(){
        if(!target_)return;
        for(int row=0;row<target_->count();++row){const auto target=static_cast<DiscTarget>(target_->itemData(row).toInt());set_combo_item_enabled(target_,row,target_available(target),QStringLiteral("Required external tools/encoders for this output target are unavailable. Configure them in Settings → External tools and encoders."));}
    }
    void refresh_external_tool_availability(){
        // A capability rescan rebuilds the codec widgets below.  Commit the
        // currently visible title controls first so a worker/tool-settings
        // change cannot restore an older per-target profile (most visibly,
        // H.264/H.265 hardware encoding falling back to the stored software
        // codec merely because the combo box is being repopulated).
        if(titles_)store_visible_title_profiles(displayed_target_);
        set_target_availability();
        if(menu_video_&&menu_audio_codec_)load_menu_controls();
        if(titles_)load_visible_title_profiles(displayed_target_);
        if(build_)build_->setEnabled(target_available(displayed_target_));
        statusBar()->showMessage(QStringLiteral("External-tool scan complete. %1").arg(target_available(displayed_target_)?"The selected target is available.":"The selected target is unavailable; see Settings → External tools and encoders."),5000);
    }
    void edit_tool_settings(){
        ToolSettingsDialog d(this,g_tool_capabilities.config);if(d.exec()!=QDialog::Accepted)return;save_tool_configuration(d.value());g_tool_capabilities=probe_tool_capabilities(d.value());refresh_external_tool_availability();if(!g_tool_capabilities.warnings.isEmpty())QMessageBox::warning(this,"External tool warnings",g_tool_capabilities.warnings.join("\n\n")+"\n\nUnavailable codec choices and targets have been disabled.");
    }
    void edit_encoding_workers(){
        auto current=load_tool_configuration();RemoteServersDialog d(this,current);if(d.exec()!=QDialog::Accepted)return;
        auto updated=d.value();refresh_legacy_remote_fields(updated);save_tool_configuration(updated);
        g_tool_capabilities=probe_tool_capabilities(load_tool_configuration());refresh_external_tool_availability();
        if(!g_tool_capabilities.warnings.isEmpty())QMessageBox::warning(this,"Encoding worker warnings",g_tool_capabilities.warnings.join("\n\n"));
    }
    void show_tool_warnings(){
        if(g_tool_capabilities.warnings.isEmpty())return;
        QMessageBox::warning(this,"External tool warnings",g_tool_capabilities.warnings.join("\n\n")+"\n\nOpen Settings → External tools and encoders to select alternate executables or providers. Unavailable codecs are disabled rather than making the whole FFmpeg installation unusable.");
    }
    QString profile_tool_problem(const EncodingProfile& e,DiscTarget target,const QString& where)const{
        if(!video_codec_available(target,e.video_codec))return QStringLiteral("%1 uses %2, but that encoder is unavailable.").arg(where,video_codec_display_name(e.video_codec));
        if(!audio_codec_available(target,e.audio_codec))return QStringLiteral("%1 uses %2, but its audio encoder or PCM feeder is unavailable.").arg(where,audio_codec_display_name(e.audio_codec));
        return {};
    }
    QString build_tool_problem(const Project& p)const{
        if(p.target==DiscTarget::Avchd){
            if(p.titles.size()!=1U)return QStringLiteral("AVCHD export currently requires exactly one title.");
            if(project_has_menu(p))return QStringLiteral("AVCHD export currently requires a menuless project. Remove the main menu before building.");
        }
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
        if(source.inherit_background_image){e.background_image=p.background_image;e.background_video=p.background_video;}if(source.inherit_background_color)e.background_color=p.background_color;if(source.inherit_button_style)e.default_button_style=p.default_button_style;if(source.inherit_encoding){e.encoding=p.encoding;e.uhd_encoding=p.uhd_encoding;e.dvd_encoding=p.dvd_encoding;e.encoding_worker=p.encoding_worker;e.encoding_worker_mode=p.encoding_worker_mode;}if(source.inherit_audio)e.audio_source=p.audio_source;if(source.inherit_duration)e.duration_seconds=p.duration_seconds;e.submenus.clear();return e;
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
        else if(target==DiscTarget::BluRay1080||target==DiscTarget::Avchd){add("1920×1080","1920x1080");add("1440×1080 anamorphic","1440x1080");add("1280×720","1280x720");add("720×576 SD","720x576");add("720×480 SD","720x480");}
        else{add("720×480 NTSC","720x480");add("720×576 PAL","720x576");add("704×480 NTSC","704x480");add("704×576 PAL","704x576");add("352×480 NTSC","352x480");add("352×576 PAL","352x576");add("352×240 NTSC","352x240");add("352×288 PAL","352x288");}
        std::string wanted=selected;if(menu&&wanted=="highest")wanted=target==DiscTarget::UltraHdBluRay2160?"3840x2160":target==DiscTarget::DvdVideo480p?"720x480":"1920x1080";else if(menu&&wanted.empty())wanted=target==DiscTarget::UltraHdBluRay2160?"1920x1080":target==DiscTarget::DvdVideo480p?"720x480":"1920x1080";
        int index=combo->findData(QString::fromStdString(wanted));if(index<0){const auto fallback=target==DiscTarget::UltraHdBluRay2160?QStringLiteral("1920x1080"):target==DiscTarget::DvdVideo480p?QStringLiteral("720x480"):QStringLiteral("1920x1080");index=menu?combo->findData(fallback):0;}if(index<0)index=0;combo->setCurrentIndex(index);
        combo->setToolTip(target==DiscTarget::Avchd?"AVCHD export is menuless; this menu control is inactive for AVCHD.":(menu?"Menu resolution is saved independently for Blu-ray, Ultra HD Blu-ray, and DVD. New projects use 1920×1080 for Blu-ray and UHD, and 720×480 NTSC for DVD. UHD defaults to 1080p to work around a VLC mouse-navigation coordinate bug; selecting 3840×2160 remains available.":"Auto chooses the legal raster closest to the source, preferring scaling up when two choices are equally close."));
    }
    static void populate_aspect_choices(QComboBox* combo, DiscTarget target, const std::string& selected, bool title, const std::string& resolution = {}) {
        const QSignalBlocker blocker(combo);combo->clear();
        const auto r=QString::fromStdString(resolution).toLower();
        const bool resolution_auto=r.isEmpty()||r=="auto";
        bool allow_wide=true,allow_four_three=false;
        if(target==DiscTarget::UltraHdBluRay2160){allow_four_three=false;}
        else if(target==DiscTarget::BluRay1080||target==DiscTarget::Avchd){allow_four_three=resolution_auto||r=="720x480"||r=="720x576";}
        else{allow_four_three=true;allow_wide=resolution_auto||r=="720x480"||r=="720x576";}
        if(title&&resolution_auto)combo->addItem("Auto (match source)",QStringLiteral("auto"));
        if(allow_wide)combo->addItem("16:9",QStringLiteral("16:9"));
        if(allow_four_three)combo->addItem("4:3",QStringLiteral("4:3"));
        int index=combo->findData(QString::fromStdString(selected));if(index<0)index=0;combo->setCurrentIndex(index);
        combo->setToolTip(target==DiscTarget::UltraHdBluRay2160?"Ultra HD Blu-ray primary video may be 3840×2160 or 1920×1080; both are 16:9.":target==DiscTarget::Avchd?"AVCHD uses the Blu-ray-family AVC raster/aspect combinations; HD modes are 16:9 while SD modes may be 4:3 or 16:9.":"Aspect choices are restricted to combinations legal for the selected raster. Blu-ray HD modes are 16:9; DVD widescreen is offered at full-D1 720-wide modes while reduced DVD rasters are 4:3.");
    }
    void update_fast_menu_pass_enabled(){
        if(!menu_fast_first_||!menu_two_pass_||!menu_video_)return;
        const auto codec=static_cast<VideoCodec>(menu_video_->currentData().toInt());
        menu_fast_first_->setEnabled(menu_two_pass_->isChecked()&&(codec==VideoCodec::X264||codec==VideoCodec::Hevc));
    }
    void set_profile_widgets(const EncodingProfile& e){populate_video_combo(menu_video_,displayed_target_,e.video_codec);configure_video_bitrate_spin(menu_bitrate_,displayed_target_,e.video_codec,e.video_bitrate_kbps,authoring_limits_.allow_exceeding_format_limits);configure_video_minrate_spin(menu_minrate_,displayed_target_,e.video_codec,e.video_min_bitrate_kbps,authoring_limits_.allow_exceeding_format_limits);configure_video_maxrate_spin(menu_maxrate_,displayed_target_,e.video_codec,e.video_max_bitrate_kbps,authoring_limits_.allow_exceeding_format_limits);populate_preset_combo(menu_preset_,e.video_codec,encoding_preset(e));configure_pass_control(menu_two_pass_,e.video_codec,e.two_pass);menu_fast_first_->setChecked(e.fast_first_pass);update_fast_menu_pass_enabled();configure_keyframe_interval_spin(menu_keyframe_,displayed_target_,e.video_codec,frame_rate_->currentData().toString().toStdString(),e.keyframe_interval_frames);populate_audio_combo(menu_audio_codec_,displayed_target_,e.audio_codec);configure_audio_bitrate_spin(menu_audio_bitrate_,displayed_target_,e.audio_codec,audio_bitrate_kbps(e),authoring_limits_.allow_exceeding_format_limits);}
    void append_tree(QTreeWidgetItem* parent,const Menu& m){if(m.id.empty())return;auto* item=parent?new QTreeWidgetItem(parent):new QTreeWidgetItem(menu_tree_);item->setText(0,QString::fromStdString(m.name));item->setData(0,Qt::UserRole,QString::fromStdString(m.id));for(const auto& c:m.submenus)append_tree(item,c);}
    void rebuild_menu_tree(){const auto keep=current_menu_id_;menu_tree_->clear();if(!root_menu_.id.empty())append_tree(nullptr,root_menu_);menu_tree_->expandAll();QTreeWidgetItemIterator it(menu_tree_);while(*it){if((*it)->data(0,Qt::UserRole).toString().toStdString()==keep){menu_tree_->setCurrentItem(*it);break;}++it;}if(!menu_tree_->currentItem()&&menu_tree_->topLevelItemCount()>0)menu_tree_->setCurrentItem(menu_tree_->topLevelItem(0));if(add_menu_button_){add_menu_button_->setText(root_menu_.id.empty()?"Add main menu":"Add submenu");add_menu_button_->setEnabled(displayed_target_!=DiscTarget::Avchd);add_menu_button_->setToolTip(displayed_target_==DiscTarget::Avchd?"AVCHD export in BDMV Author is intentionally menuless.":QString());}if(remove_menu_button_)remove_menu_button_->setEnabled(!root_menu_.id.empty());}
    static bool gui_supported_subtitle_codec(const QString& codec){
        const auto c=codec.toLower();
        return c=="hdmv_pgs_subtitle"||c=="dvd_subtitle"||c=="hddvd_subtitle"||c=="subrip"||c=="srt"||c=="ass"||c=="ssa"||c=="webvtt"||c=="text"||c=="mov_text";
    }
    std::vector<TargetChoice> stream_target_choices() const {
        std::vector<TargetChoice> out;
        for(int r=0;r<titles_->rowCount();++r){
            const auto* source_item=titles_->item(r,1);if(!source_item||source_item->text().isEmpty())continue;
            QProcess probe;
            start_tool_process(probe,g_tool_capabilities.ffprobe,QStringList{"-v","error","-show_entries","stream=codec_type,codec_name:stream_tags=language,title","-of","json",source_item->text()});
            if(!probe.waitForStarted(2000)||!probe.waitForFinished(5000)||probe.exitStatus()!=QProcess::NormalExit||probe.exitCode()!=0)continue;
            QJsonParseError parse_error;const auto doc=QJsonDocument::fromJson(probe.readAllStandardOutput(),&parse_error);if(parse_error.error!=QJsonParseError::NoError||!doc.isObject())continue;
            const auto audio_settings=audio_stream_settings_from_variant(source_item->data(RoleAudioStreamSettings));
            const auto subtitle_settings=subtitle_stream_settings_from_variant(source_item->data(RoleSubtitleStreamSettings));
            auto audio_removed=[&](int ordinal){const auto it=std::find_if(audio_settings.begin(),audio_settings.end(),[&](const AudioStreamSettings& x){return x.source_ordinal==ordinal;});return it!=audio_settings.end()&&it->removed;};
            auto subtitle_removed=[&](int ordinal){const auto it=std::find_if(subtitle_settings.begin(),subtitle_settings.end(),[&](const SubtitleStreamSettings& x){return x.source_ordinal==ordinal;});return it!=subtitle_settings.end()&&it->removed;};
            int audio_source_ordinal=0,subtitle_source_ordinal=0,audio_number=0,subtitle_number=0;
            for(const auto& v:doc.object().value("streams").toArray()){
                const auto o=v.toObject();const auto type=o.value("codec_type").toString();const auto codec=o.value("codec_name").toString();const auto tags=o.value("tags").toObject();
                const auto language=tags.value("language").toString();const auto track_title=tags.value("title").toString();
                if(type=="audio"){
                    const int source_ordinal=audio_source_ordinal++;if(audio_removed(source_ordinal))continue;
                    ++audio_number;TargetChoice t;t.kind=MenuButtonTargetKind::AudioTrack;t.title=static_cast<std::uint16_t>(r+1);t.stream=static_cast<std::uint16_t>(audio_number);
                    QString detail=language;if(detail.isEmpty())detail=track_title;if(!track_title.isEmpty()&&track_title!=detail)detail+=(detail.isEmpty()?QString():QString(" — "))+track_title;
                    t.label=QString("Title %1 — Audio %2").arg(r+1).arg(audio_number);if(!detail.isEmpty())t.label+=" — "+detail;if(!codec.isEmpty())t.label+=QString(" [%1]").arg(codec);
                    t.suggested_label=!track_title.isEmpty()?track_title:(!language.isEmpty()?QString("Audio — %1").arg(language):QString("Audio %1").arg(audio_number));out.push_back(std::move(t));
                }else if(type=="subtitle"){
                    const int source_ordinal=subtitle_source_ordinal++;if(!gui_supported_subtitle_codec(codec)||subtitle_removed(source_ordinal))continue;
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
    // A shared operation handles buttons, image/text labels, and the automatic Back
    // button. Selected items can belong to different categories on the canvas.
    std::vector<Rect*> layout_targets(Menu& m) {
        std::vector<Rect*> out;
        const int scope=layout_scope_->currentIndex();
        if(scope==0){
            for(auto* item:scene_->selectedItems()){
                if(auto* b=dynamic_cast<ButtonPreviewItem*>(item)){
                    if(b->index()==BackIndex){
                        if(parent_of(root_menu_,m.id)&&m.auto_back_button)out.push_back(&m.back_button.bounds);
                    }else if(b->index()<m.buttons.size())out.push_back(&m.buttons[b->index()].bounds);
                }else if(auto* o=dynamic_cast<OverlayPreviewItem*>(item)){
                    if(o->index()<m.overlays.size())out.push_back(&m.overlays[o->index()].bounds);
                }
            }
        }else{
            if(scope==1||scope==3){
                for(auto& b:m.buttons)out.push_back(&b.bounds);
                if(parent_of(root_menu_,m.id)&&m.auto_back_button)out.push_back(&m.back_button.bounds);
            }
            if(scope==2||scope==3)for(auto& o:m.overlays)out.push_back(&o.bounds);
        }
        return out;
    }
    void apply_menu_layout(bool spacing, MenuAlign alignment, MenuAxis axis) {
        auto* m=current_menu();if(!m)return;
        auto targets=layout_targets(*m);
        if(targets.size()<2){
            statusBar()->showMessage("Select at least two objects with Ctrl-click, or choose an All objects scope.",6000);
            return;
        }
        // Preserve a multi-selection so successive Align and Space clicks can
        // operate on the same group after the scene is regenerated.
        std::vector<std::pair<int,std::size_t>> selection;
        if(layout_scope_->currentIndex()==0){
            for(auto* item:scene_->selectedItems()){
                if(auto* b=dynamic_cast<ButtonPreviewItem*>(item))selection.emplace_back(0,b->index());
                if(auto* o=dynamic_cast<OverlayPreviewItem*>(item))selection.emplace_back(1,o->index());
            }
        }
        const auto aperture=design_aperture(stored_menu_aspect(displayed_target_));
        const Rect bounds{static_cast<int>(aperture.left()),static_cast<int>(aperture.top()),
                          static_cast<int>(aperture.width()),static_cast<int>(aperture.height())};
        const bool changed=spacing?space_menu_rects(targets,axis,layout_gap_->value(),bounds)
                                  :align_menu_rects(targets,alignment,bounds);
        if(!changed){
            statusBar()->showMessage(spacing?"No change: the requested gap may exceed the menu aperture, or objects already have this spacing.":"Objects are already aligned.",6000);
            return;
        }
        invalidate_reversible_aspect_positions();mark_modified();commit_history();refresh_canvas();
        for(auto* item:scene_->items()){
            if(auto* b=dynamic_cast<ButtonPreviewItem*>(item)){
                if(std::find(selection.begin(),selection.end(),std::pair<int,std::size_t>{0,b->index()})!=selection.end())item->setSelected(true);
            }else if(auto* o=dynamic_cast<OverlayPreviewItem*>(item)){
                if(std::find(selection.begin(),selection.end(),std::pair<int,std::size_t>{1,o->index()})!=selection.end())item->setSelected(true);
            }
        }
        statusBar()->showMessage(spacing?"Menu objects spaced.":"Menu objects aligned.",3000);
    }
    void apply_menu_alignment(MenuAlign alignment){apply_menu_layout(false,alignment,MenuAxis::Horizontal);}
    void apply_menu_spacing(MenuAxis axis){apply_menu_layout(true,MenuAlign::Left,axis);}
    void refresh_all(){rebuild_menu_tree();refresh_button_list();refresh_overlay_list();refresh_canvas();}
    bool automatic_video_bitrate_enabled() const{return automatic_bitrate_&&automatic_bitrate_->isChecked();}
    void update_enabled(){const bool present=current_menu()!=nullptr;const bool sub=present&&parent_of(root_menu_,current_menu_id_)!=nullptr;const bool automatic=present&&automatic_menu_duration(effective_menu(*current_menu()));inherit_bg_->setEnabled(present&&sub);inherit_bg_color_->setEnabled(present&&sub);inherit_style_->setEnabled(present&&sub);inherit_audio_->setEnabled(present&&sub);inherit_duration_->setEnabled(present&&sub&&!automatic);inherit_encoding_->setEnabled(present&&sub);menu_name_->setEnabled(present);bg_type_->setEnabled(present&&(!sub||!inherit_bg_->isChecked()));bg_->setEnabled(present&&(!sub||!inherit_bg_->isChecked()));bg_color_->setEnabled(present&&(!sub||!inherit_bg_color_->isChecked()));default_style_button_->setEnabled(present&&(!sub||!inherit_style_->isChecked()));menu_audio_path_->setEnabled(present&&(!sub||!inherit_audio_->isChecked()));duration_->setEnabled(present&&!automatic&&(!sub||!inherit_duration_->isChecked()));loop_media_->setEnabled(present&&automatic);const bool enc=present&&(!sub||!inherit_encoding_->isChecked());if(menu_worker_)menu_worker_->setEnabled(enc);menu_video_->setEnabled(enc);menu_bitrate_->setEnabled(enc&&!automatic_video_bitrate_enabled());menu_minrate_->setEnabled(enc);menu_maxrate_->setEnabled(enc);menu_preset_->setEnabled(enc);const auto menu_codec=static_cast<VideoCodec>(menu_video_->currentData().toInt());menu_two_pass_->setEnabled(enc&&!video_codec_is_hardware(menu_codec));menu_fast_first_->setEnabled(enc&&menu_two_pass_->isChecked()&&(menu_codec==VideoCodec::X264||menu_codec==VideoCodec::Hevc));menu_keyframe_->setEnabled(enc);menu_audio_codec_->setEnabled(enc);menu_audio_bitrate_->setEnabled(enc&&audio_codec_has_configurable_bitrate(static_cast<AudioCodec>(menu_audio_codec_->currentData().toInt())));menu_advanced_->setEnabled(enc);auto_back_->setEnabled(present&&sub);edit_back_button_->setEnabled(present&&sub&&auto_back_->isChecked());for(int r=0;r<titles_->rowCount();++r)if(auto* bitrate=qobject_cast<QSpinBox*>(titles_->cellWidget(r,3)))bitrate->setEnabled(!automatic_video_bitrate_enabled());}
    void load_menu_controls(){auto* m=current_menu();if(!m){loading_menu_=true;menu_name_->clear();bg_->clear();menu_audio_path_->clear();loading_menu_=false;update_enabled();return;}const auto e=effective_menu(*m);const bool sub=parent_of(root_menu_,m->id)!=nullptr;loading_menu_=true;menu_name_->setText(QString::fromStdString(m->name));inherit_bg_->setChecked(sub&&m->inherit_background_image);inherit_bg_color_->setChecked(sub&&m->inherit_background_color);inherit_style_->setChecked(sub&&m->inherit_button_style);inherit_audio_->setChecked(sub&&m->inherit_audio);inherit_duration_->setChecked(sub&&m->inherit_duration);inherit_encoding_->setChecked(sub&&m->inherit_encoding);bg_type_->setCurrentIndex(e.background_video.empty()?0:1);bg_->setText(QString::fromStdString((e.background_video.empty()?e.background_image:e.background_video).string()));bg_color_->setValue(e.background_color);menu_audio_path_->setText(QString::fromStdString(e.audio_source.string()));loop_media_->setChecked(e.loop_media);set_duration_control_from_menu(e);set_profile_widgets(encoding_for_target(e,displayed_target_));populate_worker_affinity_combo(menu_worker_,e.encoding_worker,e.encoding_worker_mode,"Automatic scheduling");auto_back_->setChecked(sub&&m->auto_back_button);loading_menu_=false;update_enabled();}
    template<class Copy> void toggle_inherit(bool v,bool Menu::*flag,Copy copy){if(loading_menu_)return;auto* m=current_menu();if(!m||!parent_of(root_menu_,m->id))return;if(!v){const auto e=effective_menu(*m);copy(*m,e);}m->*flag=v;load_menu_controls();refresh_canvas();}
    void set_inherit_background_image(bool v){toggle_inherit(v,&Menu::inherit_background_image,[](Menu&m,const Menu&e){m.background_image=e.background_image;m.background_video=e.background_video;});}
    void set_inherit_background_color(bool v){toggle_inherit(v,&Menu::inherit_background_color,[](Menu&m,const Menu&e){m.background_color=e.background_color;});}
    void set_inherit_style(bool v){toggle_inherit(v,&Menu::inherit_button_style,[](Menu&m,const Menu&e){m.default_button_style=e.default_button_style;});}
    void set_inherit_audio(bool v){toggle_inherit(v,&Menu::inherit_audio,[](Menu&m,const Menu&e){m.audio_source=e.audio_source;});}
    void set_inherit_duration(bool v){toggle_inherit(v,&Menu::inherit_duration,[](Menu&m,const Menu&e){m.duration_seconds=e.duration_seconds;});}
    void set_inherit_encoding(bool v){toggle_inherit(v,&Menu::inherit_encoding,[](Menu&m,const Menu&e){m.encoding=e.encoding;m.uhd_encoding=e.uhd_encoding;m.dvd_encoding=e.dvd_encoding;m.encoding_worker=e.encoding_worker;m.encoding_worker_mode=e.encoding_worker_mode;});}
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
        sourceItem->setData(RoleAc3Bitrate,t.encoding.ac3_bitrate_kbps);sourceItem->setData(RoleDcaBitrate,t.encoding.dca_bitrate_kbps);sourceItem->setData(RoleHdProfile,encoding_map(t.encoding));sourceItem->setData(RoleUhdProfile,encoding_map(t.uhd_encoding));sourceItem->setData(RoleAvchdProfile,encoding_map(t.avchd_encoding));sourceItem->setData(RoleDvdProfile,encoding_map(t.dvd_encoding));sourceItem->setData(RoleHdFrameRate,QString::fromStdString(t.frame_rate));sourceItem->setData(RoleUhdFrameRate,QString::fromStdString(t.uhd_frame_rate));sourceItem->setData(RoleAvchdFrameRate,QString::fromStdString(t.avchd_frame_rate));sourceItem->setData(RoleDvdFrameRate,QString::fromStdString(t.dvd_frame_rate));sourceItem->setData(RoleHdResolution,QString::fromStdString(t.resolution));sourceItem->setData(RoleUhdResolution,QString::fromStdString(t.uhd_resolution));sourceItem->setData(RoleAvchdResolution,QString::fromStdString(t.avchd_resolution));sourceItem->setData(RoleDvdResolution,QString::fromStdString(t.dvd_resolution));sourceItem->setData(RoleHdAspect,QString::fromStdString(t.aspect_ratio));sourceItem->setData(RoleUhdAspect,QString::fromStdString(t.uhd_aspect_ratio));sourceItem->setData(RoleAvchdAspect,QString::fromStdString(t.avchd_aspect_ratio));sourceItem->setData(RoleDvdAspect,QString::fromStdString(t.dvd_aspect_ratio));sourceItem->setData(RoleTitleMenuActions,navigation_actions_variant(t.menu_button_actions));sourceItem->setData(RoleTitleUopMask,QVariant::fromValue<qulonglong>(t.prohibited_user_operations));sourceItem->setData(RoleAudioStreamSettings,audio_stream_settings_variant(t.audio_stream_settings));sourceItem->setData(RoleSubtitleStreamSettings,subtitle_stream_settings_variant(t.subtitle_stream_settings));sourceItem->setData(RoleExternalSubtitles,external_subtitles_variant(t.external_subtitles));sourceItem->setData(RoleSubtitleDefaultStyle,subtitle_style_map(t.subtitle_default_style));sourceItem->setData(RoleDefaultAudioStream,t.default_audio_stream);sourceItem->setData(RoleDefaultSubtitleStream,t.default_subtitle_stream);sourceItem->setData(RoleTitleEncodingWorker,QString::fromStdString(t.encoding_worker));sourceItem->setData(RoleTitleEncodingWorkerMode,static_cast<int>(t.encoding_worker_mode));
        titles_->setItem(r,0,nameItem);titles_->setItem(r,1,sourceItem);
        const auto& active=encoding_for_target(t,displayed_target_);
        auto* video=video_combo(titles_,displayed_target_,active.video_codec);titles_->setCellWidget(r,2,video);
        auto* bitrate=bitrate_spin(titles_);configure_video_bitrate_spin(bitrate,displayed_target_,active.video_codec,active.video_bitrate_kbps,authoring_limits_.allow_exceeding_format_limits);titles_->setCellWidget(r,3,bitrate);
        auto* preset=preset_combo(titles_,active.video_codec,encoding_preset(active));titles_->setCellWidget(r,4,preset);
        auto* two=new QCheckBox(titles_);configure_pass_control(two,active.video_codec,active.two_pass);titles_->setCellWidget(r,5,two);
        auto* audio=audio_combo(titles_,displayed_target_,active.audio_codec);titles_->setCellWidget(r,6,audio);
        auto* audioBitrate=audio_bitrate_spin(titles_);configure_audio_bitrate_spin(audioBitrate,displayed_target_,active.audio_codec,audio_bitrate_kbps(active),authoring_limits_.allow_exceeding_format_limits);titles_->setCellWidget(r,7,audioBitrate);auto* chapterButton=new QPushButton(chapter_summary(t.chapter_mode,t.chapter_interval_seconds,t.chapters_seconds.size()),titles_);chapterButton->setToolTip("Choose source, manual, timed, or no chapter divisions for this title.");titles_->setCellWidget(r,8,chapterButton);auto* frameRate=new QComboBox(titles_);populate_title_frame_rate_choices(frameRate,displayed_target_,frame_rate_for_target(t,displayed_target_));titles_->setCellWidget(r,9,frameRate);auto* force=new QCheckBox;force->setChecked(t.force_reencode);force->setToolTip("Force both video and audio for this title to be re-encoded using the selected fallback encoding settings.");force->setStyleSheet("margin-left: 12px; margin-right: 12px;");titles_->setCellWidget(r,10,force);auto* resolution=new QComboBox(titles_);populate_resolution_choices(resolution,displayed_target_,resolution_for_target(t,displayed_target_),false);titles_->setCellWidget(r,11,resolution);auto* aspect=new QComboBox(titles_);populate_aspect_choices(aspect,displayed_target_,aspect_ratio_for_target(t,displayed_target_),true,resolution_for_target(t,displayed_target_));titles_->setCellWidget(r,12,aspect);auto* keyframe=keyframe_interval_spin(titles_,active.keyframe_interval_frames);{auto rate_token=frameRate->currentData().toString().toStdString();if(rate_token=="inherit")rate_token=frame_rate_->currentData().toString().toStdString();configure_keyframe_interval_spin(keyframe,displayed_target_,active.video_codec,rate_token,active.keyframe_interval_frames);}titles_->setCellWidget(r,13,keyframe);auto* advanced=new QPushButton("Advanced…",titles_);advanced->setToolTip("Set codec-private options for this title while preserving disc-spec-critical settings.");titles_->setCellWidget(r,14,advanced);auto* navigation=new QPushButton("Navigation…",titles_);navigation->setToolTip("Set this title's player Menu-button behavior and User Operation Prohibitions.");titles_->setCellWidget(r,15,navigation);auto* streams=new QPushButton("Streams…",titles_);streams->setToolTip("Configure individual audio codecs, bitrates, LPCM format, downmix/channel layout, advanced options, language flags, and attached subtitle files.");titles_->setCellWidget(r,16,streams);auto* minrate=video_rate_limit_spin(titles_);configure_video_minrate_spin(minrate,displayed_target_,active.video_codec,active.video_min_bitrate_kbps,authoring_limits_.allow_exceeding_format_limits);titles_->setCellWidget(r,17,minrate);auto* maxrate=video_rate_limit_spin(titles_);configure_video_maxrate_spin(maxrate,displayed_target_,active.video_codec,active.video_max_bitrate_kbps,authoring_limits_.allow_exceeding_format_limits);titles_->setCellWidget(r,18,maxrate);
        auto* fast=new QCheckBox(titles_);fast->setChecked(active.fast_first_pass);
        fast->setToolTip("Fast first pass: x264 uses this by default; x265 uses no-slow-firstpass. Only active with two-pass video encoding.");
        fast->setEnabled(active.two_pass&&(active.video_codec==VideoCodec::X264||active.video_codec==VideoCodec::Hevc));
        titles_->setCellWidget(r,19,fast);
        two->setProperty("fastFirstPassControl",QVariant::fromValue<QObject*>(fast));
        connect(two,&QCheckBox::toggled,this,[fast,video](bool checked){const auto codec=static_cast<VideoCodec>(video->currentData().toInt());fast->setEnabled(checked&&(codec==VideoCodec::X264||codec==VideoCodec::Hevc));});
        connect(chapterButton,&QPushButton::clicked,this,[this,sourceItem,chapterButton]{edit_title_chapters(sourceItem,chapterButton);});
        connect(resolution,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this,resolution,aspect](int){const auto keep=aspect->currentData().toString().toStdString();populate_aspect_choices(aspect,displayed_target_,keep,true,resolution->currentData().toString().toStdString());});
        connect(video,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this,video,bitrate,minrate,maxrate,preset,two,frameRate,keyframe](int){const auto codec=static_cast<VideoCodec>(video->currentData().toInt());configure_video_bitrate_spin(bitrate,displayed_target_,codec,bitrate->value(),authoring_limits_.allow_exceeding_format_limits);configure_video_minrate_spin(minrate,displayed_target_,codec,minrate->value(),authoring_limits_.allow_exceeding_format_limits);configure_video_maxrate_spin(maxrate,displayed_target_,codec,maxrate->value(),authoring_limits_.allow_exceeding_format_limits);populate_preset_combo(preset,codec);configure_pass_control(two,codec,two->isChecked());auto* fast=qobject_cast<QCheckBox*>(two->property("fastFirstPassControl").value<QObject*>());if(fast)fast->setEnabled(two->isChecked()&&(codec==VideoCodec::X264||codec==VideoCodec::Hevc));auto rate_token=frameRate->currentData().toString().toStdString();if(rate_token=="inherit")rate_token=frame_rate_->currentData().toString().toStdString();configure_keyframe_interval_spin(keyframe,displayed_target_,codec,rate_token);});
        connect(frameRate,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this,video,frameRate,keyframe](int){auto rate_token=frameRate->currentData().toString().toStdString();if(rate_token=="inherit")rate_token=frame_rate_->currentData().toString().toStdString();configure_keyframe_interval_spin(keyframe,displayed_target_,static_cast<VideoCodec>(video->currentData().toInt()),rate_token);});
        connect(audioBitrate,QOverload<int>::of(&QSpinBox::valueChanged),this,[this,sourceItem,audio](int v){const auto role=profile_role(displayed_target_);auto e=encoding_from_map(sourceItem->data(role),default_profile_for_target(displayed_target_));e.audio_codec=static_cast<AudioCodec>(audio->currentData().toInt());if(audio_codec_has_configurable_bitrate(e.audio_codec))set_audio_bitrate_kbps(e,v);sourceItem->setData(role,encoding_map(e));});
        connect(audio,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this,sourceItem,audio,audioBitrate](int){const auto role=profile_role(displayed_target_);auto e=encoding_from_map(sourceItem->data(role),default_profile_for_target(displayed_target_));const auto codec=static_cast<AudioCodec>(audio->currentData().toInt());e.audio_codec=codec;configure_audio_bitrate_spin(audioBitrate,displayed_target_,codec,audio_bitrate_kbps(e),authoring_limits_.allow_exceeding_format_limits);sourceItem->setData(role,encoding_map(e));});
        connect(advanced,&QPushButton::clicked,this,[this,advanced]{int row=-1;for(int i=0;i<titles_->rowCount();++i)if(titles_->cellWidget(i,14)==advanced){row=i;break;}if(row<0)return;auto* source=titles_->item(row,1);if(!source)return;auto e=row_profile(row);if(edit_advanced_codec_options(this,displayed_target_,e)){source->setData(profile_role(displayed_target_),encoding_map(e));mark_modified();}});
        connect(navigation,&QPushButton::clicked,this,[this,navigation]{int row=-1;for(int i=0;i<titles_->rowCount();++i)if(titles_->cellWidget(i,15)==navigation){row=i;break;}if(row<0)return;auto* source=titles_->item(row,1);if(!source)return;TitleNavigationDialog d(this,navigation_actions_from_variant(source->data(RoleTitleMenuActions)),source->data(RoleTitleUopMask).toULongLong(),target_choices(),!root_menu_.id.empty());if(d.exec()!=QDialog::Accepted)return;source->setData(RoleTitleMenuActions,navigation_actions_variant(d.actions()));source->setData(RoleTitleUopMask,QVariant::fromValue<qulonglong>(d.prohibited_user_operations()));mark_modified();});
        connect(streams,&QPushButton::clicked,this,[this,streams]{
            int row=-1;for(int i=0;i<titles_->rowCount();++i)if(titles_->cellWidget(i,16)==streams){row=i;break;}
            if(row<0)return;
            auto* source=titles_->item(row,1);if(!source)return;
            store_visible_title_profiles(displayed_target_);
            const auto hd=stored_row_profile(row,DiscTarget::BluRay1080);
            const auto uhd=stored_row_profile(row,DiscTarget::UltraHdBluRay2160);
            const auto avchd=stored_row_profile(row,DiscTarget::Avchd);
            const auto dvd=stored_row_profile(row,DiscTarget::DvdVideo480p);
            const auto stream_profile=row_profile(row);
            const auto old_audio=audio_stream_settings_from_variant(source->data(RoleAudioStreamSettings));
            const auto old_subtitles=subtitle_stream_settings_from_variant(source->data(RoleSubtitleStreamSettings));
            const auto old_external=external_subtitles_from_variant(source->data(RoleExternalSubtitles));
            std::vector<GuiAudioTrack> source_audio;
            std::vector<GuiSubtitleTrack> source_subtitles;
            const bool have_source_catalog=probe_gui_title_streams(source->text(),source_audio,source_subtitles);
            TitleStreamsDialog d(this,source->text(),displayed_target_,stream_profile.video_bitrate_kbps,hd,uhd,avchd,dvd,
                subtitle_style_from_map(source->data(RoleSubtitleDefaultStyle)),old_audio,old_subtitles,old_external,
                source->data(RoleTitleEncodingWorker).toString().toStdString(),
                static_cast<EncodingWorkerAffinityMode>(source->data(RoleTitleEncodingWorkerMode).isValid()?source->data(RoleTitleEncodingWorkerMode).toInt():static_cast<int>(EncodingWorkerAffinityMode::Automatic)),
                source->data(RoleDefaultAudioStream).toInt(),
                source->data(RoleDefaultSubtitleStream).isValid()?source->data(RoleDefaultSubtitleStream).toInt():-1,
                authoring_limits_.allow_exceeding_format_limits);
            if(d.exec()!=QDialog::Accepted)return;
            const auto new_audio=d.audio_settings();
            const auto new_subtitles=d.subtitle_settings();
            const auto new_external=d.external_subtitles();
            if(have_source_catalog){
                const auto old_audio_order=authored_audio_stream_keys(source_audio,old_audio);
                const auto new_audio_order=authored_audio_stream_keys(source_audio,new_audio);
                const auto old_subtitle_order=authored_subtitle_stream_keys(source_subtitles,old_subtitles,old_external);
                const auto new_subtitle_order=authored_subtitle_stream_keys(source_subtitles,new_subtitles,new_external);
                const auto title_number=static_cast<std::uint16_t>(row+1);
                remap_stream_actions(root_menu_,title_number,old_audio_order,new_audio_order,old_subtitle_order,new_subtitle_order);
                remap_stream_actions(first_play_actions_,title_number,old_audio_order,new_audio_order,old_subtitle_order,new_subtitle_order);
                for(int title_row=0;title_row<titles_->rowCount();++title_row){
                    auto* title_source=titles_->item(title_row,1);if(!title_source)continue;
                    auto actions=navigation_actions_from_variant(title_source->data(RoleTitleMenuActions));
                    remap_stream_actions(actions,title_number,old_audio_order,new_audio_order,old_subtitle_order,new_subtitle_order);
                    title_source->setData(RoleTitleMenuActions,navigation_actions_variant(actions));
                }
            }
            source->setData(RoleAudioStreamSettings,audio_stream_settings_variant(new_audio));
            source->setData(RoleSubtitleStreamSettings,subtitle_stream_settings_variant(new_subtitles));
            source->setData(RoleExternalSubtitles,external_subtitles_variant(new_external));
            source->setData(RoleDefaultAudioStream,d.default_audio_stream());
            source->setData(RoleDefaultSubtitleStream,d.default_subtitle_stream());
            source->setData(RoleTitleEncodingWorker,QString::fromStdString(d.title_encoding_worker()));
            source->setData(RoleTitleEncodingWorkerMode,static_cast<int>(d.title_encoding_worker_mode()));
            mark_modified();
            commit_history();
            refresh_button_list();
            refresh_canvas();
        });
        bitrate->setEnabled(!automatic_video_bitrate_enabled());
        watch_editor(video);watch_editor(bitrate);watch_editor(minrate);watch_editor(maxrate);watch_editor(preset);watch_editor(two);watch_editor(fast);watch_editor(audio);watch_editor(audioBitrate);watch_editor(frameRate);watch_editor(force);watch_editor(resolution);watch_editor(aspect);watch_editor(keyframe);
    }
    void add_videos(){if(displayed_target_==DiscTarget::Avchd&&titles_->rowCount()>=1){QMessageBox::information(this,"AVCHD export","AVCHD export in BDMV Author supports exactly one title. Remove the current title before adding another.");return;}QFileDialog d(this,"Add video titles");d.setFileMode(QFileDialog::ExistingFiles);d.setOption(QFileDialog::DontUseNativeDialog,true);auto* add_button=new QCheckBox("Automatically add a menu button for each video",&d);add_button->setChecked(true);if(root_menu_.id.empty()){add_button->setChecked(false);add_button->setEnabled(false);add_button->setToolTip("No main menu is present. Add a main menu first to create automatic buttons.");}if(auto* grid=qobject_cast<QGridLayout*>(d.layout()))grid->addWidget(add_button,grid->rowCount(),0,1,std::max(1,grid->columnCount()));if(d.exec()!=QDialog::Accepted)return;const auto selected=d.selectedFiles();if(displayed_target_==DiscTarget::Avchd&&selected.size()!=1){QMessageBox::information(this,"AVCHD export","Select exactly one video for an AVCHD project.");return;}const bool create_button=add_button->isChecked();for(const auto& path:selected)add_title(path,create_button);refresh_all();}
    void add_title(const QString& path,bool create_button=true){Title t=make_title_from_defaults(project_defaults_);t.name=QFileInfo(path).completeBaseName().toStdString();t.source=path.toStdString();append_title_row(t);const int r=titles_->rowCount()-1;if(create_button&&!root_menu_.id.empty()){auto* m=current_menu();if(!m)m=&root_menu_;MenuButton b;b.label=t.name;b.target_kind=MenuButtonTargetKind::Title;b.target_title=static_cast<std::uint16_t>(r+1);b.bounds=clamp_rect_to_aperture(default_bounds(m->buttons.size()),stored_menu_aspect(displayed_target_));invalidate_reversible_aspect_positions();m->buttons.push_back(std::move(b));}mark_modified();commit_history();}
    EncodingProfile stored_row_profile(int r,DiscTarget target)const{const auto* item=titles_->item(r,1);const EncodingProfile fallback=default_profile_for_target(target);if(!item)return fallback;return encoding_from_map(item->data(profile_role(target)),fallback);}
    EncodingProfile row_profile(int r)const{auto*v=qobject_cast<QComboBox*>(titles_->cellWidget(r,2));auto*b=qobject_cast<QSpinBox*>(titles_->cellWidget(r,3));auto*preset=qobject_cast<QComboBox*>(titles_->cellWidget(r,4));auto*two=qobject_cast<QCheckBox*>(titles_->cellWidget(r,5));auto*a=qobject_cast<QComboBox*>(titles_->cellWidget(r,6));auto*ab=qobject_cast<QSpinBox*>(titles_->cellWidget(r,7));auto*k=qobject_cast<QSpinBox*>(titles_->cellWidget(r,13));auto*minrate=qobject_cast<QSpinBox*>(titles_->cellWidget(r,17));auto*maxrate=qobject_cast<QSpinBox*>(titles_->cellWidget(r,18));if(!v||!b||!preset||!two||!a||!ab||!k||!minrate||!maxrate)throw std::runtime_error("internal title encoding-control error");const auto base=stored_row_profile(r,displayed_target_);return profile_from_widgets(v,b,preset,two,a,ab,&base,k,minrate,maxrate);}
    std::string stored_row_frame_rate(int r,DiscTarget target)const{const auto* item=titles_->item(r,1);if(!item)return "inherit";const auto v=item->data(frame_rate_role(target)).toString().toStdString();return v.empty()?"inherit":v;}
    std::string stored_row_resolution(int r,DiscTarget target)const{const auto* item=titles_->item(r,1);if(!item)return "auto";const auto v=item->data(resolution_role(target)).toString().toStdString();return v.empty()?"auto":v;}
    std::string stored_row_aspect(int r,DiscTarget target)const{const auto* item=titles_->item(r,1);if(!item)return "auto";const auto v=item->data(aspect_role(target)).toString().toStdString();return v.empty()?"auto":v;}
    std::string row_frame_rate(int r)const{auto* rate=qobject_cast<QComboBox*>(titles_->cellWidget(r,9));return rate?rate->currentData().toString().toStdString():"inherit";}
    std::string row_resolution(int r)const{auto* combo=qobject_cast<QComboBox*>(titles_->cellWidget(r,11));return combo?combo->currentData().toString().toStdString():"auto";}
    std::string row_aspect(int r)const{auto* combo=qobject_cast<QComboBox*>(titles_->cellWidget(r,12));return combo?combo->currentData().toString().toStdString():"auto";}
    void store_visible_title_profiles(DiscTarget target){for(int r=0;r<titles_->rowCount();++r)if(auto* item=titles_->item(r,1)){item->setData(profile_role(target),encoding_map(row_profile(r)));item->setData(frame_rate_role(target),QString::fromStdString(row_frame_rate(r)));item->setData(resolution_role(target),QString::fromStdString(row_resolution(r)));item->setData(aspect_role(target),QString::fromStdString(row_aspect(r)));}}
    void set_row_profile_widgets(int r,const EncodingProfile& e,DiscTarget target){auto*v=qobject_cast<QComboBox*>(titles_->cellWidget(r,2));auto*b=qobject_cast<QSpinBox*>(titles_->cellWidget(r,3));auto*preset=qobject_cast<QComboBox*>(titles_->cellWidget(r,4));auto*two=qobject_cast<QCheckBox*>(titles_->cellWidget(r,5));auto*a=qobject_cast<QComboBox*>(titles_->cellWidget(r,6));auto*ab=qobject_cast<QSpinBox*>(titles_->cellWidget(r,7));auto*k=qobject_cast<QSpinBox*>(titles_->cellWidget(r,13));auto*minrate=qobject_cast<QSpinBox*>(titles_->cellWidget(r,17));auto*maxrate=qobject_cast<QSpinBox*>(titles_->cellWidget(r,18));auto*rate=qobject_cast<QComboBox*>(titles_->cellWidget(r,9));if(!v||!b||!preset||!two||!a||!ab||!k||!minrate||!maxrate)return;const QSignalBlocker bv(v),bb(b),bp(preset),bt(two),ba(a),bab(ab),bk(k),bmin(minrate),bmax(maxrate);auto* fast=qobject_cast<QCheckBox*>(two->property("fastFirstPassControl").value<QObject*>());const QSignalBlocker bf(fast);if(fast)fast->setChecked(e.fast_first_pass);populate_video_combo(v,target,e.video_codec);configure_video_bitrate_spin(b,target,e.video_codec,e.video_bitrate_kbps,authoring_limits_.allow_exceeding_format_limits);configure_video_minrate_spin(minrate,target,e.video_codec,e.video_min_bitrate_kbps,authoring_limits_.allow_exceeding_format_limits);configure_video_maxrate_spin(maxrate,target,e.video_codec,e.video_max_bitrate_kbps,authoring_limits_.allow_exceeding_format_limits);populate_preset_combo(preset,e.video_codec,encoding_preset(e));configure_pass_control(two,e.video_codec,e.two_pass);if(fast)fast->setEnabled(e.two_pass&&(e.video_codec==VideoCodec::X264||e.video_codec==VideoCodec::Hevc));auto rate_token=rate?rate->currentData().toString().toStdString():std::string("auto");if(rate_token=="inherit")rate_token=frame_rate_->currentData().toString().toStdString();configure_keyframe_interval_spin(k,target,e.video_codec,rate_token,e.keyframe_interval_frames);populate_audio_combo(a,target,e.audio_codec);configure_audio_bitrate_spin(ab,target,e.audio_codec,audio_bitrate_kbps(e),authoring_limits_.allow_exceeding_format_limits);}
    void load_visible_title_profiles(DiscTarget target){for(int r=0;r<titles_->rowCount();++r){if(auto* rate=qobject_cast<QComboBox*>(titles_->cellWidget(r,9)))populate_title_frame_rate_choices(rate,target,stored_row_frame_rate(r,target));set_row_profile_widgets(r,stored_row_profile(r,target),target);if(auto* resolution=qobject_cast<QComboBox*>(titles_->cellWidget(r,11)))populate_resolution_choices(resolution,target,stored_row_resolution(r,target),false);if(auto* aspect=qobject_cast<QComboBox*>(titles_->cellWidget(r,12)))populate_aspect_choices(aspect,target,stored_row_aspect(r,target),true,stored_row_resolution(r,target));}}

    void remove_selected_titles(){invalidate_reversible_aspect_positions();std::vector<int> rows;for(const auto& index:titles_->selectionModel()->selectedRows())rows.push_back(index.row());if(rows.empty()&&titles_->currentRow()>=0)rows.push_back(titles_->currentRow());std::sort(rows.begin(),rows.end(),std::greater<int>());rows.erase(std::unique(rows.begin(),rows.end()),rows.end());for(const int row:rows){if(row<0||row>=titles_->rowCount())continue;const unsigned n=static_cast<unsigned>(row+1);titles_->removeRow(row);remove_title_references(root_menu_,static_cast<std::uint16_t>(n));remove_title_references(first_play_actions_,static_cast<std::uint16_t>(n));}mark_modified();commit_history();refresh_all();}
    void add_submenu(){if(displayed_target_==DiscTarget::Avchd){QMessageBox::information(this,"AVCHD export","BDMV Author's AVCHD export is intentionally limited to a single menuless title. Switch to Blu-ray/UHD/DVD to author menus.");return;}if(root_menu_.id.empty()){Project p;apply_new_project_defaults(p,project_defaults_);root_menu_=p.menu;root_menu_.id="top";root_menu_.name="Top Menu";root_menu_.inherit_background_image=false;root_menu_.inherit_background_color=false;root_menu_.inherit_button_style=false;root_menu_.inherit_encoding=false;root_menu_.inherit_audio=false;root_menu_.inherit_duration=false;current_menu_id_="top";next_menu_id_=1;invalidate_reversible_aspect_positions();mark_modified();commit_history();refresh_all();load_menu_controls();statusBar()->showMessage("Main menu restored.",5000);return;}auto* parent=current_menu();if(!parent)parent=&root_menu_;Menu child;child.id="menu-"+std::to_string(next_menu_id_++);child.name="Submenu "+std::to_string(next_menu_id_-1);MenuButton link;link.label=child.name;link.target_kind=MenuButtonTargetKind::Menu;link.target_menu_id=child.id;link.auto_submenu_link=true;NavigationAction link_action;link_action.kind=NavigationActionKind::Menu;link_action.target_menu_id=child.id;set_single_button_action(link,std::move(link_action));link.bounds=clamp_rect_to_aperture(default_bounds(parent->buttons.size()),stored_menu_aspect(displayed_target_));invalidate_reversible_aspect_positions();parent->buttons.push_back(std::move(link));parent->submenus.push_back(std::move(child));mark_modified();commit_history();current_menu_id_=parent->submenus.back().id;rebuild_menu_tree();load_menu_controls();refresh_button_list();refresh_overlay_list();refresh_canvas();}
    static void collect_menu_ids(const Menu& m,std::vector<std::string>& ids){if(!m.id.empty())ids.push_back(m.id);for(const auto& c:m.submenus)collect_menu_ids(c,ids);}
    static void remove_menu_links(Menu& m,const std::vector<std::string>& ids){m.buttons.erase(std::remove_if(m.buttons.begin(),m.buttons.end(),[&](MenuButton& b){if(b.actions.empty())return b.target_kind==MenuButtonTargetKind::Menu&&std::find(ids.begin(),ids.end(),b.target_menu_id)!=ids.end();b.actions.erase(std::remove_if(b.actions.begin(),b.actions.end(),[&](const NavigationAction& a){return a.kind==NavigationActionKind::Menu&&std::find(ids.begin(),ids.end(),a.target_menu_id)!=ids.end();}),b.actions.end());if(b.actions.empty())return true;mirror_first_action_to_legacy_target(b);return false;}),m.buttons.end());for(auto& c:m.submenus)remove_menu_links(c,ids);}
    void remove_current_menu(){if(root_menu_.id.empty())return;invalidate_reversible_aspect_positions();auto* victim=current_menu();if(!victim)victim=&root_menu_;std::vector<std::string> ids;collect_menu_ids(*victim,ids);first_play_actions_.erase(std::remove_if(first_play_actions_.begin(),first_play_actions_.end(),[&](const NavigationAction& a){return a.kind==NavigationActionKind::Menu&&std::find(ids.begin(),ids.end(),a.target_menu_id)!=ids.end();}),first_play_actions_.end());if(victim==&root_menu_){root_menu_=Menu{};root_menu_.id.clear();root_menu_.name.clear();root_menu_.submenus.clear();current_menu_id_.clear();next_menu_id_=1;mark_modified();commit_history();refresh_all();load_menu_controls();statusBar()->showMessage("Main menu removed. By default all titles play in order and playback stops at the end.",6000);return;}auto* parent=parent_of(root_menu_,victim->id);if(!parent)return;const auto old=victim->id;parent->submenus.erase(std::remove_if(parent->submenus.begin(),parent->submenus.end(),[&](const Menu& m){return m.id==old;}),parent->submenus.end());remove_menu_links(root_menu_,ids);current_menu_id_=parent->id;mark_modified();commit_history();refresh_all();load_menu_controls();}
    void add_title_button(){auto* m=current_menu();if(!m||titles_->rowCount()==0)return;const auto targets=title_target_choices();if(targets.empty())return;QStringList choices;for(const auto& t:targets)choices<<t.label;bool ok=false;const auto picked=QInputDialog::getItem(this,"Add title/chapter button","Target",choices,0,false,&ok);if(!ok)return;const qsizetype idx=choices.indexOf(picked);if(idx<0)return;const auto& t=targets[static_cast<std::size_t>(idx)];MenuButton b;b.label=(t.suggested_label.isEmpty()?t.label:t.suggested_label).toStdString();b.target_kind=MenuButtonTargetKind::Title;b.target_title=t.title;b.target_chapter=t.chapter;set_single_button_action(b,action_from_choice(t));b.bounds=clamp_rect_to_aperture(default_bounds(m->buttons.size()),stored_menu_aspect(displayed_target_));invalidate_reversible_aspect_positions();m->buttons.push_back(std::move(b));mark_modified();commit_history();refresh_button_list();refresh_canvas();}
    void add_stream_button(){auto* m=current_menu();if(!m)return;const auto targets=stream_target_choices();if(targets.empty()){QMessageBox::information(this,"Audio/subtitle button","No selectable audio or subtitle tracks were found. BDMV Author uses ffprobe to inspect the source titles; supported subtitle sources are PGS and common text subtitle formats.");return;}QStringList choices;for(const auto& t:targets)choices<<t.label;bool ok=false;const auto picked=QInputDialog::getItem(this,"Add audio/subtitle button","Action",choices,0,false,&ok);if(!ok)return;const qsizetype idx=choices.indexOf(picked);if(idx<0)return;const auto& t=targets[static_cast<std::size_t>(idx)];MenuButton b;b.label=(t.suggested_label.isEmpty()?t.label:t.suggested_label).toStdString();b.target_kind=t.kind;b.target_title=t.title;b.target_chapter=t.chapter;b.target_stream=t.stream;b.target_menu_id=t.menu_id;set_single_button_action(b,action_from_choice(t));b.bounds=clamp_rect_to_aperture(default_bounds(m->buttons.size()),stored_menu_aspect(displayed_target_));invalidate_reversible_aspect_positions();m->buttons.push_back(std::move(b));mark_modified();commit_history();refresh_button_list();refresh_canvas();}
    void add_menu_link(){auto* m=current_menu();if(!m)return;std::vector<const Menu*> menus;std::function<void(const Menu&)> add=[&](const Menu& x){if(x.id!=m->id)menus.push_back(&x);for(const auto& c:x.submenus)add(c);};add(root_menu_);if(menus.empty())return;QStringList choices;for(const auto* x:menus)choices<<QString::fromStdString(x->name+" ["+x->id+"]");bool ok=false;const auto picked=QInputDialog::getItem(this,"Add menu link","Target menu",choices,0,false,&ok);if(!ok)return;const qsizetype idx=choices.indexOf(picked);if(idx<0)return;MenuButton b;b.label=menus[static_cast<std::size_t>(idx)]->name;b.target_kind=MenuButtonTargetKind::Menu;b.target_menu_id=menus[static_cast<std::size_t>(idx)]->id;NavigationAction a;a.kind=NavigationActionKind::Menu;a.target_menu_id=b.target_menu_id;set_single_button_action(b,std::move(a));b.bounds=clamp_rect_to_aperture(default_bounds(m->buttons.size()),stored_menu_aspect(displayed_target_));invalidate_reversible_aspect_positions();m->buttons.push_back(std::move(b));mark_modified();commit_history();refresh_button_list();refresh_canvas();}
    void add_text_label(){auto* m=current_menu();if(!m)return;MenuOverlay o;o.kind=MenuOverlayKind::Text;o.text="Text label";o.bounds={320,240+static_cast<int>(m->overlays.size())*200,1440,160};o.font_family=project_defaults_.label_font_family;o.font_size_px=project_defaults_.label_font_size_px;o.bold=project_defaults_.label_bold;o.italic=project_defaults_.label_italic;OverlayPropertiesDialog d(this,o);if(d.exec()!=QDialog::Accepted)return;{auto value=d.value();value.bounds=clamp_rect_to_aperture(value.bounds,stored_menu_aspect(displayed_target_));invalidate_reversible_aspect_positions();m->overlays.push_back(std::move(value));}mark_modified();commit_history();refresh_overlay_list();refresh_canvas();}
    void add_image_label(){auto* m=current_menu();if(!m)return;const auto f=QFileDialog::getOpenFileName(this,"Menu label image");if(f.isEmpty())return;MenuOverlay o;o.kind=MenuOverlayKind::Image;o.image=f.toStdString();o.bounds={320,240+static_cast<int>(m->overlays.size())*240,960,240};OverlayPropertiesDialog d(this,o);if(d.exec()!=QDialog::Accepted)return;{auto value=d.value();value.bounds=clamp_rect_to_aperture(value.bounds,stored_menu_aspect(displayed_target_));invalidate_reversible_aspect_positions();m->overlays.push_back(std::move(value));}mark_modified();commit_history();refresh_overlay_list();refresh_canvas();}
    void edit_selected_label(){auto* m=current_menu();const int row=overlay_list_->currentRow();if(!m||row<0||static_cast<std::size_t>(row)>=m->overlays.size())return;OverlayPropertiesDialog d(this,m->overlays[static_cast<std::size_t>(row)]);if(d.exec()!=QDialog::Accepted)return;{auto value=d.value();value.bounds=clamp_rect_to_aperture(value.bounds,stored_menu_aspect(displayed_target_));invalidate_reversible_aspect_positions();m->overlays[static_cast<std::size_t>(row)]=std::move(value);}mark_modified();commit_history();refresh_overlay_list();refresh_canvas();}
    void remove_selected_label(){auto* m=current_menu();const int row=overlay_list_->currentRow();if(!m||row<0||static_cast<std::size_t>(row)>=m->overlays.size())return;invalidate_reversible_aspect_positions();m->overlays.erase(m->overlays.begin()+row);mark_modified();commit_history();refresh_overlay_list();refresh_canvas();}
    void edit_selected_button(){auto* m=current_menu();const int row=button_list_->currentRow();if(!m||row<0||static_cast<std::size_t>(row)>=m->buttons.size())return;const auto e=effective_menu(*m);ButtonPropertiesDialog d(this,m->buttons[static_cast<std::size_t>(row)],e.default_button_style,target_choices());if(d.exec()!=QDialog::Accepted)return;{auto value=d.value();value.bounds=clamp_rect_to_aperture(value.bounds,stored_menu_aspect(displayed_target_));invalidate_reversible_aspect_positions();m->buttons[static_cast<std::size_t>(row)]=std::move(value);}mark_modified();commit_history();refresh_button_list();refresh_canvas();}
    void remove_selected_button(){auto* m=current_menu();const int row=button_list_->currentRow();if(!m||row<0||static_cast<std::size_t>(row)>=m->buttons.size())return;invalidate_reversible_aspect_positions();m->buttons.erase(m->buttons.begin()+row);mark_modified();commit_history();refresh_button_list();refresh_canvas();}
    void edit_back_button(){auto* m=current_menu();auto* parent=parent_of(root_menu_,current_menu_id_);if(!m||!parent)return;const auto e=effective_menu(*m);TargetChoice t;t.label="Parent menu — "+QString::fromStdString(parent->name);t.kind=MenuButtonTargetKind::Menu;t.menu_id=parent->id;auto initial=m->back_button;set_single_button_action(initial,action_from_choice(t));ButtonPropertiesDialog d(this,initial,e.default_button_style,{t},true);if(d.exec()!=QDialog::Accepted)return;m->back_button=d.value();m->back_button.bounds=clamp_rect_to_aperture(m->back_button.bounds,stored_menu_aspect(displayed_target_));invalidate_reversible_aspect_positions();set_single_button_action(m->back_button,action_from_choice(t));mark_modified();commit_history();refresh_canvas();}
    void edit_startup_sequence(){const bool menuless=root_menu_.id.empty();auto initial=first_play_actions_;if(menuless&&initial.empty())initial=default_menuless_startup_actions(static_cast<std::size_t>(titles_->rowCount()));ActionSequenceDialog d(this,"Disc startup / First Playback sequence",initial,target_choices());if(d.exec()!=QDialog::Accepted)return;first_play_actions_=d.value();if(menuless&&is_default_menuless_startup_sequence(first_play_actions_,static_cast<std::size_t>(titles_->rowCount())))first_play_actions_.clear();mark_modified();commit_history();statusBar()->showMessage(menuless?(first_play_actions_.empty()?"Menu-less default: play all titles in order, then stop.":"Menu-less startup sequence updated; playback stops when the sequence ends."):(first_play_actions_.empty()?"Disc will start at the top menu.":"Disc startup sequence updated."),5000);}
    Project snapshot_project() {
        store_visible_menu_mode();Project p;p.volume_label=volume_->text().toStdString();p.output_image=output_->text().toStdString();p.menu=root_menu_;p.target=displayed_target_;p.frame_rate=frame_rate_->currentData().toString().toStdString();p.menu_resolution=menu_hd_resolution_;p.menu_uhd_resolution=menu_uhd_resolution_;p.menu_dvd_resolution=menu_dvd_resolution_;p.menu_aspect_ratio=menu_hd_aspect_;p.menu_uhd_aspect_ratio=menu_uhd_aspect_;p.menu_dvd_aspect_ratio=menu_dvd_aspect_;p.disc_capacity_bytes=selected_disc_capacity_bytes();p.automatic_video_bitrate=automatic_bitrate_?automatic_bitrate_->isChecked():true;p.use_encode_cache=cache_enabled_state_;p.use_compliance_cache=compliance_cache_enabled_state_;p.refresh_encode_cache=refresh_encode_cache_state_;p.refresh_compliance_cache=refresh_compliance_cache_state_;p.force_reencode=false;p.first_play_actions=first_play_actions_;
        for(int r=0;r<titles_->rowCount();++r){Title t;t.name=titles_->item(r,0)->text().toStdString();t.source=titles_->item(r,1)->text().toStdString();t.encoding=stored_row_profile(r,DiscTarget::BluRay1080);t.uhd_encoding=stored_row_profile(r,DiscTarget::UltraHdBluRay2160);t.avchd_encoding=stored_row_profile(r,DiscTarget::Avchd);t.dvd_encoding=stored_row_profile(r,DiscTarget::DvdVideo480p);t.frame_rate=stored_row_frame_rate(r,DiscTarget::BluRay1080);t.uhd_frame_rate=stored_row_frame_rate(r,DiscTarget::UltraHdBluRay2160);t.avchd_frame_rate=stored_row_frame_rate(r,DiscTarget::Avchd);t.dvd_frame_rate=stored_row_frame_rate(r,DiscTarget::DvdVideo480p);t.resolution=stored_row_resolution(r,DiscTarget::BluRay1080);t.uhd_resolution=stored_row_resolution(r,DiscTarget::UltraHdBluRay2160);t.avchd_resolution=stored_row_resolution(r,DiscTarget::Avchd);t.dvd_resolution=stored_row_resolution(r,DiscTarget::DvdVideo480p);t.aspect_ratio=stored_row_aspect(r,DiscTarget::BluRay1080);t.uhd_aspect_ratio=stored_row_aspect(r,DiscTarget::UltraHdBluRay2160);t.avchd_aspect_ratio=stored_row_aspect(r,DiscTarget::Avchd);t.dvd_aspect_ratio=stored_row_aspect(r,DiscTarget::DvdVideo480p);encoding_for_target(t,displayed_target_)=row_profile(r);frame_rate_for_target(t,displayed_target_)=row_frame_rate(r);resolution_for_target(t,displayed_target_)=row_resolution(r);aspect_ratio_for_target(t,displayed_target_)=row_aspect(r);if(auto* force=qobject_cast<QCheckBox*>(titles_->cellWidget(r,10)))t.force_reencode=force->isChecked();if(const auto* item=titles_->item(r,1)){t.audio_language=item->data(RoleAudioLanguage).toString().toStdString();if(t.audio_language.empty())t.audio_language="eng";t.chapter_mode=static_cast<ChapterMode>(item->data(RoleChapterMode).toInt());t.chapter_interval_seconds=item->data(RoleChapterInterval).toDouble();if(t.chapter_interval_seconds<=0.0)t.chapter_interval_seconds=300.0;for(const auto& v:item->data(RoleChapters).toList())t.chapters_seconds.push_back(v.toDouble());t.menu_button_actions=navigation_actions_from_variant(item->data(RoleTitleMenuActions));t.prohibited_user_operations=item->data(RoleTitleUopMask).toULongLong();t.audio_stream_settings=audio_stream_settings_from_variant(item->data(RoleAudioStreamSettings));t.subtitle_stream_settings=subtitle_stream_settings_from_variant(item->data(RoleSubtitleStreamSettings));t.external_subtitles=external_subtitles_from_variant(item->data(RoleExternalSubtitles));t.subtitle_default_style=subtitle_style_from_map(item->data(RoleSubtitleDefaultStyle));t.default_audio_stream=item->data(RoleDefaultAudioStream).toInt();t.default_subtitle_stream=item->data(RoleDefaultSubtitleStream).isValid()?item->data(RoleDefaultSubtitleStream).toInt():-1;t.encoding_worker=item->data(RoleTitleEncodingWorker).toString().toStdString();t.encoding_worker_mode=static_cast<EncodingWorkerAffinityMode>(item->data(RoleTitleEncodingWorkerMode).isValid()?item->data(RoleTitleEncodingWorkerMode).toInt():static_cast<int>(EncodingWorkerAffinityMode::Automatic));}p.titles.push_back(std::move(t));}
        return p;
    }
    static int maximum_menu_number(const Menu& m){int best=0;if(m.id.rfind("menu-",0)==0){int parsed=0;if(parse_integer_exact(std::string_view(m.id).substr(5),parsed))best=parsed;}for(const auto& c:m.submenus)best=std::max(best,maximum_menu_number(c));return best;}
    void edit_new_project_defaults(){DefaultsDialog d(this,saved_defaults_);if(d.exec()!=QDialog::Accepted)return;saved_defaults_=d.value();save_new_project_defaults(saved_defaults_);statusBar()->showMessage("Defaults saved; they will be used by newly created projects.",5000);}
    void new_project(){
        if(!maybe_save_modified())return;
        suppress_modified_=true;project_defaults_=saved_defaults_;Project p;apply_new_project_defaults(p,project_defaults_);if(target_is_avchd(p.target)){p.menu=Menu{};p.menu.id.clear();p.menu.name.clear();}else{p.menu.id="top";p.menu.name="Top Menu";p.menu.inherit_background_image=false;p.menu.inherit_background_color=false;p.menu.inherit_button_style=false;p.menu.inherit_encoding=false;p.menu.inherit_audio=false;p.menu.inherit_duration=false;}loading_menu_=true;titles_->setRowCount(0);volume_->setText(QString::fromStdString(p.volume_label));output_->clear();cache_enabled_state_=p.use_encode_cache;compliance_cache_enabled_state_=p.use_compliance_cache;refresh_encode_cache_state_=false;refresh_compliance_cache_state_=false;populate_size_target_choices(p.target,p.disc_capacity_bytes);automatic_bitrate_->setChecked(p.automatic_video_bitrate);load_menu_mode_state(p);displayed_target_=p.target;{const QSignalBlocker blocker(target_);target_->setCurrentIndex(target_->findData(static_cast<int>(displayed_target_)));}populate_frame_rate_choices(p.frame_rate);populate_resolution_choices(menu_resolution_,displayed_target_,stored_menu_resolution(displayed_target_),true);populate_aspect_choices(menu_aspect_,displayed_target_,stored_menu_aspect(displayed_target_),false,stored_menu_resolution(displayed_target_));update_build_button_text();root_menu_=p.menu;initialize_editor_bounds_for_loaded_project();first_play_actions_.clear();current_menu_id_=root_menu_.id.empty()?std::string{}:std::string("top");next_menu_id_=1;project_file_.clear();loading_menu_=false;progress_->setValue(0);progress_->setFormat("0.00%");rebuild_menu_tree();load_menu_controls();refresh_all();menu_resolution_->setEnabled(displayed_target_!=DiscTarget::Avchd);menu_aspect_->setEnabled(displayed_target_!=DiscTarget::Avchd);if(add_menu_button_)add_menu_button_->setEnabled(displayed_target_!=DiscTarget::Avchd);suppress_modified_=false;reset_history();mark_clean();refresh_external_tool_availability();
    }
    void populate_project(const Project& p){
        suppress_modified_=true;project_defaults_=saved_defaults_;loading_menu_=true;volume_->setText(QString::fromStdString(p.volume_label));output_->setText(QString::fromStdString(p.output_image.string()));cache_enabled_state_=p.use_encode_cache;compliance_cache_enabled_state_=p.use_compliance_cache;refresh_encode_cache_state_=false;refresh_compliance_cache_state_=false;populate_size_target_choices(p.target,p.disc_capacity_bytes);automatic_bitrate_->setChecked(p.automatic_video_bitrate);load_menu_mode_state(p);displayed_target_=p.target;{const QSignalBlocker blocker(target_);target_->setCurrentIndex(target_->findData(static_cast<int>(displayed_target_)));}populate_frame_rate_choices(p.frame_rate);populate_resolution_choices(menu_resolution_,displayed_target_,stored_menu_resolution(displayed_target_),true);populate_aspect_choices(menu_aspect_,displayed_target_,stored_menu_aspect(displayed_target_),false,stored_menu_resolution(displayed_target_));update_build_button_text();titles_->setRowCount(0);for(const auto& t:p.titles)append_title_row(t);root_menu_=p.menu;concretize_gui_menu_fonts(root_menu_);initialize_editor_bounds_for_loaded_project();first_play_actions_=p.first_play_actions;current_menu_id_=root_menu_.id.empty()?std::string{}:root_menu_.id;next_menu_id_=maximum_menu_number(root_menu_)+1;loading_menu_=false;rebuild_menu_tree();load_menu_controls();refresh_all();menu_resolution_->setEnabled(displayed_target_!=DiscTarget::Avchd);menu_aspect_->setEnabled(displayed_target_!=DiscTarget::Avchd);if(add_menu_button_)add_menu_button_->setEnabled(displayed_target_!=DiscTarget::Avchd);suppress_modified_=false;refresh_external_tool_availability();
    }
    bool save_project(bool save_as){
        QString path=project_file_;if(save_as||path.isEmpty())path=QFileDialog::getSaveFileName(this,"Save BDMV Author project",path.isEmpty()?QString("project.bdmvproject"):path,"BDMV Author project (*.bdmvproject)");if(path.isEmpty())return false;if(!path.endsWith(".bdmvproject",Qt::CaseInsensitive))path+=".bdmvproject";
        const auto persistence=load_tool_configuration().worker_affinity_persistence;Project snapshot=snapshot_project();Project disk=snapshot;if(persistence!="project")clear_project_worker_affinities(disk);QString error;if(!save_project_file(this,disk,path,&error)){QMessageBox::critical(this,"Could not save project",error);return false;}if(persistence=="local"){if(!save_local_worker_affinities(snapshot,path,&error)){QMessageBox::warning(this,"Worker assignments",error);}}else remove_local_worker_affinities(path);project_file_=path;mark_clean();statusBar()->showMessage(QString("Saved project %1").arg(path),5000);return true;
    }
    void open_project(){
        const QString path=QFileDialog::getOpenFileName(this,"Open BDMV Author project",project_file_,"BDMV Author project (*.bdmvproject);;JSON files (*.json);;All files (*)");if(path.isEmpty())return;if(!maybe_save_modified())return;QString error;bool font_replacements=false;auto p=load_project_file(this,path,&error,&font_replacements);if(!p){if(!error.isEmpty())QMessageBox::critical(this,"Could not open project",error);return;}const auto persistence=load_tool_configuration().worker_affinity_persistence;if(persistence!="project")clear_project_worker_affinities(*p);if(persistence=="local")apply_local_worker_affinities(*p,path);populate_project(*p);project_file_=path;if(font_replacements){statusBar()->showMessage(QString("Opened project %1 with replacement fonts; save the project to keep those replacements.").arg(path),8000);}else{mark_clean();statusBar()->showMessage(QString("Opened project %1").arg(path),5000);}reset_history();if(font_replacements){saved_history_index_=-1;project_modified_=true;update_window_title();}
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
        if(!encode_progress_dialog_)encode_progress_dialog_=std::make_unique<EncodeProgressDialog>(this);encode_progress_dialog_->reset();encoding_details_->setEnabled(true);encode_progress_dialog_->show();encode_progress_dialog_->raise();encode_progress_dialog_->activateWindow();
        close_after_render_=false;
        cancel_requested_=std::make_shared<std::atomic_bool>(false);
        const auto cancel_token=cancel_requested_;
        watcher_=std::make_unique<QFutureWatcher<BuildResult>>();
        connect(watcher_.get(),&QFutureWatcher<BuildResult>::finished,this,[this,build_target]{
            const BuildResult result=watcher_->result();
            build_->setEnabled(target_available(displayed_target_));cancel_->setEnabled(false);cancel_requested_.reset();
            if(result.cancelled){progress_->setFormat("Cancelled");if(encode_progress_dialog_)encode_progress_dialog_->finish("cancelled");statusBar()->showMessage("Render cancelled; temporary files cleaned up.",5000);}
            else if(result.error.isEmpty()){progress_->setValue(10000);progress_->setFormat("100.00%");if(encode_progress_dialog_)encode_progress_dialog_->finish("completed");refresh_encode_cache_state_=false;refresh_compliance_cache_state_=false;QMessageBox::information(this,"BDMV Author",QStringLiteral("%1 image created.").arg(output_target_name(build_target)));
                if(!result.filter_warnings.isEmpty() && !result.filter_warning_shown_during_build)
                    QMessageBox::warning(this,"BDMV Author — hardware filter fallback",result.filter_warnings.join("\n\n---\n\n"));}
            else{if(encode_progress_dialog_)encode_progress_dialog_->finish("failed");QMessageBox::critical(this,"BDMV Author",result.error);}
            if(close_after_render_){close_after_render_=false;QTimer::singleShot(0,this,[this]{close();});}
        });
        auto limits=authoring_limits_;
        const auto storage=gui_storage_paths();
        auto future=QtConcurrent::run([this,p,tools,limits,storage,cancel_token]()->BuildResult{
            try{
                ToolPaths engine_tools=tools;
                auto warnings=std::make_shared<QStringList>();
                auto warning_mutex=std::make_shared<std::mutex>();
                auto warning_shown=std::make_shared<std::atomic_bool>(false);
                engine_tools.filter_fallback_warning=[this,warnings,warning_mutex,warning_shown](const std::string& message){
                    {
                        std::lock_guard<std::mutex> lock(*warning_mutex);
                        warnings->append(QString::fromStdString(message));
                    }
                    // Show the first hardware failure as soon as it occurs,
                    // not after the DVD (possibly a long two-pass encode) is done.
                    if(!warning_shown->exchange(true)) {
                        const QString diagnostic=QString::fromStdString(message);
                        QMetaObject::invokeMethod(this,[this,diagnostic]{
                            QMessageBox::warning(this,"BDMV Author — hardware filter fallback",diagnostic);
                        },Qt::QueuedConnection);
                    }
                };
                AuthorEngine e(engine_tools,limits,storage);
                e.author(p,[this,cancel_token](double v,const std::string&m){
                    if(cancel_token->load(std::memory_order_acquire))return;
                    if(m.rfind("NONCOMPLIANT:",0)==0)std::cerr<<"["<<std::fixed<<std::setprecision(2)<<v<<"%] "<<m<<'\n'<<std::flush;
                    QMetaObject::invokeMethod(this,[this,v,m,cancel_token]{if(cancel_token->load(std::memory_order_acquire))return;progress_->setValue(static_cast<int>(std::lround(v*100.0)));progress_->setFormat(QString::number(v,'f',2)+"%");statusBar()->showMessage(QString::fromStdString(m));},Qt::QueuedConnection);
                },[cancel_token]{return cancel_token->load(std::memory_order_acquire);},[this,cancel_token](const EncodeJobProgress& ev){
                    if(cancel_token->load(std::memory_order_acquire))return;
                    QMetaObject::invokeMethod(this,[this,ev,cancel_token]{if(cancel_token->load(std::memory_order_acquire)||!encode_progress_dialog_)return;encode_progress_dialog_->update_encode(ev);},Qt::QueuedConnection);
                });
                BuildResult result;
                {std::lock_guard<std::mutex> lock(*warning_mutex);result.filter_warnings=*warnings;}
                result.filter_warning_shown_during_build=warning_shown->load();
                return result;
            }catch(const AuthorCancelled&){BuildResult r;r.cancelled=true;return r;}
            catch(const std::exception&e){BuildResult r;if(cancel_token->load(std::memory_order_acquire))r.cancelled=true;
                else r.error=QString::fromStdString(e.what());return r;}
        });
        watcher_->setFuture(future);
    }

};

int main(int argc,char**argv){bool debug_mode=false;for(int i=1;i<argc;++i)if(std::string_view(argv[i])=="--debug")debug_mode=true;QApplication app(argc,argv);QCoreApplication::setOrganizationName("BDMV Author");QCoreApplication::setApplicationName("BDMV Author");QGuiApplication::setApplicationDisplayName(QStringLiteral("BDMV Author"));QGuiApplication::setDesktopFileName(QStringLiteral("bdmvauthor"));const QIcon icon=bdmvauthor_app_icon();QApplication::setWindowIcon(icon);try{g_tool_capabilities=probe_tool_capabilities(load_tool_configuration());Window w(debug_mode);w.setWindowIcon(icon);w.show();return app.exec();}catch(const std::exception&e){QMessageBox::critical(nullptr,"BDMV Author startup error",QString::fromStdString(e.what()));return 1;}}
