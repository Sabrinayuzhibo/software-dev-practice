#include "backup/agent/realtime.h"
#include "backup/agent/remote_session.h"
#include "backup/core/io.h"
#include "backup/core/source_scope.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QSet>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <fcntl.h>
#include <map>
#include <poll.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

namespace backup::agent
{
using namespace core;
namespace
{
constexpr int kQuietMs = 1200;
constexpr int kMaxDelayMs = 10000;
constexpr int kReconcileMs = 30000;
constexpr uint32_t kEvents =
    IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE | IN_CREATE | IN_DELETE |
    IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT;
using Notify = std::function<void(const QString&, const QString&, bool)>;

// 工作线程只读取目录和属性；不打开特殊文件、不跟随软链接，不读取文件内容。
class Watch final : public QThread
{
  public:
    Watch(QJsonObject task, Notify notify)
        : task_(std::move(task)), notify_(std::move(notify))
    {
    }
    ~Watch() override
    {
        requestInterruption();
        wait();
    }

  private:
    void visit(const QString& path, QCryptographicHash& hash, int fd,
               QString& error)
    {
        if (isInterruptionRequested())
        {
            return;
        }
        const auto bytes = QFile::encodeName(path);
        struct stat info
        {
        };
        hash.addData(bytes);
        hash.addData(QByteArray(1, '\0'));
        Descriptor filesystem(::open("/", O_PATH | O_DIRECTORY | O_CLOEXEC));
        Descriptor node(path == "/"
                            ? ::dup(filesystem.get())
                            : openBelow(filesystem.get(), path.mid(1), O_PATH));
        require(::fstat(node.get(), &info) == 0,
                "Cannot inspect monitored source: " + path);
        for (const auto value :
             {qint64(info.st_dev), qint64(info.st_ino), qint64(info.st_mode),
              qint64(info.st_uid), qint64(info.st_gid), qint64(info.st_size),
              qint64(info.st_mtim.tv_sec), qint64(info.st_mtim.tv_nsec),
              qint64(info.st_ctim.tv_sec), qint64(info.st_ctim.tv_nsec)})
        {
            hash.addData(QByteArray::number(value) + ',');
        }
        if (!S_ISDIR(info.st_mode) && !S_ISREG(info.st_mode))
        {
            return;
        }
        // 只跟随本进程固定描述符的 proc 链接；来源路径的各级软链接均已拒绝。
        const auto pinned =
            QFile::encodeName(QString("/proc/self/fd/%1").arg(node.get()));
        const auto mask = kEvents | (S_ISDIR(info.st_mode) ? IN_ONLYDIR : 0);
        if (fd >= 0)
        {
            const auto watch =
                ::inotify_add_watch(fd, pinned.constData(), mask);
            if (watch < 0)
            {
                error = "监听不完整，正在通过定期核对补偿：" + path;
            }
            else
            {
                sources_.insert(watch);
                seen_.insert(watch);
            }
        }
        if (!S_ISDIR(info.st_mode))
        {
            return;
        }
        // 从同一固定节点打开可读目录，避免再次按路径解析时被替换。
        Descriptor directory(
            ::openat(node.get(), ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        QDir dir(QString("/proc/self/fd/%1").arg(directory.get()));
        if (!QFileInfo(path).isReadable())
        {
            error = "无法读取目录：" + path;
            return;
        }
        const auto children =
            dir.entryList(QDir::AllEntries | QDir::Hidden | QDir::System |
                              QDir::NoDotAndDotDot,
                          QDir::Name);
        for (const auto& name : children)
        {
            visit(QDir(path).filePath(name), hash, fd, error);
            if (isInterruptionRequested())
            {
                return;
            }
        }
    }

    void run() override
    {
        const auto id = task_.value("id").toString();
        const int fd = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);

        const SourceScope scope(task_);
        QElapsedTimer timer;
        timer.start();
        qint64 nextScan = 0;
        QByteArray previous;
        while (!isInterruptionRequested())
        {
            if (timer.elapsed() >= nextScan)
            {
                QCryptographicHash hash(QCryptographicHash::Sha256);
                QString error =
                    fd < 0 ? "文件监听资源不可用，每 30 秒核对变化" : QString();
                seen_.clear();
                sources_.clear();
                for (const auto& relative : scope.paths())
                {
                    try
                    {
                        visit(relative, hash, fd, error);
                    }
                    catch (const std::exception& failure)
                    {
                        error = QString::fromUtf8(failure.what());
                        hash.addData(error.toUtf8());
                    }
                    // 选定文件原子替换、来源目录删除后重建，需要父目录通知。
                    auto parent = QFileInfo(relative).absolutePath();
                    while (fd >= 0)
                    {
                        const auto watch = ::inotify_add_watch(
                            fd, QFile::encodeName(parent).constData(),
                            kEvents | IN_ONLYDIR | IN_DONT_FOLLOW);
                        if (watch >= 0)
                        {
                            seen_.insert(watch);
                            break;
                        }
                        if (parent == "/")
                        {
                            break;
                        }
                        parent = QFileInfo(parent).absolutePath();
                    }
                }
                const auto current = hash.result();
                notify_(id, error, current != previous);
                previous = current;
                for (const auto watch : watched_ - seen_)
                {
                    ::inotify_rm_watch(fd, watch);
                }
                watched_ = seen_;
                nextScan = timer.elapsed() + kReconcileMs;
            }
            if (fd < 0)
            {
                QThread::msleep(100);
                continue;
            }
            pollfd descriptor{fd, POLLIN, 0};
            if (::poll(&descriptor, 1, 100) <= 0)
            {
                continue;
            }
            alignas(inotify_event) char bytes[64 * 1024];
            bool changed = false;
            bool rescan = false;
            QString error;
            ssize_t count;
            while ((count = ::read(fd, bytes, sizeof(bytes))) > 0)
            {
                for (ssize_t offset = 0; offset < count;)
                {
                    const auto* event =
                        reinterpret_cast<const inotify_event*>(bytes + offset);
                    rescan |= (event->mask & (kEvents | IN_Q_OVERFLOW)) != 0;
                    changed |= ((event->mask & kEvents) != 0 &&
                                sources_.contains(event->wd)) ||
                               (event->mask & IN_Q_OVERFLOW) != 0;
                    if ((event->mask & IN_Q_OVERFLOW) != 0)
                    {
                        error = "监听事件溢出，正在重新核对整个备份范围";
                    }
                    offset += sizeof(inotify_event) + event->len;
                }
            }
            if (rescan)
            {
                if (changed)
                {
                    notify_(id, error, true);
                }
                // 事件发生后补齐新增目录的监听；期间遗漏由完整属性核对兜底。
                nextScan = std::min(nextScan, timer.elapsed() + 500);
            }
        }
        if (fd >= 0)
        {
            ::close(fd);
        }
    }
    QSet<int> sources_;
    QSet<int> seen_;
    QSet<int> watched_;
    QJsonObject task_;
    Notify notify_;
};
} // 命名空间

struct Realtime::Impl
{
    struct Task
    {
        QJsonObject config;
        std::unique_ptr<Watch> watch;
        bool dirty = true;
        bool running = false;
        bool locked = false;
        QString waiting;
        QString error;
        QString failureKind;
        QString monitorError;
        QString operation;
        qint64 first = 0;
        qint64 due = 0;
        int failures = 0;
        qint64 overflows = 0;
    };
    std::map<QString, Task> tasks;
    QHash<QString, QJsonObject> uncertain;
    QHash<QString, QDeadlineTimer> remoteSessions;

    bool needsAuthentication(const Task& task) const
    {
        const auto target = task.config.value("target").toObject();
        if (!remoteTarget(target))
        {
            return false;
        }
        const auto session = remoteSessions.constFind(endpointKey(target));
        return session == remoteSessions.cend() || session->hasExpired();
    }
    QTimer timer;
    QElapsedTimer clock;
    std::function<bool()> busy;
    Start start;
};

Realtime::Realtime(std::function<bool()> busy, Start start)
    : impl_(std::make_unique<Impl>())
{
    impl_->busy = std::move(busy);
    impl_->start = std::move(start);
    impl_->clock.start();
    connect(&impl_->timer, &QTimer::timeout, this,
            [this]
            {
                if (impl_->busy())
                {
                    return;
                }
                // 按到期时间挑选，避免持续修改的第一个任务饿死其他任务。
                auto chosen = impl_->tasks.end();
                for (auto it = impl_->tasks.begin(); it != impl_->tasks.end();
                     ++it)
                {
                    const auto& task = it->second;
                    if (task.dirty && !task.running && !task.locked &&
                        !impl_->needsAuthentication(task) &&
                        task.waiting.isEmpty() &&
                        task.due <= impl_->clock.elapsed() &&
                        (chosen == impl_->tasks.end() ||
                         task.due < chosen->second.due))
                    {
                        chosen = it;
                    }
                }
                if (chosen == impl_->tasks.end())
                {
                    return;
                }
                auto& task = chosen->second;
                task.dirty = false;
                task.running = true;
                task.first = 0;
                try
                {
                    impl_->start(chosen->first);
                }
                catch (const std::exception& error)
                {
                    completed({{"task_id", chosen->first},
                               {"state", "FAILED"},
                               {"error", QString::fromUtf8(error.what())}});
                }
            });
    impl_->timer.start(250);
}
Realtime::~Realtime() = default;

void Realtime::configure(const QJsonObject& config)
{
    QSet<QString> enabled;
    for (const auto& value : config.value("tasks").toArray())
    {
        const auto source = value.toObject();
        if (!source.value("realtime").toBool())
        {
            continue;
        }
        const auto id = text(source, "id");
        enabled.insert(id);
        auto signature = source;
        for (const auto& target : config.value("targets").toArray())
        {
            if (target.toObject().value("id") == source.value("target_id"))
            {
                signature.insert("target", target);
            }
        }
        auto& task = impl_->tasks[id];
        if (task.config == signature)
        {
            continue;
        }
        task.watch.reset();
        if (task.config.value("target") != signature.value("target"))
        {
            task.locked = false;
        }
        task.config = signature;
        const auto uncertain = impl_->uncertain.value(id);
        if (!uncertain.isEmpty())
        {
            task.waiting = uncertain.value("id").toString();
            task.error = uncertain.value("error").toString();
        }
        task.dirty = true;
        task.first = impl_->clock.elapsed();
        task.due = task.first + kQuietMs;
        auto notify = [this, signature](const QString& taskId,
                                        const QString& error, bool changed)
        {
            QMetaObject::invokeMethod(
                this,
                [this, taskId, error, changed, signature]
                {
                    const auto found = impl_->tasks.find(taskId);
                    if (found == impl_->tasks.end() ||
                        found->second.config != signature)
                    {
                        return;
                    }
                    auto& current = found->second;
                    current.monitorError = error;
                    if (error.startsWith("监听事件溢出"))
                    {
                        ++current.overflows;
                    }
                    if (!changed)
                    {
                        return;
                    }
                    const auto now = impl_->clock.elapsed();
                    if (!current.dirty)
                    {
                        current.first = now;
                    }
                    current.dirty = true;
                    current.monitorError = error;
                    if (current.failures == 0)
                    {
                        current.due = std::min(now + kQuietMs,
                                               current.first + kMaxDelayMs);
                    }
                },
                Qt::QueuedConnection);
        };
        task.watch = std::make_unique<Watch>(source, notify);
        task.watch->start();
    }
    for (auto it = impl_->tasks.begin(); it != impl_->tasks.end();)
    {
        if (!enabled.contains(it->first))
        {
            it = impl_->tasks.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void Realtime::completed(const QJsonObject& operation)
{
    const auto id = operation.value("task_id").toString();
    if (operation.value("state") == "WAITING")
    {
        impl_->uncertain.insert(id, operation);
    }
    else if (impl_->uncertain.value(id).value("id") == operation.value("id") &&
             operation.value("state").toString().startsWith("SUCCEEDED"))
    {
        impl_->uncertain.remove(id);
    }
    const auto found = impl_->tasks.find(id);
    if (found == impl_->tasks.end())
    {
        return;
    }
    auto& task = found->second;
    task.running = false;
    task.locked =
        operation.value("error").toString().startsWith("REPOSITORY_LOCKED:");
    task.operation = operation.value("id").toString();
    task.failureKind = operation.value("failure_kind").toString();
    if (operation.value("state").toString().startsWith("SUCCEEDED"))
    {
        task.error.clear();
        task.waiting = impl_->uncertain.value(id).value("id").toString();
        task.failures = 0;
    }
    else
    {
        task.error = operation.value("error").toString();
        task.dirty = true;
        task.failures = std::min(task.failures + 1, 5);
        task.due =
            impl_->clock.elapsed() + std::min(60000, 2000 << task.failures);
        if (operation.value("state") == "WAITING")
        {
            task.waiting = task.operation;
        }
    }
}

void Realtime::setRepositoryLocked(const QJsonObject& config,
                                   const QString& repository, bool locked)
{
    QSet<QString> targets;
    for (const auto& value : config.value("targets").toArray())
    {
        const auto target = value.toObject();
        if (target.value("repository_encryption_id") == repository)
        {
            targets.insert(text(target, "id"));
        }
    }
    for (auto& [id, task] : impl_->tasks)
    {
        if (targets.contains(task.config.value("target_id").toString()) ||
            (!locked && task.locked))
        {
            task.locked = locked;
            task.failures = 0;
            task.error.clear();
            task.due = impl_->clock.elapsed();
        }
    }
}

void Realtime::setAuthenticated(const QJsonObject& target, qint64 seconds)
{
    if (!remoteTarget(target))
    {
        return;
    }
    const auto key = endpointKey(target);
    if (seconds <= 0)
    {
        impl_->remoteSessions.remove(key);
        return;
    }
    for (auto& [id, task] : impl_->tasks)
    {
        const auto configured = task.config.value("target").toObject();
        if (remoteTarget(configured) && endpointKey(configured) == key)
        {
            task.dirty |= impl_->needsAuthentication(task);
            task.failures = 0;
            task.failureKind.clear();
            task.error.clear();
            task.due = impl_->clock.elapsed();
        }
    }
    impl_->remoteSessions.insert(key, QDeadlineTimer(seconds * 1000));
}

QJsonObject Realtime::status() const
{
    QJsonObject result;
    for (const auto& [id, task] : impl_->tasks)
    {
        const auto state =
            task.running                               ? "BACKING_UP"
            : impl_->needsAuthentication(task)         ? "WAITING_AUTH"
            : !task.waiting.isEmpty()                  ? "WAITING_CONFIRMATION"
            : task.locked                              ? "WAITING_UNLOCK"
            : task.failureKind == "WAITING_CONNECTION" ? "WAITING_CONNECTION"
            : task.failureKind == "CERTIFICATE_FAILED" ? "CERTIFICATE_FAILED"
            : task.failures > 0                        ? "RETRYING"
            : task.dirty                               ? "PENDING"
            : !task.monitorError.isEmpty()             ? "DEGRADED"
                                                       : "WATCHING";
        result.insert(id, QJsonObject{{"state", state},
                                      {"error", task.error},
                                      {"monitor_error", task.monitorError},
                                      {"operation_id", task.operation},
                                      {"overflow_count", task.overflows}});
    }
    return result;
}
} // 命名空间 backup::agent
