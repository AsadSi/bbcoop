// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <QDebug>
#include <QtEndian>
#include "stream_extractor.h"
#include "stun_server.h"

// The client's signaling frame: shadPS4 0.19 sends FF FF FF FF + payload ("narrow"), 0.20
// (gpu/bbnet20, net/p2p_codec.cpp FrameDatagram) FF 83 FF FF FF FF + payload ("wide"). bbcoop:
// both are accepted and the echo goes back in the framing of the ping.
static constexpr uint16_t SIGNALING_VPORT_NBO = 0xFFFF;
static constexpr int VPORT_HEADER_SIZE = 4;
static constexpr int WIDE_HEADER_SIZE = 6;

static bool IsWide(const QByteArray& data) {
    return data.size() >= WIDE_HEADER_SIZE && static_cast<uint8_t>(data[0]) == 0xFF &&
           static_cast<uint8_t>(data[1]) == 0x83 && static_cast<uint8_t>(data[2]) == 0xFF &&
           static_cast<uint8_t>(data[3]) == 0xFF && static_cast<uint8_t>(data[4]) == 0xFF &&
           static_cast<uint8_t>(data[5]) == 0xFF;
}

static QByteArray FrameSignaling(const QByteArray& payload, bool wide) {
    QByteArray framed;
    if (wide) {
        framed.append(QByteArray::fromHex("ff83ffffffff"));
    } else {
        framed.append(reinterpret_cast<const char*>(&SIGNALING_VPORT_NBO), 2);
        framed.append(reinterpret_cast<const char*>(&SIGNALING_VPORT_NBO), 2);
    }
    framed.append(payload);
    return framed;
}

static QByteArray StripVportHeader(const QByteArray& data, bool wide) {
    if (wide)
        return data.mid(WIDE_HEADER_SIZE);
    if (data.size() < VPORT_HEADER_SIZE)
        return {};
    uint16_t src_vp, dst_vp;
    memcpy(&src_vp, data.constData(), 2);
    memcpy(&dst_vp, data.constData() + 2, 2);
    if (src_vp != SIGNALING_VPORT_NBO || dst_vp != SIGNALING_VPORT_NBO)
        return {}; // not a signaling packet
    return data.mid(VPORT_HEADER_SIZE);
}

StunServer::StunServer(SharedState* shared, QObject* parent)
    : QObject(parent), m_socket(new QUdpSocket(this)), m_shared(shared) {
    connect(m_socket, &QUdpSocket::readyRead, this, &StunServer::OnReadyRead);
}

bool StunServer::Start(const QHostAddress& addr, uint16_t port) {
    if (!m_socket->bind(addr, port)) {
        qCritical() << "STUN UDP bind failed on" << addr.toString() << ":" << port
                    << m_socket->errorString();
        return false;
    }
    qInfo().nospace().noquote() << "STUN UDP listener on: " << addr.toString() << ":" << port;
    return true;
}

void StunServer::OnReadyRead() {
    while (m_socket->hasPendingDatagrams()) {
        QByteArray raw;
        raw.resize(static_cast<int>(m_socket->pendingDatagramSize()));
        QHostAddress sender;
        uint16_t senderPort = 0;
        m_socket->readDatagram(raw.data(), raw.size(), &sender, &senderPort);

        if (raw.isEmpty())
            continue;

        // Strip signaling vport header
        const bool wide = IsWide(raw);
        QByteArray data = StripVportHeader(raw, wide);
        if (data.isEmpty())
            continue;

        uint8_t cmd = static_cast<uint8_t>(data[0]);
        switch (cmd) {
        case 0x01:
            HandleStunPing(data, sender, senderPort, wide);
            break;
        default:
            break;
        }
    }
}

void StunServer::HandleStunPing(const QByteArray& data, const QHostAddress& sender,
                                uint16_t senderPort, bool wide) {
    // cmd(1) + online_id(16) + local_ip(4) = 21 bytes minimum
    if (data.size() < 21)
        return;

    // Extract null-padded online_id from bytes 1-16
    QByteArray rawId = data.mid(1, 16);
    int nullPos = rawId.indexOf('\0');
    QString npid = QString::fromUtf8(rawId.left(nullPos >= 0 ? nullPos : 16)).trimmed();
    if (npid.isEmpty())
        return;

    QString extIpStr = sender.toString();
    // Strip IPv6-mapped prefix if present (e.g. "::ffff:192.168.1.1" -> "192.168.1.1")
    if (extIpStr.startsWith("::ffff:"))
        extIpStr = extIpStr.mid(7);

    qInfo() << "STUN ping: npid=" << npid << "ext=" << extIpStr << ":" << senderPort;

    // Record external endpoint
    {
        QWriteLocker lk(&m_shared->matching.udpLock);
        m_shared->matching.udpExt[npid] = {extIpStr, senderPort};
    }

    // Reply: ext_ip(4 bytes network order) + ext_port(2 bytes network order)
    QHostAddress extAddr(extIpStr);
    uint32_t ipNet = qToBigEndian(static_cast<uint32_t>(extAddr.toIPv4Address()));
    uint16_t portNet = qToBigEndian(senderPort);

    QByteArray response;
    response.append(reinterpret_cast<const char*>(&ipNet), 4);
    response.append(reinterpret_cast<const char*>(&portNet), 2);
    m_socket->writeDatagram(FrameSignaling(response, wide), sender, senderPort);
}
