#include "backup/agent/vault.h"
#include "backup/agent/channel.h"
#include "backup/core/io.h"

#include <openssl/crypto.h>

namespace backup::agent
{
using namespace core;
namespace
{
thread_local VaultKeys* activeKeys = nullptr;

QByteArray wrapContext(const QJsonObject& header)
{
    const auto id = text(header, "repository_id");
    require(header.value("format") == 1 && validId(id),
            "Unsupported repository encryption header");
    return "BackupSystem/v1/key/" + id.toUtf8();
}
} // 匿名命名空间

VaultAccess newVault(const QString& password)
{
    const auto salt = secureRandom(kSaltBytes);
    const auto wrapping = passwordKey(password, salt);
    auto master = std::make_shared<Secret>();
    auto random = secureRandom(master->bytes.size());
    std::copy(random.cbegin(), random.cend(), master->bytes.begin());
    OPENSSL_cleanse(random.data(), static_cast<size_t>(random.size()));
    QJsonObject header{{"format", 1},
                       {"repository_id", newId()},
                       {"salt", encodeCiphertext(salt)}};
    const QByteArrayView key(
        reinterpret_cast<const char*>(master->bytes.data()),
        master->bytes.size());
    header.insert("wrapped_key", encodeCiphertext(encryptionAlgorithm().seal(
                                     *wrapping, key, wrapContext(header))));
    return {header, master};
}

VaultAccess unlockVault(const QJsonObject& header, const QString& password)
{
    const auto context = wrapContext(header);
    const auto salt = decodeCiphertext(text(header, "salt"), kSaltBytes);
    const auto wrapped =
        decodeCiphertext(text(header, "wrapped_key"), 32 + kSealOverhead);
    const auto wrapping = passwordKey(password, salt);
    auto master = std::make_shared<Secret>();
    auto key = encryptionAlgorithm().open(*wrapping, wrapped, context);
    require(key.size() == static_cast<qsizetype>(master->bytes.size()),
            "Invalid wrapped repository key");
    std::copy(key.cbegin(), key.cend(), master->bytes.begin());
    OPENSSL_cleanse(key.data(), static_cast<size_t>(key.size()));
    return {header, master};
}

QByteArray vaultContext(const VaultAccess& access, const QString& version,
                        const QByteArray& purpose)
{
    return "BackupSystem/v1/" + text(access.header, "repository_id").toUtf8() +
           '/' + version.toUtf8() + '/' + purpose;
}

VaultKeys::Scope::Scope(VaultKeys& keys)
{
    require(!activeKeys, "Nested key scope");
    activeKeys = &keys;
}

VaultKeys::Scope::~Scope()
{
    activeKeys = nullptr;
}

VaultAccess VaultKeys::access(const QJsonObject& header)
{
    if (activeKeys && !header.isEmpty())
    {
        const auto saved =
            activeKeys->unlocked_.value(text(header, "repository_id"));
        if (saved.header == header)
        {
            return saved;
        }
    }
    return {header, {}};
}

QJsonObject VaultKeys::configure(const QString& action, const QJsonObject& args,
                                 Channel& channel)
{
    auto header = channel.health().value("encryption").toObject();
    if (action == "initialize_encryption")
    {
        require(header.isEmpty() &&
                    channel.health().value("sealed_protocol") == 1,
                "Encryption requires an empty repository on an updated Server");
        const auto access = newVault(text(args, "password"));
        header = channel.rawCall(action, access.header);
        require(header == access.header,
                "Server did not accept encryption policy");
        unlocked_.insert(text(header, "repository_id"), access);
    }
    else if (action == "unlock_repository")
    {
        require(!header.isEmpty(), "Repository encryption is not enabled");
        auto access = unlockVault(header, text(args, "password"));
        unlocked_.insert(text(header, "repository_id"), std::move(access));
    }
    else
    {
        require(!header.isEmpty(), "Repository encryption is not enabled");
        unlocked_.remove(text(header, "repository_id"));
    }
    return {{"repository_encryption_id", text(header, "repository_id")},
            {"data_root", channel.health().value("data_root")},
            {"unlocked", action != "lock_repository"}};
}
} // 命名空间 backup::agent
