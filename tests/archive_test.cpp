#include "backup/core/archive.h"
#include "backup/core/compression.h"
#include "backup/core/io.h"
#include "backup/core/manifest.h"
#include "backup/core/version_verifier.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryFile>
#include <functional>
#include <iostream>

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
    throw Error("Invalid archive operation accepted");
}

void segmented(const QString& algorithm)
{
    const QByteArray bytes(kChunkBytes - 7, 'c');
    const auto stored =
        algorithm == "none" ? bytes : compressBlock(bytes, algorithm);
    QJsonObject entry{
        {"path", "file"},
        {"type", "file"},
        {"index", "0"},
        {"size", QString::number(bytes.size())},
        {"sha256", QString::fromLatin1(QCryptographicHash::hash(
                                           bytes, QCryptographicHash::Sha256)
                                           .toHex())}};
    const int format =
        algorithm == "none" ? kPlainVersionFormat : kVersionFormat;
    if (algorithm != "none")
    {
        entry.insert("compression", algorithm);
        entry.insert("stored_size", QString::number(stored.size()));
        entry.insert("stored_sha256",
                     QString::fromLatin1(QCryptographicHash::hash(
                                             stored, QCryptographicHash::Sha256)
                                             .toHex()));
    }
    Manifest manifest;
    manifest.append(entry, format);
    const auto line =
        QJsonDocument(entry).toJson(QJsonDocument::Compact) + '\n';
    auto summary = manifest.totals();
    summary.insert("id", "unit-version");
    summary.insert("task_id", "unit-task");
    summary.insert("source", "/source");
    summary.insert("format", format);
    summary.insert("warning_count", 0);
    summary.insert("manifest_sha256",
                   QString::fromLatin1(QCryptographicHash::hash(
                                           line, QCryptographicHash::Sha256)
                                           .toHex()));
    if (algorithm != "none")
    {
        summary.insert("compression", algorithm);
    }
    auto old = summary;
    old.insert("format", 2);
    old.remove("compression");
    old.insert("file_types", QJsonArray{"socket"});
    rejects([&] { VersionVerifier unsupported(old); });
    QTemporaryFile file;
    require(file.open(), "Cannot prepare archive unit fixture");
    ArchiveWriter writer(file);
    writer.write(ArchiveRecord::Summary,
                 QJsonDocument(summary).toJson(QJsonDocument::Compact));
    writer.write(ArchiveRecord::Entry,
                 QJsonDocument(entry).toJson(QJsonDocument::Compact));
    // 37 字节记录刻意切开压缩头和负载，验证跨记录的增量解码。
    for (qsizetype offset = 0; offset < stored.size(); offset += 37)
    {
        writer.write(
            ArchiveRecord::Data,
            stored.sliced(offset, qMin<qsizetype>(37, stored.size() - offset)));
    }
    writer.write(ArchiveRecord::FileEnd, {});
    writer.finish();
    rejects([&] { writer.finish(); });
    rejects([&] { writer.write(ArchiveRecord::Data, "x"); });
    require(file.flush() && file.seek(0), "Cannot rewind archive unit fixture");
    QByteArray decoded;
    int finished = 0;
    ArchiveCallbacks callbacks;
    callbacks.content = [&](const QByteArray& data)
    {
        require(data.size() <= kChunkBytes,
                "Archive emitted an oversized block");
        decoded.append(data);
    };
    callbacks.fileEnd = [&] { ++finished; };
    const auto info = verifyArchive(file, callbacks);
    require(decoded == bytes && finished == 1 && info.bytes == file.size(),
            "Segmented archive round trip failed");
    require(file.seek(0), "Cannot rewind cancelled archive");
    callbacks.checkpoint = [] { throw Error("Interrupted archive"); };
    rejects([&] { verifyArchive(file, callbacks); });
    require(file.pos() < file.size(),
            "Cancellation did not stop archive reading");
}
} // 匿名命名空间

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    try
    {
        for (const auto* algorithm : {"none", "huffman", "lz77"})
        {
            segmented(algorithm);
        }
        std::cout << "PASS: segmented archive decoding, bounded callbacks, "
                     "writer state and cancellation\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
