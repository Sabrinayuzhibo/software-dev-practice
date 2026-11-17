#include "backup/agent/metadata_restore.h"
#include "backup/agent/operation_dispatch.h"
#include "backup/agent/restore_node.h"
#include "backup/agent/source_sink.h"
#include "backup/agent/source_tree.h"
#include "backup/core/io.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QTemporaryDir>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <type_traits>
#include <unistd.h>

using namespace backup::agent;
using namespace backup::core;

namespace
{
void rejects(const std::function<void()>& operation)
{
    try
    {
        operation();
    }
    catch (const Error&)
    {
        return;
    }
    throw Error("Operation should have failed");
}

// 用真实 fd 和不跟随链接的路径调用验证属性；系统调用故障由集成套件注入。
void checkMetadata(const QString& directory)
{
    QFile file(directory + "/sample");
    require(file.open(QIODevice::ReadWrite) && file.write("content") == 7 &&
                file.flush(),
            "Cannot prepare metadata sample");
    const QJsonObject entry{{"path", "sample"},
                            {"mode", 0640},
                            {"uid", QString::number(::getuid())},
                            {"gid", QString::number(::getgid())},
                            {"mtime_sec", "-1"},
                            {"mtime_nsec", "123456789"}};
    QJsonObject status{{"metadata_warning_count", 0}};
    restoreDescriptorMetadata(file.handle(), entry, status);
    struct stat info
    {
    };
    require(
        ::fstat(file.handle(), &info) == 0 && (info.st_mode & 0777) == 0640 &&
            info.st_uid == ::getuid() && info.st_gid == ::getgid() &&
            info.st_mtim.tv_sec == -1 && info.st_mtim.tv_nsec == 123456789 &&
            number(status, "metadata_warning_count") == 0,
        "Descriptor metadata restore lost permissions, ownership or precision");
    Descriptor parent(::open(directory.toUtf8().constData(),
                             O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    require(::symlinkat("sample", parent.get(), "link") == 0,
            "Cannot prepare metadata link");
    auto linkEntry = entry;
    linkEntry.insert("path", "link");
    linkEntry.insert("type", "symlink");
    linkEntry.insert("mtime_sec", "-2");
    restorePathMetadata(parent.get(), "link", linkEntry, status);
    require(::fstatat(parent.get(), "link", &info, AT_SYMLINK_NOFOLLOW) == 0 &&
                S_ISLNK(info.st_mode) && (info.st_mode & 0777) == 0777 &&
                info.st_mtim.tv_sec == -2 &&
                info.st_mtim.tv_nsec == 123456789 &&
                number(status, "metadata_warning_count") == 0,
            "Path metadata restore changed link semantics");
    require(::fstat(file.handle(), &info) == 0 && info.st_mtim.tv_sec == -1,
            "Path metadata restore followed a symbolic link");

    QJsonObject failed{{"metadata_warning_count", 0}};
    restoreDescriptorMetadata(-1, entry, failed);
    const auto warnings = failed.value("warnings").toArray();
    require(number(failed, "metadata_warning_count") == 3 &&
                warnings.size() == 3 &&
                warnings[0].toObject().value("field") == "owner/group" &&
                warnings[1].toObject().value("field") == "permissions" &&
                warnings[2].toObject().value("field") == "modification time" &&
                warnings[2].toObject().value("path") == "sample",
            "Metadata restore did not retain all field failures");
    restoreDescriptorMetadata(-1, {{"path", "legacy"}}, failed);
    require(number(failed, "metadata_warning_count") == 3,
            "Legacy metadata should be skipped");
}

// 不建立网络连接，直接观察真实遍历器经虚接口输出的块与硬链接关系。
class RecordingSink final : public ISourceSink
{
  public:
    QString stage() const override
    {
        return "recording";
    }
    void entry(const QJsonObject& value) override
    {
        entries.append(value);
    }
    void chunk(qint64 offset, const QByteArray& value) override
    {
        require(offset == bytes.size() && !value.isEmpty() &&
                    value.size() <= kChunkBytes,
                "Invalid source block boundary");
        bytes.append(value);
        ++chunks;
    }
    void finishFile(const QString& sha256) override
    {
        hashes.append(sha256);
    }
    void problem(const QString& path, const QString& reason, QJsonObject&,
                 QJsonArray&) const override
    {
        throw Error(path + ": " + reason);
    }
    QJsonArray entries;
    QByteArray bytes;
    QStringList hashes;
    int chunks = 0;
};

void checkSource(const QString& directory)
{
    const QByteArray bytes(kChunkBytes + 17, 'a');
    const auto original = (directory + "/original").toUtf8();
    QFile file(QString::fromUtf8(original));
    require(file.open(QIODevice::WriteOnly) &&
                file.write(bytes) == bytes.size() && file.flush(),
            "Cannot prepare source sample");
    file.close();
    const auto alias = (directory + "/alias").toUtf8();
    require(::link(original.constData(), alias.constData()) == 0,
            "Cannot prepare hard link");
    RecordingSink sink;
    const auto status =
        sourceTree({{"path", directory}}, sink, [](const auto&) {});
    require(status.value("stage") == "recording" &&
                status.value("complete").toBool() &&
                number(status, "files") == 2 &&
                number(status, "hardlinks") == 1 &&
                number(status, "bytes") == bytes.size() * 2 &&
                number(status, "stored_bytes") == bytes.size() &&
                sink.entries.size() == 2 && sink.chunks == 2 &&
                sink.bytes == bytes && sink.hashes.size() == 1 &&
                sink.hashes[0] ==
                    QString::fromLatin1(QCryptographicHash::hash(
                                            bytes, QCryptographicHash::Sha256)
                                            .toHex()),
            "Source sink changed content, counters or hard-link streaming");
    const auto anchor = sink.entries[0].toObject();
    const auto linked = sink.entries[1].toObject();
    require(anchor.value("type") == "file" &&
                linked.value("type") == "hardlink" &&
                linked.value("link_to") == anchor.value("path"),
            "Source sink lost hard-link identity");

    ScanSourceSink scanner;
    QJsonObject failed{{"error_count", 0}};
    QJsonArray warnings;
    scanner.problem({}, "denied", failed, warnings);
    scanner.problem("second", "changed", failed, warnings);
    require(!failed.value("complete").toBool() &&
                number(failed, "error_count") == 2 && warnings.size() == 2 &&
                warnings[0].toObject().value("path") == ".",
            "Scan sink did not retain all failures");
}

void checkNodes(const QString& directory)
{
    Descriptor parent(::open(directory.toUtf8().constData(),
                             O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    const QJsonObject folder{{"type", "directory"}};
    const auto created = restoreNode(parent.get(), "folder", folder);
    require(created.metadata == MetadataTiming::Deferred &&
                QString::fromLatin1(created.counter) == "directories",
            "Directory metadata must be deferred");
    rejects([&] { restoreNode(parent.get(), "folder", folder); });
    const auto link =
        restoreNode(parent.get(), "link",
                    {{"type", "symlink"},
                     {"target_base64",
                      QString::fromLatin1(QByteArray("missing").toBase64())}});
    QByteArray target(32, '\0');
    const auto length =
        ::readlinkat(parent.get(), "link", target.data(), target.size());
    require(length == 7 && target.first(length) == "missing" &&
                link.metadata == MetadataTiming::Immediate,
            "Symlink strategy changed target or metadata timing");
    restoreNode(parent.get(), "pipe", {{"type", "fifo"}});
    struct stat info
    {
    };
    require(::fstatat(parent.get(), "pipe", &info, AT_SYMLINK_NOFOLLOW) == 0 &&
                S_ISFIFO(info.st_mode),
            "FIFO strategy did not create a FIFO");
    rejects([&]
            { restoreNode(parent.get(), "unknown", {{"type", "unknown"}}); });
}

void checkOperations(const QString& source, const QString& state)
{
    for (const auto* action : {"scan", "backup", "restore", "delete_version"})
    {
        require(isDataOperation(action), "Missing data operation");
    }
    require(!isDataOperation("versions") && !isDataOperation("ping") &&
                !isDataOperation("confirm") && !isDataOperation("unknown"),
            "Query dispatch should remain with the query handler");
    const QJsonObject args{{"path", source}};
    const QJsonObject empty;
    const QJsonArray emptyList;
    const OperationContext context{args,      empty,     empty,
                                   emptyList, emptyList, state};
    const auto result =
        executeDataOperation("scan", context, [](const auto&) {});
    require(number(result, "files") == 2 && result.value("stage") == "scan",
            "Operation dispatch did not execute a real scan");
    const OperationContext overlap{args,      empty,     empty,
                                   emptyList, emptyList, source};
    rejects([&] { executeDataOperation("scan", overlap, [](const auto&) {}); });
    rejects([&]
            { executeDataOperation("unknown", context, [](const auto&) {}); });
    rejects([&]
            { executeDataOperation("versions", context, [](const auto&) {}); });
}
} // 匿名命名空间

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    static_assert(std::is_abstract_v<ISourceSink> &&
                  std::has_virtual_destructor_v<ISourceSink>);
    try
    {
        QTemporaryDir root;
        require(root.isValid() && QDir(root.path()).mkdir("source") &&
                    QDir(root.path()).mkdir("nodes") &&
                    QDir(root.path()).mkdir("metadata"),
                "Cannot prepare test directories");
        checkMetadata(root.path() + "/metadata");
        checkSource(root.path() + "/source");
        checkNodes(root.path() + "/nodes");
        checkOperations(root.path() + "/source", root.path() + "/state");
        std::cout << "Agent module contracts passed\n";
        return 0;
    }
    catch (const std::exception& failure)
    {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
