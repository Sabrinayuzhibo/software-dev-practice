#include "backup/agent/remote_session.h"
#include "backup/agent/channel.h"

#include <QDir>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

namespace backup::agent
{
using namespace core;
namespace
{
thread_local RemoteSessions* activeSessions = nullptr;
}

ChannelError::ChannelError(ConnectionFailure kind, const QString& message)
    : Error(message), kind_(kind)
{
}

ConnectionFailure ChannelError::kind() const
{
    return kind_;
}

QString ChannelError::code() const
{
    switch (kind_)
    {
        case ConnectionFailure::Authentication:
            return "AUTH_REQUIRED";
        case ConnectionFailure::Certificate:
            return "CERTIFICATE_FAILED";
        case ConnectionFailure::Network:
            return "WAITING_CONNECTION";
    }
    return {};
}

bool remoteTarget(const QJsonObject& target)
{
    return target.value("mode") == "remote";
}

void validateEndpoint(const QJsonObject& target)
{
    const auto mode = target.value("mode").toString("local");
    const auto host = text(target, "host");
    const auto port = number(target, "port");
    require(port > 0 && port <= 65535 && (mode == "local" || mode == "remote"),
            "Invalid backup target");
    if (mode == "local")
    {
        require(host == "127.0.0.1", "Local target must use 127.0.0.1");
        return;
    }
    static const QRegularExpression hostname(
        "^[a-zA-Z0-9](?:[a-zA-Z0-9.-]*[a-zA-Z0-9])?$");
    require(host.size() <= 253 && (!QHostAddress(host).isNull() ||
                                   hostname.match(host).hasMatch()),
            "Invalid remote hostname or address");
    const auto ca = target.value("ca_certificate").toString();
    require(
        !target.contains("ca_certificate") ||
            (target.value("ca_certificate").isString() &&
             (ca.isEmpty() || (ca.size() <= 4096 && !ca.contains(QChar(0)) &&
                               QDir::isAbsolutePath(ca)))),
        "Invalid CA certificate path");
}

QString endpointKey(const QJsonObject& target)
{
    return QString::fromUtf8(
        QJsonDocument(QJsonArray{target.value("host"), target.value("port"),
                                 target.value("ca_certificate").toString()})
            .toJson(QJsonDocument::Compact));
}

RemoteSessions::Scope::Scope(RemoteSessions& sessions)
{
    require(!activeSessions, "Nested remote session scope");
    activeSessions = &sessions;
}

RemoteSessions::Scope::~Scope()
{
    activeSessions = nullptr;
}

void RemoteSessions::resume(Channel& channel, const QJsonObject& target)
{
    const auto key = endpointKey(target);
    if (!activeSessions || !activeSessions->sessions_.contains(key) ||
        activeSessions->sessions_.value(key).expiry.hasExpired())
    {
        throw ChannelError(ConnectionFailure::Authentication,
                           "AUTH_REQUIRED: Sign in to the remote target");
    }
    try
    {
        channel.rawCall(
            "remote_resume",
            {{"session_token", activeSessions->sessions_.value(key).token}});
    }
    catch (const ChannelError& error)
    {
        if (error.kind() == ConnectionFailure::Authentication)
        {
            activeSessions->sessions_.remove(key);
        }
        throw;
    }
}

QJsonObject RemoteSessions::login(Channel& channel, const QJsonObject& target,
                                  const QJsonObject& args)
{
    require(remoteTarget(target), "Login requires a remote target");
    const auto password = text(args, "password");
    const auto account = text(args, "account");
    require(validId(account) && password.toUtf8().size() <= 1024,
            "Invalid login fields");
    const auto reply = channel.rawCall(
        "remote_login", {{"account", account}, {"password", password}});
    const auto lifetime = number(reply, "expires_in");
    const auto token = text(reply, "session_token");
    require(lifetime > 0 && lifetime <= 86400 && token.size() <= 64,
            "Invalid remote session response");
    sessions_.removeIf([](const QHash<QString, Session>::iterator& item)
                       { return item.value().expiry.hasExpired(); });
    sessions_.insert(endpointKey(target),
                     {token, QDeadlineTimer(lifetime * 1000)});
    channel.authenticate();
    return {{"expires_in", lifetime}};
}
} // 命名空间 backup::agent
