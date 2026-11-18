#include "backup/agent/source_tree.h"
#include "backup/agent/remote_session.h"
#include "backup/agent/source_sink.h"
#include "backup/agent/worker_support.h"
#include "backup/core/detail_page.h"
#include "backup/core/io.h"
#include "backup/core/manifest.h"
#include "backup/core/source_scope.h"
#include "source_entries.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <fcntl.h>
#include <filesystem>
#include <unistd.h>

namespace backup::agent
{
using namespace core;
namespace
{
void addSample(QJsonArray& sample, QJsonObject entry)
{
    if (entry.value("type") == "symlink" && entry.contains("target_base64"))
    {
        entry.insert("link_target", QString::fromUtf8(symlinkTarget(entry)));
        entry.remove("target_base64");
    }
    if (entry.contains("size"))
    {
        entry.insert("size", number(entry, "size"));
    }
    const auto fits = [&]
    {
        return sample.size() < kPageSize &&
               QJsonDocument(sample).toJson(QJsonDocument::Compact).size() +
                       detailLine(entry).size() <
                   kDetailPageBytes;
    };
    if (!entry.contains("reason"))
    {
        for (qsizetype index = sample.size(); index > 0 && !fits(); --index)
        {
            if (sample[index - 1].toObject().contains("reason"))
            {
                sample.removeAt(index - 1);
            }
        }
    }
    if (fits())
    {
        sample.append(entry);
    }
}
// 一次遍历拥有自己的统计、去重和目录快照；外部依赖只借用到 run 返回。
class SourceWalker
{
  public:
    SourceWalker(int root, const SourceScope& scope, ISourceSink& sink,
                 const Progress& progress, const struct stat& rootBefore,
                 const QString& rootPath)
        : root_(root), scope_(scope), sink_(sink), progress_(progress),
          rootBefore_(rootBefore), entries_(root, sink, progress),
          status_({{"stage", sink.stage()},
                   {"root", rootPath},
                   {"files", 0},
                   {"directories", 0},
                   {"bytes", 0},
                   {"stored_bytes", 0},
                   {"symlinks", 0},
                   {"hardlinks", 0},
                   {"fifos", 0},
                   {"character_devices", 0},
                   {"block_devices", 0},
                   {"sockets", 0},
                   {"complete", true},
                   {"error_count", 0}})
    {
    }

    QJsonObject run()
    {
        if (scope_.selection().isEmpty())
        {
            visit({});
        }
        else
        {
            for (const auto& value : scope_.selection())
            {
                const auto item = value.toObject();
                const auto path = text(item, "path");
                // 仅创建结构父目录，不扫描旁边未选择的条目。
                QString parent;
                const auto components = path.split('/');
                for (qsizetype index = 0; index + 1 < components.size();
                     ++index)
                {
                    parent = parent.isEmpty()
                                 ? components[index]
                                 : parent + '/' + components[index];
                    process(parent, "directory", true);
                }
                process(path, text(item, "type"), false);
            }
        }
        verify();
        status_.insert("sample", sample_);
        status_.insert("warnings", warnings_);
        status_.insert(
            "entries",
            number(status_, "files") + number(status_, "directories") +
                number(status_, "symlinks") + number(status_, "fifos") +
                number(status_, "character_devices") +
                number(status_, "block_devices") + number(status_, "sockets"));
        status_.insert("hashed_files",
                       number(status_, "files") - number(status_, "hardlinks"));
        status_.insert("unreadable_files", status_.value("error_count"));
        return status_;
    }

  private:
    void problem(const QString& path, const QString& reason)
    {
        sink_.problem(path, reason, status_, warnings_);
    }

    void includeParents(const QString& path)
    {
        QString parent;
        const auto components = path.split('/');
        for (qsizetype index = 0; index + 1 < components.size(); ++index)
        {
            parent = parent.isEmpty() ? components[index]
                                      : parent + '/' + components[index];
            if (deferredDirectories_.contains(parent))
            {
                const auto info = deferredDirectories_.take(parent);
                addSample(
                    sample_,
                    entries_.process(parent, info, status_,
                                     structuralDirectories_.contains(parent)));
            }
        }
    }

    void process(const QString& path, const QString& expected, bool structural)
    {
        checkOperationCancelled();
        if (processed_.contains(path))
        {
            return;
        }
        auto parentSlash = path.lastIndexOf('/');
        while (parentSlash >= 0)
        {
            if (excludedDirectories_.contains(path.left(parentSlash)))
            {
                return;
            }
            parentSlash = path.lastIndexOf('/', parentSlash - 1);
        }
        const auto previous = number(status_, "bytes");
        QString type = expected;
        try
        {
            const auto info = statBelow(root_, path);
            type = sourceEntryType(info.st_mode);
            require(expected.isEmpty() || type == expected,
                    "Selected source type changed: " + path);
            status_.insert("path", path);
            if (type == "directory" &&
                scope_.excludesDirectorySubtree(path, info.st_uid,
                                                info.st_mtim.tv_sec))
            {
                excludedDirectories_.insert(path);
                processed_.insert(path);
                progress_(status_);
                return;
            }
            if (type == "directory" &&
                (structural || (!scope_.includes("directory") ||
                                !scope_.matches(path, type, 0, info.st_uid,
                                                info.st_mtim.tv_sec))))
            {
                deferredDirectories_.insert(path, info);
                if (structural)
                {
                    structuralDirectories_.insert(path);
                }
                processed_.insert(path);
                if (!structural)
                {
                    visit(path);
                }
                progress_(status_);
                return;
            }
            if ((type == "character_device" || type == "block_device" ||
                 type == "socket") &&
                !scope_.includes(type) &&
                (type == "socket" || scope_.fileTypes().isEmpty() ||
                 scope_.hasEmptyDirectoryRule()))
            {
                const auto reason =
                    type == "socket"
                        ? "Socket node skipped; backing up socket nodes is "
                          "not supported"
                        : type + " node skipped; select this type to back it "
                                 "up";
                QJsonObject skipped{
                    {"path", path}, {"type", type}, {"reason", reason}};
                addSample(sample_, skipped);
                warnings_.append(skipped);
                processed_.insert(path);
                progress_(status_);
                return;
            }
            if (type != "directory" && !scope_.includes(type) &&
                (supportedEntryType(type) || !scope_.hasEmptyDirectoryRule()))
            {
                processed_.insert(path);
                return;
            }
            if (type != "directory")
            {
                const auto filterType =
                    type == "file" && info.st_nlink > 1 ? "hardlink" : type;
                if (!scope_.matches(path, filterType,
                                    type == "file" ? info.st_size : 0,
                                    info.st_uid, info.st_mtim.tv_sec))
                {
                    processed_.insert(path);
                    progress_(status_);
                    return;
                }
            }
            if (supportedEntryType(type))
            {
                includeParents(path);
            }
            const auto entry =
                entries_.process(path, info, status_, structural);
            processed_.insert(path);
            addSample(sample_, entry);
            if (entry.contains("reason"))
            {
                warnings_.append(entry);
            }
            if (S_ISDIR(info.st_mode) && !structural)
            {
                visit(path);
            }
            progress_(status_);
        }
        catch (const ChannelError&)
        {
            throw;
        }
        catch (const std::exception& failure)
        {
            checkOperationCancelled();
            // 上传接收者在此抛出；扫描接收者记录失败后继续处理其他条目。
            problem(path, QString::fromUtf8(failure.what()));
            status_.insert("bytes", previous);
            if (!type.isEmpty() && !processed_.contains(path))
            {
                addSample(sample_, {{"path", path}, {"type", type}});
            }
        }
    }

    void visit(const QString& relative)
    {
        checkOperationCancelled();
        try
        {
            Descriptor directory(
                relative.isEmpty()
                    ? ::dup(root_)
                    : openBelow(root_, relative, O_RDONLY | O_DIRECTORY));
            // 通过固定的目录 fd 枚举，防止路径被替换为软链接。
            const auto pinned =
                "/proc/self/fd/" + std::to_string(directory.get());
            std::error_code error;
            auto iterator = std::filesystem::directory_iterator(pinned, error);
            require(!error, QString::fromStdString(error.message()));
            while (iterator != std::filesystem::directory_iterator())
            {
                checkOperationCancelled();
                const auto native = iterator->path().filename().native();
                const auto name =
                    QString::fromUtf8(native.data(), native.size());
                const auto path =
                    relative.isEmpty() ? name : relative + '/' + name;
                try
                {
                    require(name.toUtf8().toStdString() == native &&
                                validPath(path),
                            "Unsupported filename encoding or path");
                    process(path, {}, false);
                }
                catch (const ChannelError&)
                {
                    throw;
                }
                catch (const std::exception& failure)
                {
                    checkOperationCancelled();
                    problem(path, QString::fromUtf8(failure.what()));
                }
                iterator.increment(error);
                require(!error, QString::fromStdString(error.message()));
            }
        }
        catch (const ChannelError&)
        {
            throw;
        }
        catch (const std::exception& failure)
        {
            checkOperationCancelled();
            problem(relative, QString::fromUtf8(failure.what()));
        }
    }

    void verify()
    {
        try
        {
            entries_.verify();
            for (auto item = deferredDirectories_.begin();
                 item != deferredDirectories_.end(); ++item)
            {
                checkOperationCancelled();
                const auto after = statBelow(root_, item.key());
                require(structuralDirectories_.contains(item.key())
                            ? sameSourceDirectoryState(item.value(), after)
                            : sameFileState(item.value(), after),
                        "Source directory changed during traversal: " +
                            item.key());
            }
            struct stat after
            {
            };
            require(::fstat(root_, &after) == 0 &&
                        (scope_.selection().isEmpty()
                             ? sameFileState(rootBefore_, after)
                             : sameSourceDirectoryState(rootBefore_, after)),
                    "Source directory changed during traversal");
        }
        catch (const ChannelError&)
        {
            throw;
        }
        catch (const std::exception& failure)
        {
            checkOperationCancelled();
            problem(".", QString::fromUtf8(failure.what()));
        }
    }

    int root_;
    const SourceScope& scope_;
    ISourceSink& sink_;
    const Progress& progress_;
    const struct stat rootBefore_;
    SourceEntries entries_;
    QJsonObject status_;
    QJsonArray sample_;
    QJsonArray warnings_;
    QSet<QString> processed_;
    QSet<QString> excludedDirectories_;
    QHash<QString, struct stat> deferredDirectories_;
    QSet<QString> structuralDirectories_;
};
} // namespace

QJsonObject sourceTree(const QJsonObject& source, ISourceSink& sink,
                       const Progress& progress)
{
    const SourceScope scope(source);
    require(!scope.fileTypes().contains("socket"),
            "Socket node backups are no longer supported; create a task "
            "without the socket type");
    const auto rootPath = canonicalPath(scope.root());
    require(rootPath == scope.root(),
            "Source root has changed: " + scope.root());
    Descriptor filesystem(::open("/", O_PATH | O_DIRECTORY | O_CLOEXEC));
    const int flags =
        (scope.selection().isEmpty() ? O_RDONLY : O_PATH) | O_DIRECTORY;
    // 逐级固定父目录且不跟随软链接；选定文件时无需列举逻辑根目录。
    Descriptor root(rootPath == "/"
                        ? ::open("/", flags | O_CLOEXEC)
                        : openBelow(filesystem.get(), rootPath.mid(1), flags));
    struct stat rootBefore
    {
    };
    require(::fstat(root.get(), &rootBefore) == 0, "Cannot inspect source");
    SourceWalker walker(root.get(), scope, sink, progress, rootBefore,
                        rootPath);
    return walker.run();
}
} // namespace backup::agent
