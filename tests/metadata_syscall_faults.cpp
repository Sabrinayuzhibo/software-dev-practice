// 仅供测试：模拟文件系统拒绝元数据写入，不向生产接口增加故障开关。
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
bool denied(int descriptor)
{
    if (!std::getenv("BACKUP_TEST_METADATA_FAULT"))
    {
        return false;
    }
    // 只拦截还原目标，配置、日志和历史记录仍执行真实持久化。
    char name[64]{};
    std::snprintf(name, sizeof(name), "/proc/self/fd/%d", descriptor);
    char path[4096]{};
    const auto count = ::readlink(name, path, sizeof(path) - 1);
    if (count > 0 && std::strstr(path, "/metadata-warning-restore"))
    {
        errno = EPERM;
        return true;
    }
    return false;
}

// 测试库始终要求系统符号存在；缺少时按正常系统调用失败报告。
template <typename Function> Function original(const char* name)
{
    auto function = reinterpret_cast<Function>(dlsym(RTLD_NEXT, name));
    if (!function)
    {
        errno = ENOSYS;
    }
    return function;
}
} // 匿名命名空间

extern "C" int fchown(int descriptor, uid_t uid, gid_t gid) noexcept
{
    if (denied(descriptor))
    {
        return -1;
    }
    static const auto call = original<decltype(&fchown)>("fchown");
    return call ? call(descriptor, uid, gid) : -1;
}

extern "C" int fchmod(int descriptor, mode_t mode) noexcept
{
    if (denied(descriptor))
    {
        return -1;
    }
    static const auto call = original<decltype(&fchmod)>("fchmod");
    return call ? call(descriptor, mode) : -1;
}

extern "C" int futimens(int descriptor, const timespec times[2]) noexcept
{
    if (denied(descriptor))
    {
        return -1;
    }
    static const auto call = original<decltype(&futimens)>("futimens");
    return call ? call(descriptor, times) : -1;
}

extern "C" int fchownat(int parent, const char* leaf, uid_t uid, gid_t gid,
                        int flags) noexcept
{
    if (denied(parent))
    {
        return -1;
    }
    static const auto call = original<decltype(&fchownat)>("fchownat");
    return call ? call(parent, leaf, uid, gid, flags) : -1;
}

extern "C" int fchmodat(int parent, const char* leaf, mode_t mode,
                        int flags) noexcept
{
    if (denied(parent))
    {
        return -1;
    }
    static const auto call = original<decltype(&fchmodat)>("fchmodat");
    return call ? call(parent, leaf, mode, flags) : -1;
}

extern "C" int utimensat(int parent, const char* leaf, const timespec times[2],
                         int flags) noexcept
{
    if (denied(parent))
    {
        return -1;
    }
    static const auto call = original<decltype(&utimensat)>("utimensat");
    return call ? call(parent, leaf, times, flags) : -1;
}
