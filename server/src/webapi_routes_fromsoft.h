// SPDX-License-Identifier: GPL-2.0-or-later
// bbcoop: Bloodborne's FromSoft game-server endpoints (ss.info, login, messengers), answered here.
#pragma once
#include <QByteArray>
#include <QString>

class QHttpServer;

namespace WebApiRoutes {
// Base64 ss.info naming baseUrl for every api_* endpoint (exposed for tests).
QByteArray FromSoftServerInfo(const QString& baseUrl);
void RegisterFromSoftRoutes(QHttpServer& http);
} // namespace WebApiRoutes
