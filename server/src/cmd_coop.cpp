// SPDX-License-Identifier: GPL-2.0-or-later
// bbcoop: co-op sync relay. Lamp travel, progress flags, loot copies and deaths are not part of
// Bloodborne's own netcode; the clients exchange them through here. Each message goes to every
// other member of every room the sender is in, in order, over the connection already open.
#include <QDebug>
#include <QSet>
#include "client_session.h"
#include "proto_utils.h"
#include "shadnet.pb.h"

ErrorType ClientSession::CmdCoopRelay(StreamExtractor& data) {
    shadnet::CoopRelayRequest req;
    if (!decodeProto(req, data) || data.error())
        return ErrorType::Malformed;

    QSet<QString> targets;
    {
        QReadLocker lk(&m_shared->matching.roomsLock);
        for (const Room& room : m_shared->matching.rooms) {
            if (!room.findByNpid(m_info.npid))
                continue;
            for (const RoomMember& member : room.members) {
                if (member.npid != m_info.npid)
                    targets.insert(member.npid);
            }
        }
    }

    shadnet::NotifyCoopRelay notify;
    notify.set_from_npid(m_info.npid.toStdString());
    notify.set_data(req.data());
    QByteArray payload;
    appendProto(payload, notify);
    const QByteArray packet = BuildNotification(NotificationType::CoopRelay, payload);
    for (const QString& npid : targets)
        SendMatchingNotification([packet](uint32_t) { return packet; }, npid);
    return ErrorType::NoError;
}
