#include "backup/core/archive.h"
#include "backup/core/compression.h"
#include "backup/core/detail_page.h"
#include "backup/core/io.h"
#include "backup/core/version_verifier.h"

#include <QJsonDocument>
#include <QtEndian>
#include <limits>
#include <memory>

namespace backup::core
{
namespace
{
const QByteArray kHeader = QByteArray::fromHex("4253415200010000");
constexpr qsizetype kRecordHeaderBytes = 12;
constexpr qsizetype kTrailerBytes = 48;

QByteArray recordHeader(ArchiveRecord type, quint64 length)
{
    QByteArray header(kRecordHeaderBytes, '\0');
    qToBigEndian(static_cast<quint32>(type), header.data());
    qToBigEndian(length, header.data() + 4);
    return header;
}

void checkLength(ArchiveRecord type, quint64 length)
{
    if (type == ArchiveRecord::Data)
    {
        require(length > 0 && length <= static_cast<quint64>(kChunkBytes),
                "Invalid archive data length");
    }
    else if (type == ArchiveRecord::FileEnd)
    {
        require(length == 0, "Invalid archive file terminator");
    }
    else if (type == ArchiveRecord::Final)
    {
        require(length == kTrailerBytes, "Invalid archive trailer length");
    }
    else
    {
        require(type == ArchiveRecord::Summary ||
                    type == ArchiveRecord::Warning ||
                    type == ArchiveRecord::Entry,
                "Unsupported archive record");
        require(length > 0 &&
                    length < static_cast<quint64>(kArchiveMetadataBytes),
                "Invalid archive metadata length");
    }
}

QByteArray readExact(QFile& input, qint64 count)
{
    const auto bytes = input.read(count);
    require(input.error() == QFileDevice::NoError && bytes.size() == count,
            "Truncated or unreadable archive");
    return bytes;
}

class FileVerifier
{
  public:
    FileVerifier(const QJsonObject& entry, const ArchiveCallbacks& callbacks)
        : entry_(entry), callbacks_(callbacks),
          algorithm_(compressionAlgorithm(entry)), size_(number(entry, "size")),
          storedSize_(algorithm_ == "none" ? size_
                                           : number(entry, "stored_size"))
    {
    }

    void append(const QByteArray& bytes)
    {
        require(bytes.size() <= storedSize_ - storedOffset_,
                "Archive content exceeds declared size");
        storedHash_.addData(bytes);
        storedOffset_ += bytes.size();
        if (algorithm_ == "none")
        {
            decoded(bytes);
            return;
        }
        pending_.append(bytes);
        require(pending_.size() <= kChunkBytes * 2 + kCompressionHeaderBytes,
                "Archive compression buffer exceeds supported size");
        while (pending_.size() >= kCompressionHeaderBytes)
        {
            if (callbacks_.checkpoint)
            {
                callbacks_.checkpoint();
            }
            const auto length = compressionBlockSize(pending_, algorithm_);
            if (pending_.size() < length)
            {
                break;
            }
            decoded(decompressBlock(pending_.first(length), algorithm_));
            pending_.remove(0, length);
        }
    }

    void finish()
    {
        require(storedOffset_ == storedSize_ && originalOffset_ == size_ &&
                    pending_.isEmpty() &&
                    QString::fromLatin1(hash_.result().toHex()) ==
                        text(entry_, "sha256"),
                "Archive file checksum or length mismatch");
        require(algorithm_ == "none" ||
                    QString::fromLatin1(storedHash_.result().toHex()) ==
                        text(entry_, "stored_sha256"),
                "Archive stored checksum mismatch");
    }

  private:
    void decoded(const QByteArray& bytes)
    {
        require(bytes.size() <= size_ - originalOffset_,
                "Archive decompression exceeds declared size");
        hash_.addData(bytes);
        originalOffset_ += bytes.size();
        if (callbacks_.content)
        {
            callbacks_.content(bytes);
        }
    }
    QJsonObject entry_;
    const ArchiveCallbacks& callbacks_;
    QString algorithm_;
    qint64 size_;
    qint64 storedSize_;
    qint64 storedOffset_ = 0;
    qint64 originalOffset_ = 0;
    QByteArray pending_;
    QCryptographicHash hash_{QCryptographicHash::Sha256};
    QCryptographicHash storedHash_{QCryptographicHash::Sha256};
};

class ArchiveParser
{
  public:
    ArchiveParser(QFile& input, const ArchiveCallbacks& callbacks)
        : input_(input), callbacks_(callbacks)
    {
    }

    ArchiveInfo run()
    {
        const auto header = readExact(input_, kHeader.size());
        require(header == kHeader, "Unsupported archive format or flags");
        hash_.addData(header);
        bytes_ = header.size();
        while (true)
        {
            if (callbacks_.checkpoint)
            {
                callbacks_.checkpoint();
            }
            const auto header = readExact(input_, kRecordHeaderBytes);
            const auto type = static_cast<ArchiveRecord>(
                qFromBigEndian<quint32>(header.constData()));
            const auto length = qFromBigEndian<quint64>(header.constData() + 4);
            checkLength(type, length);
            const auto payload = readExact(input_, static_cast<qint64>(length));
            if (type == ArchiveRecord::Final)
            {
                return finish(payload);
            }
            require(bytes_ <= std::numeric_limits<qint64>::max() -
                                  header.size() - payload.size() &&
                        records_ < std::numeric_limits<qint64>::max(),
                    "Archive counter overflow");
            hash_.addData(header);
            hash_.addData(payload);
            bytes_ += header.size() + payload.size();
            ++records_;
            accept(type, payload);
        }
    }

  private:
    void accept(ArchiveRecord type, const QByteArray& payload)
    {
        if (type == ArchiveRecord::Summary)
        {
            require(!verifier_ && records_ == 1,
                    "Duplicate or misplaced archive summary");
            summary_ = parseObject(payload);
            require(validId(text(summary_, "id")) &&
                        validId(text(summary_, "task_id")),
                    "Invalid archive version or task identifier");
            require(!summary_.contains("encrypted") &&
                        !summary_.contains("encryption"),
                    "Encrypted archives are not supported");
            if (summary_.contains("source_name"))
            {
                require(text(summary_, "source_name").size() <= 120,
                        "Archive source name is too long");
            }
            verifier_ = std::make_unique<VersionVerifier>(summary_);
            if (callbacks_.summary)
            {
                callbacks_.summary(summary_);
            }
            return;
        }
        require(bool(verifier_), "Archive summary must be first");
        if (type == ArchiveRecord::Data)
        {
            require(bool(file_), "Archive data has no file entry");
            file_->append(payload);
            return;
        }
        if (type == ArchiveRecord::FileEnd)
        {
            require(bool(file_), "Unexpected archive file terminator");
            file_->finish();
            file_.reset();
            if (callbacks_.fileEnd)
            {
                callbacks_.fileEnd();
            }
            return;
        }
        require(!file_, "Archive file is missing its terminator");
        const auto object = parseObject(payload);
        if (type == ArchiveRecord::Warning)
        {
            require(!entriesStarted_, "Archive warnings must precede entries");
            text(object, "path");
            text(object, "reason");
            if (object.contains("type"))
            {
                text(object, "type");
            }
            if (object.contains("field"))
            {
                text(object, "field");
            }
            warningHash_.addData(detailLine(object));
            require(warnings_ < std::numeric_limits<qint64>::max(),
                    "Archive warning count overflow");
            ++warnings_;
            if (callbacks_.warning)
            {
                callbacks_.warning(object);
            }
        }
        else
        {
            require(type == ArchiveRecord::Entry, "Unexpected archive record");
            entriesStarted_ = true;
            verifier_->append(object);
            if (object.value("type") == "file")
            {
                file_ = std::make_unique<FileVerifier>(object, callbacks_);
            }
            if (callbacks_.entry)
            {
                callbacks_.entry(object);
            }
        }
    }

    ArchiveInfo finish(const QByteArray& trailer)
    {
        require(verifier_ && !file_, "Incomplete archive");
        require(bytes_ <= std::numeric_limits<qint64>::max() -
                              kRecordHeaderBytes - kTrailerBytes,
                "Archive size overflow");
        const auto digest = hash_.result();
        require(qFromBigEndian<quint64>(trailer.constData()) ==
                        static_cast<quint64>(records_) &&
                    qFromBigEndian<quint64>(trailer.constData() + 8) ==
                        static_cast<quint64>(bytes_) &&
                    trailer.sliced(16) == digest,
                "Archive trailer checksum or counters mismatch");
        require(input_.read(1).isEmpty() &&
                    input_.error() == QFileDevice::NoError,
                "Trailing archive bytes");
        verifier_->finish();
        require(warnings_ == (summary_.contains("warning_count")
                                  ? number(summary_, "warning_count")
                                  : 0),
                "Archive warning count mismatch");
        require(!summary_.contains("warnings_sha256") ||
                    QString::fromLatin1(warningHash_.result().toHex()) ==
                        text(summary_, "warnings_sha256"),
                "Archive warning checksum mismatch");
        return {summary_, digest, records_,
                bytes_ + kRecordHeaderBytes + kTrailerBytes};
    }
    QFile& input_;
    const ArchiveCallbacks& callbacks_;
    QJsonObject summary_;
    std::unique_ptr<VersionVerifier> verifier_;
    std::unique_ptr<FileVerifier> file_;
    QCryptographicHash hash_{QCryptographicHash::Sha256};
    QCryptographicHash warningHash_{QCryptographicHash::Sha256};
    qint64 records_ = 0;
    qint64 bytes_ = 0;
    qint64 warnings_ = 0;
    bool entriesStarted_ = false;
};
} // 匿名命名空间

ArchiveWriter::ArchiveWriter(QFile& output) : output_(output)
{
    require(output_.pos() == 0 && output_.size() == 0,
            "Archive output must be empty");
    writeAll(output_, kHeader);
    hash_.addData(kHeader);
    bytes_ = kHeader.size();
}

void ArchiveWriter::write(ArchiveRecord type, const QByteArray& payload)
{
    require(!finished_ && type != ArchiveRecord::Final,
            "Archive is already complete");
    checkLength(type, payload.size());
    const auto header = recordHeader(type, payload.size());
    require(bytes_ <= std::numeric_limits<qint64>::max() - header.size() -
                          payload.size() &&
                records_ < std::numeric_limits<qint64>::max(),
            "Archive counter overflow");
    writeAll(output_, header);
    writeAll(output_, payload);
    hash_.addData(header);
    hash_.addData(payload);
    bytes_ += header.size() + payload.size();
    ++records_;
}

void ArchiveWriter::finish()
{
    require(!finished_, "Archive is already complete");
    require(bytes_ <= std::numeric_limits<qint64>::max() - kRecordHeaderBytes -
                          kTrailerBytes,
            "Archive size overflow");
    QByteArray trailer(kTrailerBytes, '\0');
    qToBigEndian(static_cast<quint64>(records_), trailer.data());
    qToBigEndian(static_cast<quint64>(bytes_), trailer.data() + 8);
    trailer.replace(16, 32, hash_.result());
    writeAll(output_, recordHeader(ArchiveRecord::Final, trailer.size()));
    writeAll(output_, trailer);
    finished_ = true;
}

ArchiveInfo verifyArchive(QFile& input, const ArchiveCallbacks& callbacks)
{
    ArchiveParser parser(input, callbacks);
    return parser.run();
}
} // 命名空间 backup::core
