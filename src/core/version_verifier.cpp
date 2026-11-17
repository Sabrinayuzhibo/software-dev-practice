#include "backup/core/version_verifier.h"
#include "backup/core/compression.h"
#include "backup/core/io.h"

#include <QJsonArray>
#include <QJsonDocument>

namespace backup::core
{
VersionVerifier::VersionVerifier(const QJsonObject& summary)
    : summary_(summary), scope_(summary, "source")
{
    const auto format = summary_.value("format").toInt();
    require(supportedVersionFormat(format), "Unsupported version format");
    require((format >= 5) == (compressionAlgorithm(summary_) != "none"),
            "Compression does not match version format");
    require(format >= 3 || (!scope_.fileTypes().contains("character_device") &&
                            !scope_.fileTypes().contains("block_device") &&
                            !scope_.fileTypes().contains("socket")),
            "Version format does not support saved node types");
    if (summary_.contains("root_metadata"))
    {
        require(summary_.value("root_metadata").isObject(),
                "Invalid source root metadata");
        const auto metadata = summary_.value("root_metadata").toObject();
        require(metadata.contains("mode"), "Missing source root metadata");
        validateMetadata(metadata);
    }
}

QByteArray VersionVerifier::append(const QJsonObject& entry)
{
    const auto format = summary_.value("format").toInt();
    if (format >= 5 && entry.value("type") == "file")
    {
        require(compressionAlgorithm(entry) == compressionAlgorithm(summary_),
                "Content compression differs from version policy");
    }
    require(scope_.allows(entry),
            "Manifest entry is outside the saved selection: " +
                text(entry, "path"));
    manifest_.append(entry, format);
    if (scope_.hasFilters() && scope_.preservesEmptyDirectories() &&
        entry.value("type") == "directory" &&
        scope_.matches(text(entry, "path"), "directory", 0,
                       number(entry, "uid"), signedNumber(entry, "mtime_sec")))
    {
        allowedEmptyDirectories_.insert(text(entry, "path"));
    }
    const auto line =
        QJsonDocument(entry).toJson(QJsonDocument::Compact) + '\n';
    require(line.size() < 64 * 1024, "Manifest entry exceeds supported size");
    hash_.addData(line);
    return line;
}

void VersionVerifier::finish()
{
    for (const auto& value : scope_.selection())
    {
        const auto path = text(value.toObject(), "path");
        require((value.toObject().value("type") == "directory" &&
                 !scope_.includes("directory")) ||
                    manifest_.contains(path) || scope_.hasFilters(),
                "Selected source missing from manifest: " + path);
    }
    if (scope_.hasFilters() ||
        (scope_.hasEmptyDirectoryRule() && !scope_.preservesEmptyDirectories()))
    {
        manifest_.requirePopulatedDirectories(allowedEmptyDirectories_);
    }
    const auto totals = manifest_.totals();
    const auto format = summary_.value("format").toInt();
    for (auto item = totals.begin(); item != totals.end(); ++item)
    {
        const auto field = item.key().toLatin1();
        if ((format == 1 && field != "entries" && field != "files" &&
             field != "directories" && field != "bytes") ||
            (format == 2 && (field == "character_devices" ||
                             field == "block_devices" || field == "sockets")))
        {
            continue;
        }
        require(number(summary_, field.constData()) ==
                    number(totals, field.constData()),
                "Version manifest statistics mismatch: " + item.key());
    }
    require(QString::fromLatin1(hash_.result().toHex()) ==
                text(summary_, "manifest_sha256"),
            "Version manifest checksum mismatch");
}
} // 命名空间 backup::core
