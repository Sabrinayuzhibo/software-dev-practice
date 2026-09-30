#include "backup/protocol/frame.h"

#include <QJsonDocument>
#include <QJsonParseError>

namespace backup::protocol {

QByteArray encode(const Message& message) {
    if (message.type.isEmpty() || message.requestId.isEmpty()) {
        return {};
    }

    QJsonObject object{{"type", message.type},
                       {"request_id", message.requestId},
                       {"payload", message.payload}};
    const QByteArray body = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (body.size() > static_cast<qsizetype>(kMaxFrameBytes)) {
        return {};
    }

    const auto length = static_cast<quint32>(body.size());
    QByteArray frame;
    frame.reserve(4 + body.size());
    frame.append(static_cast<char>((length >> 24) & 0xff));
    frame.append(static_cast<char>((length >> 16) & 0xff));
    frame.append(static_cast<char>((length >> 8) & 0xff));
    frame.append(static_cast<char>(length & 0xff));
    frame.append(body);
    return frame;
}

bool FrameDecoder::feed(const QByteArray& chunk, QList<Message>* messages,
                        QString* error) {
    buffer_.append(chunk);
    while (buffer_.size() >= 4) {
        const auto byte = [this](int index) {
            return static_cast<quint32>(
                static_cast<unsigned char>(buffer_.at(index)));
        };
        const quint32 length = (byte(0) << 24) | (byte(1) << 16) |
                               (byte(2) << 8) | byte(3);
        if (length == 0 || length > kMaxFrameBytes) {
            *error = QStringLiteral("Invalid frame length");
            buffer_.clear();
            return false;
        }
        if (buffer_.size() < 4 + static_cast<qsizetype>(length)) {
            return true;
        }

        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(
            buffer_.mid(4, static_cast<qsizetype>(length)), &parseError);
        buffer_.remove(0, 4 + static_cast<qsizetype>(length));
        if (parseError.error != QJsonParseError::NoError ||
            !document.isObject()) {
            *error = QStringLiteral("Invalid JSON message");
            buffer_.clear();
            return false;
        }

        const QJsonObject object = document.object();
        const QString type = object.value("type").toString();
        const QString requestId = object.value("request_id").toString();
        if (type.isEmpty() || requestId.isEmpty() ||
            !object.value("payload").isObject()) {
            *error = QStringLiteral("Missing message fields");
            buffer_.clear();
            return false;
        }
        messages->append({type, requestId, object.value("payload").toObject()});
    }
    return true;
}

}  // namespace backup::protocol
