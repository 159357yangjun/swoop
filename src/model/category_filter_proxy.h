#ifndef CATEGORY_FILTER_PROXY_H
#define CATEGORY_FILTER_PROXY_H

#include <QSortFilterProxyModel>
#include <QSet>
#include "task_list_model.h"

// 左侧分类树对应的过滤类别
enum class Category { All, Downloading, Completed, Failed, Cancelled };

// 文件类型（与 IDM 左侧树对应）
enum class FileType { All, Compressed, Document, Music, Program, Video, Other };

// 按任务状态 + 文件类型 + 队列过滤的代理模型
class CategoryFilterProxy : public QSortFilterProxyModel {
    Q_OBJECT
public:
    explicit CategoryFilterProxy(QObject* parent = nullptr)
        : QSortFilterProxyModel(parent), m_cat(Category::All), m_fileType(FileType::All) {}

    void setCategory(Category c) { m_cat = c; invalidate(); }
    void setFileType(FileType ft) { m_fileType = ft; invalidate(); }
    void setQueueFilter(const QString& queue) { m_queue = queue; invalidate(); }

    // 根据文件名后缀判断文件类型
    static FileType detectFileType(const QString& fileName)
    {
        QString ext = fileName.section(QLatin1Char('.'), -1).toLower();
        if (ext.isEmpty()) return FileType::Other;

        static const QSet<QString> compressed = {
            QStringLiteral("zip"), QStringLiteral("rar"), QStringLiteral("7z"),
            QStringLiteral("tar"), QStringLiteral("gz"), QStringLiteral("bz2"),
            QStringLiteral("xz"), QStringLiteral("tgz"), QStringLiteral("bz"),
            QStringLiteral("cab"), QStringLiteral("iso")
        };
        static const QSet<QString> documents = {
            QStringLiteral("pdf"), QStringLiteral("doc"), QStringLiteral("docx"),
            QStringLiteral("txt"), QStringLiteral("rtf"), QStringLiteral("xls"),
            QStringLiteral("xlsx"), QStringLiteral("ppt"), QStringLiteral("pptx"),
            QStringLiteral("csv"), QStringLiteral("html"), QStringLiteral("htm")
        };
        static const QSet<QString> music = {
            QStringLiteral("mp3"), QStringLiteral("flac"), QStringLiteral("wav"),
            QStringLiteral("aac"), QStringLiteral("ogg"), QStringLiteral("wma"),
            QStringLiteral("m4a"), QStringLiteral("opus")
        };
        static const QSet<QString> program = {
            QStringLiteral("exe"), QStringLiteral("msi"), QStringLiteral("apk"),
            QStringLiteral("dmg"), QStringLiteral("pkg"), QStringLiteral("deb"),
            QStringLiteral("rpm"), QStringLiteral("appimage")
        };
        static const QSet<QString> video = {
            QStringLiteral("mp4"), QStringLiteral("mkv"), QStringLiteral("avi"),
            QStringLiteral("mov"), QStringLiteral("wmv"), QStringLiteral("flv"),
            QStringLiteral("webm"), QStringLiteral("ts"), QStringLiteral("m3u8")
        };

        if (compressed.contains(ext)) return FileType::Compressed;
        if (documents.contains(ext))   return FileType::Document;
        if (music.contains(ext))       return FileType::Music;
        if (program.contains(ext))     return FileType::Program;
        if (video.contains(ext))       return FileType::Video;
        return FileType::Other;
    }

    // 文件类型 → 自动归档子目录名（IDM 风格中文）
    static QString fileTypeFolderName(FileType ft)
    {
        switch (ft) {
            case FileType::Compressed: return QStringLiteral("压缩包");
            case FileType::Document:   return QStringLiteral("文档");
            case FileType::Music:      return QStringLiteral("音乐");
            case FileType::Program:    return QStringLiteral("程序");
            case FileType::Video:      return QStringLiteral("视频");
            default:                   return QStringLiteral("其他");
        }
    }

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override
    {
        QModelIndex idx = sourceModel()->index(sourceRow, 0, sourceParent);

        // 状态过滤
        if (m_cat != Category::All) {
            int state = sourceModel()->data(idx, TaskListModel::StateRole).toInt();
            switch (m_cat) {
                case Category::Downloading: if (!(state == 0 || state == 1 || state == 2)) return false; break;
                case Category::Completed:   if (state != 3) return false; break;
                case Category::Failed:     if (state != 4) return false; break;
                case Category::Cancelled:  if (state != 5) return false; break;
                default: break;
            }
        }

        // 文件类型过滤
        if (m_fileType != FileType::All) {
            QString fileName = sourceModel()->data(idx, Qt::DisplayRole).toString();
            if (detectFileType(fileName) != m_fileType)
                return false;
        }

        // 队列过滤
        if (!m_queue.isEmpty()) {
            QString q = sourceModel()->data(idx, TaskListModel::QueueRole).toString();
            if (q != m_queue)
                return false;
        }

        return true;
    }

private:
    Category m_cat;
    FileType m_fileType;
    QString  m_queue;
};

#endif // CATEGORY_FILTER_PROXY_H
