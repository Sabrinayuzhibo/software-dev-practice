#pragma once

#include "backup/agent/operations.h"

namespace backup::agent
{
class Channel;
// Null channel means scan: aggregate entry errors and mark incomplete. Uploads
// use the same type rules and stop on error. Neither mode follows symlinks.
QJsonObject sourceTree(const QJsonObject& source, Channel* channel,
                       const Progress& progress);
} // namespace backup::agent
