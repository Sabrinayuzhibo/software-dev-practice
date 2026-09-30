#include <QDebug>

#include "backup/protocol/frame.h"

using backup::protocol::FrameDecoder;
using backup::protocol::Message;

static bool check(bool condition, const char* description) {
    if (!condition) {
        qCritical() << "FAIL:" << description;
    }
    return condition;
}

int main() {
    const Message ping{QStringLiteral("ping"), QStringLiteral("request-1"), {}};
    const QByteArray frame = backup::protocol::encode(ping);
    if (!check(!frame.isEmpty(), "encode ping")) return 1;

    FrameDecoder fragmented;
    QList<Message> messages;
    QString error;
    if (!check(fragmented.feed(frame.left(1), &messages, &error), "first byte") ||
        !check(messages.isEmpty(), "wait for full header") ||
        !check(fragmented.feed(frame.mid(1, 2), &messages, &error), "header split") ||
        !check(messages.isEmpty(), "wait for body") ||
        !check(fragmented.feed(frame.mid(3), &messages, &error), "body") ||
        !check(messages.size() == 1 && messages[0].type == ping.type &&
                   messages[0].requestId == ping.requestId,
               "decoded fragmented frame")) {
        return 1;
    }

    FrameDecoder combined;
    messages.clear();
    if (!check(combined.feed(frame + frame, &messages, &error),
               "two frames in one read") ||
        !check(messages.size() == 2, "both frames decoded")) {
        return 1;
    }

    FrameDecoder oversized;
    messages.clear();
    const QByteArray excessiveLength = QByteArray::fromHex("00400001");
    if (!check(!oversized.feed(excessiveLength, &messages, &error),
               "oversized frame rejected")) {
        return 1;
    }

    FrameDecoder malformed;
    messages.clear();
    const QByteArray emptyObjectFrame = QByteArray::fromHex("00000002") + "{}";
    if (!check(!malformed.feed(emptyObjectFrame, &messages, &error),
               "message without required fields rejected")) {
        return 1;
    }
    return 0;
}
