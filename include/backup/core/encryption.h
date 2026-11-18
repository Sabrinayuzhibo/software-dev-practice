#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <array>
#include <memory>

namespace backup::core
{
inline constexpr qsizetype kSealOverhead = 28;
inline constexpr qsizetype kSaltBytes = 16;

// 密钥仅存在调用进程内存，销毁时清除。禁止隐式复制。
class Secret final
{
  public:
    Secret() = default;
    ~Secret();
    Secret(const Secret&) = delete;
    Secret& operator=(const Secret&) = delete;
    std::array<unsigned char, 32> bytes{};
};

// 认证失败抛出 Error，且不会返回未经认证的明文。
class IEncryptionAlgorithm
{
  public:
    virtual ~IEncryptionAlgorithm() = default;
    virtual QByteArray seal(const Secret& key, QByteArrayView plain,
                            QByteArrayView context) const = 0;
    virtual QByteArray open(const Secret& key, QByteArrayView ciphertext,
                            QByteArrayView context) const = 0;
};

const IEncryptionAlgorithm& encryptionAlgorithm(int format = 1);
std::shared_ptr<Secret> deriveSecret(const Secret& key, QByteArrayView context);
QByteArray secureRandom(qsizetype bytes);
std::shared_ptr<Secret> passwordKey(const QString& password,
                                    QByteArrayView salt);
QByteArray decodeCiphertext(const QString& encoded, qsizetype maxBytes);
QString encodeCiphertext(QByteArrayView bytes);
} // 命名空间 backup::core
