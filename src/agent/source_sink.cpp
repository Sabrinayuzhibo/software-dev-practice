#include "backup/agent/source_sink.h"
#include "backup/agent/channel.h"
#include "backup/core/io.h"

#include <limits>

namespace backup::agent
{
QString ScanSourceSink::stage() const
{
    return "scan";
}
void ScanSourceSink::entry(const QJsonObject&) {}
void ScanSourceSink::chunk(qint64, const QByteArray&) {}
void ScanSourceSink::finishFile(const QString&) {}

void ScanSourceSink::problem(const QString& path, const QString& reason,
                             QJsonObject& status, QJsonArray& warnings) const
{
    const auto count = core::number(status, "error_count");
    core::require(count < std::numeric_limits<qint64>::max(),
                  "Source statistics overflow");
    warnings.append(QJsonObject{{"path", path.isEmpty() ? "." : path},
                                {"reason", reason},
                                {"severity", "error"}});
    status.insert("complete", false);
    status.insert("error_count", count + 1);
}

UploadSourceSink::UploadSourceSink(Channel& channel) : channel_(channel) {}
QString UploadSourceSink::stage() const
{
    return "upload";
}
void UploadSourceSink::entry(const QJsonObject& value)
{
    channel_.call("entry", value);
}
void UploadSourceSink::chunk(qint64 offset, const QByteArray& bytes)
{
    channel_.call("chunk", {{"offset", QString::number(offset)},
                            {"data", QString::fromLatin1(bytes.toBase64())}});
}
void UploadSourceSink::finishFile(const QString& sha256)
{
    channel_.call("finish_file", {{"sha256", sha256}});
}
void UploadSourceSink::problem(const QString& path, const QString& reason,
                               QJsonObject&, QJsonArray&) const
{
    throw core::Error(path + ": " + reason);
}
} // namespace backup::agent
