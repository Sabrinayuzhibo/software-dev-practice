#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>

namespace backup::protocol {

inline constexpr quint32 kMaxFrameBytes = 4U * 1024U * 1024U;

struct Message {
    QString type;
    QString requestId;
    QJsonObject payload;
};

// A frame is a four-byte network-order length followed by one UTF-8 JSON object.
QByteArray encode(const Message& message);

class FrameDecoder {
public:
    // Handles both partial frames and several frames in one TCP read.
    bool feed(const QByteArray& chunk, QList<Message>* messages, QString* error);

private:
    QByteArray buffer_;
};

}  // namespace backup::protocol
