// Test-only LD_PRELOAD shim. Fail one named restore leaf without production
// hooks.
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
bool fail(const char* kind, const char* leaf)
{
    const auto* selected = std::getenv("BACKUP_TEST_FAIL_TYPE");
    const auto* path = std::getenv("BACKUP_TEST_FAIL_LEAF");
    if (selected && path && std::strcmp(selected, kind) == 0 &&
        std::strcmp(path, leaf) == 0)
    {
        errno = EOPNOTSUPP;
        return true;
    }
    return false;
}
} // namespace

extern "C" int linkat(int source, const char* path, int destination,
                      const char* leaf, int flags) noexcept
{
    if (fail("hardlink", leaf))
    {
        return -1;
    }
    static const auto original =
        reinterpret_cast<decltype(&linkat)>(dlsym(RTLD_NEXT, "linkat"));
    if (!original)
    {
        errno = ENOSYS;
        return -1;
    }
    return original(source, path, destination, leaf, flags);
}

extern "C" int symlinkat(const char* target, int parent,
                         const char* leaf) noexcept
{
    if (fail("symlink", leaf))
    {
        return -1;
    }
    static const auto original =
        reinterpret_cast<decltype(&symlinkat)>(dlsym(RTLD_NEXT, "symlinkat"));
    if (!original)
    {
        errno = ENOSYS;
        return -1;
    }
    return original(target, parent, leaf);
}

extern "C" int mkfifoat(int parent, const char* leaf, mode_t mode) noexcept
{
    if (fail("fifo", leaf))
    {
        return -1;
    }
    static const auto original =
        reinterpret_cast<decltype(&mkfifoat)>(dlsym(RTLD_NEXT, "mkfifoat"));
    if (!original)
    {
        errno = ENOSYS;
        return -1;
    }
    return original(parent, leaf, mode);
}

extern "C" int renameat2(int source, const char* path, int destination,
                          const char* leaf, unsigned int flags) noexcept
{
    if (fail("socket", leaf))
    {
        return -1;
    }
    static const auto original =
        reinterpret_cast<decltype(&renameat2)>(dlsym(RTLD_NEXT, "renameat2"));
    if (!original)
    {
        errno = ENOSYS;
        return -1;
    }
    return original(source, path, destination, leaf, flags);
}
