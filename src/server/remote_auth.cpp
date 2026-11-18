#include "backup/server/remote_auth.h"
#include "backup/core/encryption.h"
#include "backup/core/io.h"

#include <QCryptographicHash>
#include <QFileInfo>
#include <QJsonDocument>
#include <fcntl.h>
#include <openssl/crypto.h>
#include <unistd.h>

namespace backup::server
{
using namespace core;
namespace
{
constexpr qsizetype kTokenBytes = 32;
constexpr qsizetype kMaxSessions = 64;
constexpr int kLoginIntervalMs = 1000;

QByteArray bytes(const Secret& secret)
{
    return QByteArray(reinterpret_cast<const char*>(secret.bytes.data()),
                      secret.bytes.size());
}
} // 匿名命名空间

void RemoteAuth::create(const QString& path, const QString& account,
                        const QString& password)
{
    require(validId(account), "Invalid remote account name");
    const auto salt = secureRandom(kSaltBytes);
    const auto verifier = passwordKey(password, salt);
    Descriptor descriptor(
        ::open(QFile::encodeName(path).constData(),
               O_WRONLY | O_CLOEXEC | O_NOFOLLOW | O_CREAT | O_EXCL, 0600));
    QFile file;
    require(file.open(descriptor.get(), QIODevice::WriteOnly),
            "Cannot open account file");
    const QJsonObject object{{"format", 1},
                             {"account", account},
                             {"salt", encodeCiphertext(salt)},
                             {"verifier", encodeCiphertext(bytes(*verifier))}};
    writeAll(file, QJsonDocument(object).toJson(QJsonDocument::Compact));
    syncFile(file);
    syncDirectory(QFileInfo(path).absolutePath());
}

RemoteAuth::RemoteAuth(const QString& path, int lifetimeSeconds)
    : lifetimeSeconds_(lifetimeSeconds)
{
    struct stat info
    {
    };
    require(::lstat(QFile::encodeName(path).constData(), &info) == 0 &&
                S_ISREG(info.st_mode) && info.st_uid == ::getuid() &&
                (info.st_mode & 0077) == 0,
            "Remote account file must be private");
    const auto object = readObject(path);
    account_ = text(object, "account");
    salt_ = decodeCiphertext(text(object, "salt"), kSaltBytes);
    verifier_ = decodeCiphertext(text(object, "verifier"), kTokenBytes);
    require(object.value("format") == 1 && validId(account_) &&
                salt_.size() == kSaltBytes && verifier_.size() == kTokenBytes &&
                lifetimeSeconds_ > 0 && lifetimeSeconds_ <= 86400,
            "Invalid remote authentication configuration");
}

QByteArray RemoteAuth::digest(const QString& token)
{
    const auto raw = decodeCiphertext(token, kTokenBytes);
    require(raw.size() == kTokenBytes, "Invalid session token");
    return QCryptographicHash::hash(raw, QCryptographicHash::Sha256);
}

bool RemoteAuth::valid(const QByteArray& session) const
{
    const auto found = sessions_.constFind(session);
    return found != sessions_.cend() && !found->hasExpired();
}

QJsonObject RemoteAuth::login(const QJsonObject& args)
{
    require(!lastAttempt_.isValid() ||
                lastAttempt_.hasExpired(kLoginIntervalMs),
            "Wait one second before trying to sign in again");
    lastAttempt_.start();
    const auto name = text(args, "account");
    require(name.size() <= 80, "Invalid remote account name");
    const auto candidate = passwordKey(text(args, "password"), salt_);
    const bool matches = CRYPTO_memcmp(candidate->bytes.data(),
                                       verifier_.constData(), kTokenBytes) == 0;
    require(matches && name == account_, "Invalid remote account or password");
    sessions_.removeIf(
        [](const QHash<QByteArray, QDeadlineTimer>::iterator& item)
        { return item.value().hasExpired(); });
    require(sessions_.size() < kMaxSessions, "Too many active remote sessions");
    const auto token = encodeCiphertext(secureRandom(kTokenBytes));
    sessions_.insert(digest(token), QDeadlineTimer(lifetimeSeconds_ * 1000LL));
    return {{"session_token", token}, {"expires_in", lifetimeSeconds_}};
}
} // 命名空间 backup::server
