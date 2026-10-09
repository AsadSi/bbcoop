// SPDX-License-Identifier: GPL-2.0-or-later
// bbcoop: Bloodborne's FromSoft game server, private. The game's boot fetch of ss.info (redirected
// here by the client, see http.cpp) gets an endpoint list naming this server for every api_*;
// login hands out the SessionId/UserId every later request carries; everything else succeeds with
// nothing in it. /summon_messenger/* stays in webapi_routes_bloodborne.cpp.
//
// Response shapes beyond {"ResKind":0,"MessageId":"<Name>Response"} are unverified: the game's
// eboot holds the JSON keys it reads (check them against a dump before relying on lists).
#include "webapi_routes_fromsoft.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QHash>
#include <QHttpServer>
#include <QHttpServerRequest>
#include <QHttpServerResponse>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QStringList>

namespace WebApiRoutes {
namespace {

QJsonObject Body(const QHttpServerRequest& request) {
    return QJsonDocument::fromJson(request.body()).object();
}

QHttpServerResponse Json(const QJsonObject& body) {
    return QHttpServerResponse{"application/json", QJsonDocument(body).toJson(QJsonDocument::Compact),
                               QHttpServerResponse::StatusCode::Ok};
}

// {"ResKind":0,"MessageId":"<Name>Response"} for a "<Name>Request", as the summon routes answer.
QJsonObject Success(const QJsonObject& request, const QString& fallbackName) {
    QString name = request.value(QStringLiteral("MessageId")).toString();
    if (name.endsWith(QStringLiteral("Request"))) name.chop(7);
    if (name.isEmpty()) name = fallbackName;
    name += QStringLiteral("Response");
    QJsonObject body;
    body.insert(QStringLiteral("ResKind"), 0);
    body.insert(QStringLiteral("MessageId"), name);
    return body;
}

// The same player gets the same UserId every session (summon signs are keyed by it).
qint64 StableUserId(const QJsonObject& login) {
    const QByteArray account = login.value(QStringLiteral("PlatformAccountId")).toVariant().toString().toUtf8();
    const QByteArray digest = QCryptographicHash::hash(account, QCryptographicHash::Sha256);
    qint64 id = 0;
    for (int i = 0; i < 4; ++i) id = (id << 8) | static_cast<unsigned char>(digest[i]);
    id &= 0x7FFFFFFF; // fits a signed 32-bit field, in case the game keeps it in one
    return id ? id : 1;
}

// The lists a response must carry even when empty: the game rejects a response without them
// ("Failed to record note search information" for BloodMessSearchAdd). Keys from the eboot's
// UTF-16 response key table (CUSA00900 1.09, file offset 0x49d0d40), by request MessageId.
void EmptyFields(const QString& message, QJsonObject& out) {
    static const QHash<QString, QStringList> lists = {
        {QStringLiteral("BloodMessSearchAddRequest"), {QStringLiteral("LostBloodMessIdList")}},
        {QStringLiteral("BloodMessGetEvaluateRequest"), {QStringLiteral("BloodMessEvaluationList")}},
        {QStringLiteral("BloodMessGetListRequest"), {QStringLiteral("BloodMessList")}},
        {QStringLiteral("ChairMessGetListRequest"), {QStringLiteral("ChairMessList")}},
        {QStringLiteral("WanderingGhostGetRequest"), {QStringLiteral("WanderingGhostList")}},
        {QStringLiteral("TombMessGetListRequest"), {QStringLiteral("TombMessList")}},
        {QStringLiteral("NoticeNormalGetRequest"), {QStringLiteral("NoticeList")}},
        {QStringLiteral("NoticeEmergencyGetRequest"), {QStringLiteral("NoticeList")}},
        {QStringLiteral("ChannelGetInfoRequest"), {QStringLiteral("ChannelInfoList")}},
        {QStringLiteral("ChannelSearchRequest"), {QStringLiteral("ChannelList")}},
        {QStringLiteral("ChannelWordSearchRequest"), {QStringLiteral("ChannelList")}},
        {QStringLiteral("ChannelRandomJoinRequest"), {QStringLiteral("ChannelList")}},
    };
    for (const QString& key : lists.value(message)) out.insert(key, QJsonArray{});
    if (message == QStringLiteral("ChairMessGetListRequest")) out.insert(QStringLiteral("ChairMessCount"), 0);
    if (message == QStringLiteral("UserAgreementGetRequest")) out.insert(QStringLiteral("UserAgreementText"), QString());
}

} // namespace

QByteArray FromSoftServerInfo(const QString& baseUrl) {
    QFile file(QStringLiteral(":/fromsoft_ss_info.xml"));
    if (!file.open(QIODevice::ReadOnly)) return {};
    QByteArray xml = file.readAll();
    xml.replace("@BASE@", baseUrl.toUtf8());
    return xml.toBase64();
}

void RegisterFromSoftRoutes(QHttpServer& http) {
    // The api_* base is whatever address the game used to reach us (its Host header), so the
    // same server works on a LAN, over Tailscale or on localhost.
    http.route("/<arg>/ss.info", QHttpServerRequest::Method::Get,
               [](const QString&, const QHttpServerRequest& request) {
                   const QString host = QString::fromUtf8(request.value("Host"));
                   const QString base = QStringLiteral("http://") +
                                        (host.isEmpty() ? request.localAddress().toString() : host);
                   return QHttpServerResponse{"text/plain", FromSoftServerInfo(base),
                                              QHttpServerResponse::StatusCode::Ok};
               });

    http.route("/basic_utils/login", QHttpServerRequest::Method::Post, [](const QHttpServerRequest& request) {
        const QJsonObject in = Body(request);
        QJsonObject out = Success(in, QStringLiteral("Login"));
        const quint64 session = QRandomGenerator::global()->generate64();
        out.insert(QStringLiteral("SessionId"), QString::number(session, 16));
        out.insert(QStringLiteral("UserId"), StableUserId(in));
        return Json(out);
    });

    // The game asks for CharaIdNum character ids before it plays online ("Failed to reset online
    // character data" without them). Keys from the eboot's UTF-16 key table (CUSA00900 1.09, file
    // offset 0x49d0dba): PublishCharacterIdList of PublishCharaId. Ids are stable per user.
    http.route("/basic_utils/sync_chara_id", QHttpServerRequest::Method::Post, [](const QHttpServerRequest& request) {
        const QJsonObject in = Body(request);
        QJsonObject out = Success(in, QStringLiteral("SyncCharaId"));
        const qint64 user = in.value(QStringLiteral("UserId")).toVariant().toLongLong();
        const int count = qBound(0, in.value(QStringLiteral("CharaIdNum")).toInt(), 16);
        QJsonArray ids;
        for (int i = 0; i < count; ++i) {
            QJsonObject id;
            id.insert(QStringLiteral("PublishCharaId"), (user & 0x7FFFFFF) * 16 + i + 1);
            ids.append(id);
        }
        out.insert(QStringLiteral("PublishCharacterIdList"), ids);
        return Json(out);
    });

    http.route("/basic_utils/get_datetime", QHttpServerRequest::Method::Post, [](const QHttpServerRequest& request) {
        QJsonObject out = Success(Body(request), QStringLiteral("ServerTimeGet"));
        out.insert(QStringLiteral("ServerTime"), QDateTime::currentSecsSinceEpoch());
        return Json(out);
    });

    // Everything else: messages, bloodstains, ghosts, lamps, chalice, penalties, notices. Accepted
    // and answered empty, so the game never takes an error path.
    static const QStringList succeed = {
        "/basic_utils/get_normal_notice", "/basic_utils/get_emergency_notice",
        "/basic_utils/get_user_agreement",
        "/blood_messenger/create", "/blood_messenger/delete", "/blood_messenger/evaluate_message",
        "/blood_messenger/evaluation", "/blood_messenger/exist_messages", "/blood_messenger/message_area",
        "/tomb_messenger/create", "/tomb_messenger/delete", "/tomb_messenger/message_area",
        "/tomb_messenger/death_vision_get",
        "/wandering_ghost/create", "/wandering_ghost/get",
        "/chair_messenger/get", "/chair_messenger/update",
        "/messenger_shell/upload",
        "/penalty/notify_multi_play_error", "/penalty/notify_user_properties_move_count",
        "/penalty/check_user_priority_move_count",
        "/channel/get_info", "/channel/upload", "/channel/share", "/channel/search", "/channel/word_search",
        "/channel/get_details_info", "/channel/random_join", "/channel/add_material",
        "/channel/notify_add_material_complete",
    };
    for (const QString& path : succeed) {
        const QString name = path.section('/', -1);
        http.route(path, QHttpServerRequest::Method::Post, [name](const QHttpServerRequest& request) {
            const QJsonObject in = Body(request);
            QJsonObject out = Success(in, name);
            EmptyFields(in.value(QStringLiteral("MessageId")).toString(), out);
            return Json(out);
        });
    }
}

} // namespace WebApiRoutes
