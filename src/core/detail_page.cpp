#include "backup/core/detail_page.h"
#include "backup/core/io.h"

#include <QCryptographicHash>
#include <QJsonDocument>

namespace backup::core
{
namespace
{
bool append(QJsonArray& page, const QJsonObject& item, qint64& bytes)
{
    const auto size = detailLine(item).size();
    if (page.size() >= kPageSize || bytes + size > kDetailPageBytes)
    {
        return false;
    }
    page.append(item);
    bytes += size;
    return true;
}

QJsonObject result(const QJsonArray& page, qint64 offset, qint64 total)
{
    const auto next = offset + page.size();
    return {{"items", page},
            {"total", total},
            {"next_offset", next < total ? QJsonValue(next) : QJsonValue()}};
}
} // namespace

QByteArray detailLine(const QJsonObject& item)
{
    auto line = QJsonDocument(item).toJson(QJsonDocument::Compact) + '\n';
    require(line.size() < kDetailPageBytes / 2, "Detail entry is too large");
    return line;
}

QJsonObject detailPage(const QJsonArray& items, qint64 offset)
{
    require(offset >= 0, "Invalid detail offset");
    QJsonArray page;
    qint64 bytes = 0;
    for (qint64 index = offset; index < items.size(); ++index)
    {
        require(items[index].isObject(), "Invalid detail entry");
        if (!append(page, items[index].toObject(), bytes))
        {
            break;
        }
    }
    return result(page, offset, items.size());
}

QJsonObject readDetailPage(const QString& path, qint64 offset, qint64 count,
                           const QString& sha256)
{
    require(offset >= 0 && count >= 0, "Invalid detail offset or count");
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "Cannot read details: " + path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QJsonArray page;
    qint64 index = 0;
    qint64 bytes = 0;
    bool full = false;
    while (!file.atEnd())
    {
        const auto line = file.readLine(kDetailPageBytes);
        require(!line.isEmpty() && line.endsWith('\n'), "Invalid detail data");
        hash.addData(line);
        if (index >= offset && !full)
        {
            full = !append(page, parseObject(line), bytes);
        }
        ++index;
    }
    require(file.error() == QFileDevice::NoError && index == count &&
                QString::fromLatin1(hash.result().toHex()) == sha256,
            "Details failed integrity verification");
    return result(page, offset, count);
}
} // namespace backup::core
