#include "backup/core/manifest.h"
#include "backup/core/io.h"

#include <limits>
#include <sys/sysmacros.h>

namespace backup::core
{
bool Manifest::contains(const QString& path) const
{
    return paths_.contains(path);
}

bool supportedEntryType(const QString& type)
{
    return type == "file" || type == "directory" || type == "symlink" ||
           type == "hardlink" || type == "fifo" ||
           type == "character_device" || type == "block_device" ||
           type == "socket";
}

bool supportedVersionFormat(int format)
{
    return format >= 1 && format <= kVersionFormat;
}

QByteArray symlinkTarget(const QJsonObject& entry)
{
    const auto encoded = text(entry, "target_base64").toLatin1();
    require(encoded.size() <= ((kLinkTargetBytes + 2) / 3) * 4,
            "Symbolic link target is too long");
    const auto decoded = QByteArray::fromBase64Encoding(
        encoded, QByteArray::AbortOnBase64DecodingErrors);
    require(bool(decoded) && !decoded.decoded.isEmpty() &&
                decoded.decoded.size() <= kLinkTargetBytes &&
                !decoded.decoded.contains('\0') &&
                decoded.decoded.toBase64() == encoded,
            "Invalid symbolic link target");
    return decoded.decoded;
}

void Manifest::checkPath(const QString& path) const
{
    require(validPath(path) && !paths_.contains(path),
            "Unsafe or duplicate manifest path: " + path);
    const auto slash = path.lastIndexOf('/');
    require(slash < 0 || directories_.contains(path.left(slash)),
            "Missing or non-directory manifest parent: " + path);
}

void Manifest::append(const QJsonObject& entry, int format)
{
    const auto path = text(entry, "path");
    checkPath(path);
    require(number(entry, "index") == count(), "Invalid manifest index");
    const auto type = text(entry, "type");
    require(supportedVersionFormat(format) && supportedEntryType(type) &&
                (format != 1 || type == "file" || type == "directory") &&
                (format >= 3 || (type != "character_device" &&
                                 type != "block_device" && type != "socket")),
            "Unsupported manifest type or version: " + path);
    require(!entry.contains("link_group") ||
                (format >= 2 && type == "file" &&
                 entry.value("link_group") == path),
            "Invalid hard link group: " + path);
    require((!entry.contains("link_to") || type == "hardlink") &&
                (!entry.contains("target_base64") || type == "symlink"),
            "Link fields do not match entry type: " + path);
    if (format >= 4 && (type == "file" || type == "hardlink" ||
                        type == "directory" || type == "symlink" ||
                        type == "fifo"))
    {
        const QStringList metadataFields{"mode", "uid", "gid", "mtime_sec",
                                         "mtime_nsec"};
        const auto metadataCount = std::count_if(
            metadataFields.cbegin(), metadataFields.cend(),
            [&entry](const QString& field) { return entry.contains(field); });
        require(metadataCount == 0 || metadataCount == metadataFields.size(),
                "Incomplete file metadata: " + path);
        if (metadataCount != 0)
        {
            const auto mode = number(entry, "mode");
            const auto uid = number(entry, "uid");
            const auto gid = number(entry, "gid");
            signedNumber(entry, "mtime_sec");
            const auto nanoseconds = number(entry, "mtime_nsec");
            require(mode <= 0777 && uid <= std::numeric_limits<uid_t>::max() &&
                        gid <= std::numeric_limits<gid_t>::max() &&
                        nanoseconds < 1000000000,
                    "Invalid file metadata: " + path);
        }
    }
    else
    {
        for (const auto* field : {"mode", "uid", "gid", "mtime_sec",
                                  "mtime_nsec"})
        {
            require(!entry.contains(QLatin1String(field)),
                    "Metadata is not supported by this manifest format: " +
                        path);
        }
    }
    const bool device = type == "character_device" || type == "block_device";
    require(device || (!entry.contains("device_major") &&
                       !entry.contains("device_minor")),
            "Device fields do not match entry type: " + path);
    if (device)
    {
        const auto majorNumber = number(entry, "device_major");
        const auto minorNumber = number(entry, "device_minor");
        require(majorNumber <= std::numeric_limits<unsigned int>::max() &&
                    minorNumber <= std::numeric_limits<unsigned int>::max(),
                "Device number is out of range: " + path);
        const auto identifier =
            makedev(static_cast<unsigned int>(majorNumber),
                    static_cast<unsigned int>(minorNumber));
        require(major(identifier) == majorNumber &&
                    minor(identifier) == minorNumber,
                "Device number cannot be represented: " + path);
    }
    if (device || type == "socket")
    {
        require(!entry.contains("size") && !entry.contains("sha256") &&
                    !entry.contains("target_base64") &&
                    !entry.contains("link_to") &&
                    !entry.contains("link_group"),
                "Special node contains content or link fields: " + path);
    }
    if (type == "file" || type == "hardlink")
    {
        const auto size = number(entry, "size");
        const auto digest = text(entry, "sha256").toLatin1();
        require(digest.size() == 64 &&
                    QByteArray::fromHex(digest).toHex() == digest,
                "Invalid file checksum: " + path);
        require(size <= std::numeric_limits<qint64>::max() - bytes_,
                "Manifest size overflow");
        if (type == "hardlink")
        {
            const auto target = text(entry, "link_to");
            const auto anchor = groups_.value(target);
            require(validPath(target) && !anchor.isEmpty() &&
                        number(anchor, "size") == size &&
                        anchor.value("sha256") == entry.value("sha256"),
                    "Invalid hard link reference: " + path);
            ++hardlinks_;
        }
        else
        {
            storedBytes_ += size;
            if (entry.contains("link_group"))
            {
                groups_.insert(path, entry);
            }
        }
        bytes_ += size;
        ++files_;
    }
    else if (type == "directory")
    {
        directories_.insert(path);
    }
    else if (type == "symlink")
    {
        symlinkTarget(entry);
        ++symlinks_;
    }
    else if (type == "fifo")
    {
        ++fifos_;
    }
    else if (type == "character_device")
    {
        ++characterDevices_;
    }
    else if (type == "block_device")
    {
        ++blockDevices_;
    }
    else
    {
        ++sockets_;
    }
    if (type != "directory")
    {
        auto slash = path.lastIndexOf('/');
        while (slash >= 0)
        {
            const auto parent = path.left(slash);
            contentParents_.insert(parent);
            slash = parent.lastIndexOf('/');
        }
    }
    paths_.insert(path);
}

void Manifest::requirePopulatedDirectories() const
{
    for (const auto& path : directories_)
    {
        require(contentParents_.contains(path),
                "Empty directory is excluded by saved rules: " + path);
    }
}

QJsonObject Manifest::totals() const
{
    return {{"entries", QString::number(count())},
            {"files", QString::number(files_)},
            {"directories", QString::number(directories_.size())},
            {"bytes", QString::number(bytes_)},
            {"stored_bytes", QString::number(storedBytes_)},
            {"symlinks", QString::number(symlinks_)},
            {"hardlinks", QString::number(hardlinks_)},
            {"fifos", QString::number(fifos_)},
            {"character_devices", QString::number(characterDevices_)},
            {"block_devices", QString::number(blockDevices_)},
            {"sockets", QString::number(sockets_)}};
}

qint64 Manifest::count() const
{
    return paths_.size();
}
} // namespace backup::core
