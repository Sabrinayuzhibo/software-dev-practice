#include "backup/core/io.h"

#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <unistd.h>

using namespace backup::core;

namespace
{
void rejects(const std::function<void()>& operation)
{
    try
    {
        operation();
    }
    catch (const Error&)
    {
        return;
    }
    throw Error("Expected operation to reject invalid input");
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    try
    {
        require(overlaps("/source", "/source/sub"),
                "Descendant overlap missed");
        require(!overlaps("/source", "/source-other"),
                "Path prefix is not containment");
        require(overlaps("/", "/source"), "Root overlap missed");
        for (const auto& path :
             {"../escape", "/absolute", "a//b", "a/../b", "./a"})
        {
            require(!validPath(path), "Unsafe manifest path accepted");
        }
        require(validPath(QString::fromUtf8("sub/中文 name.txt")),
                "Valid UTF-8 path rejected");
        rejects([] { number({{"size", -1}}, "size"); });
        rejects([] { number({{"size", 1.5}}, "size"); });
        rejects([] { number({{"size", "9223372036854775808"}}, "size"); });
        rejects([] { number({{"size", 9007199254740992.0}}, "size"); });
        require(number({{"size", "9223372036854775807"}}, "size") == INT64_MAX,
                "Exact decimal counter lost precision");

        QTemporaryDir directory;
        require(directory.isValid(), "Cannot create test directory");
        const auto root = directory.path();
        makeDirectory(root + "/inside");
        makeDirectory(root + "/outside");
        writeObject(root + "/outside/sentinel", {{"value", "untouched"}});
        Descriptor fd(::open(QFile::encodeName(root + "/inside").constData(),
                             O_RDONLY | O_DIRECTORY));
        require(::symlinkat("../outside", fd.get(), "link") == 0,
                "Cannot create test link");
        rejects(
            [&] {
                Descriptor escaped(
                    openBelow(fd.get(), "link/sentinel", O_WRONLY));
            });
        require(readObject(root + "/outside/sentinel").value("value") ==
                    "untouched",
                "Restore escaped through a link");
        rejects(
            [&] {
                Descriptor escaped(
                    openBelow(fd.get(), "../outside/sentinel", O_WRONLY));
            });
        const auto config = root + "/config.json";
        writeObject(config, {{"version", 1}});
        rejects(
            [&] {
                writeObject(config,
                            {{"oversized", QString(kMaxJsonBytes + 1, 'x')}});
            });
        require(readObject(config).value("version") == 1,
                "Failed write replaced valid configuration");
        rejects([] { parseObject("{truncated"); });
        std::cout << "PASS: path boundaries, symlink escape, exact counters, "
                     "atomic failed configuration write\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
