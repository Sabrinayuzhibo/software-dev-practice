#include "backup/core/encryption.h"
#include "backup/core/io.h"

#include <openssl/evp.h>
#include <openssl/rand.h>

namespace backup::core
{
namespace
{
constexpr int kNonceBytes = 12;
constexpr int kTagBytes = 16;
constexpr int kMaxSealBytes = 1024 * 1024;

class AesGcm final : public IEncryptionAlgorithm
{
  public:
    QByteArray seal(const Secret& key, QByteArrayView plain,
                    QByteArrayView context) const override
    {
        const auto nonce = secureRandom(kNonceBytes);
        QByteArray tag(kTagBytes, Qt::Uninitialized);
        const auto encrypted = transform(key, true, nonce, plain, context, tag);
        return nonce + encrypted + tag;
    }

    QByteArray open(const Secret& key, QByteArrayView ciphertext,
                    QByteArrayView context) const override
    {
        require(ciphertext.size() >= kSealOverhead, "Truncated ciphertext");
        auto tag = ciphertext.last(kTagBytes).toByteArray();
        return transform(
            key, false, ciphertext.first(kNonceBytes),
            ciphertext.sliced(kNonceBytes, ciphertext.size() - kSealOverhead),
            context, tag);
    }

  private:
    static QByteArray transform(const Secret& key, bool encrypt,
                                QByteArrayView nonce, QByteArrayView input,
                                QByteArrayView context, QByteArray& tag)
    {
        require(input.size() <= kMaxSealBytes && context.size() <= 4096,
                "Encryption block exceeds supported size");
        std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(
            EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
        const auto asBytes = [](QByteArrayView data)
        { return reinterpret_cast<const unsigned char*>(data.data()); };
        require(ctx && EVP_CipherInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr,
                                         key.bytes.data(), asBytes(nonce),
                                         encrypt) == 1,
                "Cannot initialize AES-256-GCM");
        int used = 0;
        require(EVP_CipherUpdate(ctx.get(), nullptr, &used, asBytes(context),
                                 static_cast<int>(context.size())) == 1,
                "Cannot bind encryption context");
        QByteArray result(input.size() + EVP_MAX_BLOCK_LENGTH,
                          Qt::Uninitialized);
        auto* out = reinterpret_cast<unsigned char*>(result.data());
        require(EVP_CipherUpdate(ctx.get(), out, &used, asBytes(input),
                                 static_cast<int>(input.size())) == 1,
                "Cannot process encryption block");
        if (!encrypt)
        {
            require(EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG,
                                        kTagBytes, tag.data()) == 1,
                    "Cannot set authentication tag");
        }
        int tail = 0;
        if (EVP_CipherFinal_ex(ctx.get(), out + used, &tail) != 1)
        {
            OPENSSL_cleanse(out, static_cast<size_t>(result.size()));
            throw Error("Authentication failed: wrong password or damaged "
                        "encrypted data");
        }
        if (encrypt)
        {
            require(EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG,
                                        kTagBytes, tag.data()) == 1,
                    "Cannot read authentication tag");
        }
        result.resize(used + tail);
        return result;
    }
};
} // 匿名命名空间

Secret::~Secret()
{
    OPENSSL_cleanse(bytes.data(), bytes.size());
}

const IEncryptionAlgorithm& encryptionAlgorithm(int format)
{
    require(format == 1, "Unsupported encryption format");
    static const AesGcm aes;
    return aes;
}

std::shared_ptr<Secret> deriveSecret(const Secret& key, QByteArrayView context)
{
    auto result = std::make_shared<Secret>();
    size_t written = 0;
    require(EVP_Q_mac(nullptr, "HMAC", nullptr, "SHA256", nullptr,
                      key.bytes.data(), key.bytes.size(),
                      reinterpret_cast<const unsigned char*>(context.data()),
                      context.size(), result->bytes.data(),
                      result->bytes.size(), &written) &&
                written == result->bytes.size(),
            "Cannot derive encryption subkey");
    return result;
}

QByteArray secureRandom(qsizetype bytes)
{
    require(bytes > 0 && bytes <= kMaxSealBytes, "Invalid random byte count");
    QByteArray result(bytes, Qt::Uninitialized);
    require(RAND_bytes(reinterpret_cast<unsigned char*>(result.data()),
                       static_cast<int>(bytes)) == 1,
            "Cannot obtain secure random bytes");
    return result;
}

std::shared_ptr<Secret> passwordKey(const QString& password,
                                    QByteArrayView salt)
{
    auto utf8 = password.toUtf8();
    if (utf8.isEmpty() || utf8.size() > 1024 || salt.size() != kSaltBytes)
    {
        OPENSSL_cleanse(utf8.data(), static_cast<size_t>(utf8.size()));
        throw Error("Password must contain 1 to 1024 UTF-8 bytes");
    }
    auto result = std::make_shared<Secret>();
    constexpr uint64_t kCost = 1 << 17;
    constexpr uint64_t kMaxMemory = 192 * 1024 * 1024;
    const int derived = EVP_PBE_scrypt(
        utf8.constData(), utf8.size(),
        reinterpret_cast<const unsigned char*>(salt.data()), salt.size(), kCost,
        8, 1, kMaxMemory, result->bytes.data(), result->bytes.size());
    OPENSSL_cleanse(utf8.data(), static_cast<size_t>(utf8.size()));
    require(derived == 1, "Cannot derive encryption key");
    return result;
}

QByteArray decodeCiphertext(const QString& encoded, qsizetype maxBytes)
{
    require(encoded.size() <= ((maxBytes + 2) / 3) * 4,
            "Encoded encrypted block exceeds supported size");
    auto value = QByteArray::fromBase64Encoding(
        encoded.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    require(bool(value) && value.decoded.size() <= maxBytes,
            "Invalid encrypted block encoding");
    return std::move(value.decoded);
}

QString encodeCiphertext(QByteArrayView bytes)
{
    return QString::fromLatin1(bytes.toByteArray().toBase64());
}
} // 命名空间 backup::core
