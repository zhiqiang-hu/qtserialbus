﻿/****************************************************************************
**
** Copyright (C) 2015 Denis Shienkov <denis.shienkov@gmail.com>
** Copyright (C) 2015 The Qt Company Ltd.
** Contact: http://www.qt.io/licensing/
**
** This file is part of the QtSerialBus module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL3$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see http://www.qt.io/terms-conditions. For further
** information use the contact form at http://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 3 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPLv3 included in the
** packaging of this file. Please review the following information to
** ensure the GNU Lesser General Public License version 3 requirements
** will be met: https://www.gnu.org/licenses/lgpl.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 2.0 or later as published by the Free
** Software Foundation and appearing in the file LICENSE.GPL included in
** the packaging of this file. Please review the following information to
** ensure the GNU General Public License version 2.0 requirements will be
** met: http://www.gnu.org/licenses/gpl-2.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "peakcanbackend.h"
#include "peakcanbackend_p.h"
#include "peakcan_symbols_p.h"

#include <QtSerialBus/qcanbusdevice.h>

#include <QtCore/qtimer.h>
#include <QtCore/qcoreevent.h>

#include <algorithm>

#ifdef Q_OS_WIN32
#include <QtCore/qwineventnotifier.h>
#else
#include <QtCore/qsocketnotifier.h>
#endif

QT_BEGIN_NAMESPACE

#ifndef LINK_LIBPCANBASIC
Q_GLOBAL_STATIC(QLibrary, pcanLibrary)
#endif

bool PeakCanBackend::canCreate(QString *errorReason)
{
#ifdef LINK_LIBPCANBASIC
    return true;
#else
    static bool symbolsResolved = resolveSymbols(pcanLibrary());
    if (!symbolsResolved) {
        *errorReason = tr("The PCAN runtime library is not found");
        return false;
    }
    return true;
#endif
}

struct PcanChannel{
    char        name[6];
    TPCANHandle index;
};
PcanChannel pcanChannels[] = {
    { "usb0",  PCAN_USBBUS1  },
    { "usb1",  PCAN_USBBUS2  },
    { "usb2",  PCAN_USBBUS3  },
    { "usb3",  PCAN_USBBUS4  },
    { "usb4",  PCAN_USBBUS5  },
    { "usb5",  PCAN_USBBUS6  },
    { "usb6",  PCAN_USBBUS7  },
    { "usb7",  PCAN_USBBUS8  },
    { "usb8",  PCAN_USBBUS9  },
    { "usb9",  PCAN_USBBUS10 },
    { "usb10", PCAN_USBBUS11 },
    { "usb11", PCAN_USBBUS12 },
    { "usb12", PCAN_USBBUS13 },
    { "usb13", PCAN_USBBUS14 },
    { "usb14", PCAN_USBBUS15 },
    { "usb15", PCAN_USBBUS16 },
    { "pci0",  PCAN_PCIBUS1  },
    { "pci1",  PCAN_PCIBUS2  },
    { "pci2",  PCAN_PCIBUS3  },
    { "pci3",  PCAN_PCIBUS4  },
    { "pci4",  PCAN_PCIBUS5  },
    { "pci5",  PCAN_PCIBUS6  },
    { "pci6",  PCAN_PCIBUS7  },
    { "pci7",  PCAN_PCIBUS8  },
    { "pci8",  PCAN_PCIBUS9  },
    { "pci9",  PCAN_PCIBUS10 },
    { "pci10", PCAN_PCIBUS11 },
    { "pci11", PCAN_PCIBUS12 },
    { "pci12", PCAN_PCIBUS13 },
    { "pci13", PCAN_PCIBUS14 },
    { "pci14", PCAN_PCIBUS15 },
    { "pci15", PCAN_PCIBUS16 },
    { "none",  PCAN_NONEBUS  }
};

#if defined(Q_OS_WIN32)
class ReadNotifier : public QWinEventNotifier
{
    // no Q_OBJECT macro!
public:
    explicit ReadNotifier(PeakCanBackendPrivate *d, QObject *parent)
        : QWinEventNotifier(parent)
        , dptr(d)
    {
        setHandle(dptr->readHandle);
    }

protected:
    bool event(QEvent *e) override
    {
        if (e->type() == QEvent::WinEventAct) {
            dptr->startRead();
            return true;
        }
        return QWinEventNotifier::event(e);
    }

private:
    PeakCanBackendPrivate *dptr;
};
#else
class ReadNotifier : public QSocketNotifier
{
    // no Q_OBJECT macro!
public:
    explicit ReadNotifier(PeakCanBackendPrivate *d, QObject *parent)
        : QSocketNotifier(d->readHandle, QSocketNotifier::Read, parent)
        , dptr(d)
    {
    }

protected:
    bool event(QEvent *e) override
    {
        if (e->type() == QEvent::SockAct) {
            dptr->startRead();
            return true;
        }
        return QSocketNotifier::event(e);
    }

private:
    PeakCanBackendPrivate *dptr;
};
#endif

class WriteNotifier : public QTimer
{
    // no Q_OBJECT macro!
public:
    WriteNotifier(PeakCanBackendPrivate *d, QObject *parent)
        : QTimer(parent)
        , dptr(d)
    {
    }

protected:
    void timerEvent(QTimerEvent *e) override
    {
        if (e->timerId() == timerId()) {
            dptr->startWrite();
            return;
        }
        QTimer::timerEvent(e);
    }

private:
    PeakCanBackendPrivate *dptr;
};

PeakCanBackendPrivate::PeakCanBackendPrivate(PeakCanBackend *q)
    : q_ptr(q)
    , isOpen(false)
    , channelIndex(PCAN_NONEBUS)
    , writeNotifier(nullptr)
    , readNotifier(nullptr)
#if defined(Q_OS_WIN32)
    , readHandle(INVALID_HANDLE_VALUE)
#else
    , readHandle(-1)
#endif
{
}

struct BitrateItem
{
    int bitrate;
    int code;
};

struct BitrateLessFunctor
{
    bool operator()( const BitrateItem &item1, const BitrateItem &item2) const
    {
        return item1.bitrate < item2.bitrate;
    }
};

static int bitrateCodeFromBitrate(int bitrate)
{
    static const BitrateItem bitratetable[] = {
        { 5000, PCAN_BAUD_5K },
        { 10000, PCAN_BAUD_10K },
        { 20000, PCAN_BAUD_20K },
        { 33000, PCAN_BAUD_33K },
        { 47000, PCAN_BAUD_47K },
        { 50000, PCAN_BAUD_50K },
        { 83000, PCAN_BAUD_83K },
        { 95000, PCAN_BAUD_95K },
        { 100000, PCAN_BAUD_100K },
        { 125000, PCAN_BAUD_125K },
        { 250000, PCAN_BAUD_250K },
        { 500000, PCAN_BAUD_500K },
        { 800000, PCAN_BAUD_800K },
        { 1000000, PCAN_BAUD_1M }
    };

    static const BitrateItem *endtable = bitratetable + (sizeof(bitratetable) / sizeof(*bitratetable));

    const BitrateItem item = { bitrate , 0 };
    const BitrateItem *where = std::lower_bound(bitratetable, endtable, item, BitrateLessFunctor());
    return where != endtable ? where->code : -1;
}

bool PeakCanBackendPrivate::open()
{
    Q_Q(PeakCanBackend);

    const int bitrate = q->configurationParameter(QCanBusDevice::BitRateKey).toInt();
    const int bitrateCode = bitrateCodeFromBitrate(bitrate);

    const TPCANStatus st = ::CAN_Initialize(channelIndex, bitrateCode, 0, 0, 0);
    if (st != PCAN_ERROR_OK) {
        q->setError(systemErrorString(st), QCanBusDevice::ConnectionError);
        return false;
    }

#if defined(Q_OS_WIN32)
    if (readHandle == INVALID_HANDLE_VALUE) {
        readHandle = ::CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!readHandle) {
            q->setError(qt_error_string(::GetLastError()), QCanBusDevice::ConnectionError);
            return false;
        }
    }
#endif

    const TPCANStatus err = ::CAN_SetValue(channelIndex, PCAN_RECEIVE_EVENT, &readHandle, sizeof(readHandle));
    if (err != PCAN_ERROR_OK) {
        q->setError(systemErrorString(err), QCanBusDevice::ConnectionError);
        return false;
    }

    writeNotifier = new WriteNotifier(this, q);
    writeNotifier->setInterval(0);

    readNotifier = new ReadNotifier(this, q);
    readNotifier->setEnabled(true);

    isOpen = true;
    return true;
}

void PeakCanBackendPrivate::close()
{
    Q_Q(PeakCanBackend);

    delete readNotifier;
    readNotifier = nullptr;

    delete writeNotifier;
    writeNotifier = nullptr;

    quint32 value = 0;
    const TPCANStatus err = ::CAN_SetValue(channelIndex, PCAN_RECEIVE_EVENT, &value, sizeof(value));
    if (err != PCAN_ERROR_OK)
        emit q->setError(systemErrorString(err), QCanBusDevice::ConnectionError);

    const TPCANStatus st = ::CAN_Uninitialize(channelIndex);
    if (st != PCAN_ERROR_OK)
        q->setError(systemErrorString(st), QCanBusDevice::ConnectionError);

#if defined(Q_OS_WIN32)
    if (readHandle && (readHandle != INVALID_HANDLE_VALUE)) {
        if (!::CloseHandle(readHandle))
            q->setError(qt_error_string(::GetLastError()), QCanBusDevice::ConnectionError);
        readHandle = INVALID_HANDLE_VALUE;
    }
#else
    readHandle = -1;
#endif

    isOpen = false;
}

bool PeakCanBackendPrivate::setConfigurationParameter(int key, const QVariant &value)
{
    Q_Q(PeakCanBackend);

    switch (key) {
    case QCanBusDevice::BitRateKey:
        return verifyBitRate(value.toInt());
    default:
        q->setError(PeakCanBackend::tr("Unsupported configuration key: %1").arg(key),
                    QCanBusDevice::ConfigurationError);
        return false;
    }
}

void PeakCanBackendPrivate::setupChannel(const QByteArray &interfaceName)
{
    const PcanChannel *chn = pcanChannels;
    while (chn->index != PCAN_NONEBUS && chn->name != interfaceName)
        ++chn;
    channelIndex = chn->index;
}

// Calls only when the device is closed
void PeakCanBackendPrivate::setupDefaultConfigurations()
{
    Q_Q(PeakCanBackend);

    q->setConfigurationParameter(QCanBusDevice::BitRateKey, 500000);
}

QString PeakCanBackendPrivate::systemErrorString(int errorCode)
{
    QByteArray buffer(256, 0);
    if (::CAN_GetErrorText(errorCode, 0, buffer.data()) != PCAN_ERROR_OK)
        return PeakCanBackend::tr("Unable to retrieve an error string");
    return QString::fromLatin1(buffer);
}

void PeakCanBackendPrivate::startWrite()
{
    Q_Q(PeakCanBackend);

    if (!q->hasOutgoingFrames()) {
        writeNotifier->stop();
        return;
    }

    const QCanBusFrame frame = q->dequeueOutgoingFrame();
    const QByteArray payload = frame.payload();

    TPCANMsg message;
    ::memset(&message, 0, sizeof(message));

    message.ID = frame.frameId();
    message.LEN = payload.size();
    message.MSGTYPE = frame.hasExtendedFrameFormat() ? PCAN_MESSAGE_EXTENDED : PCAN_MESSAGE_STANDARD;

    if (frame.frameType() == QCanBusFrame::RemoteRequestFrame)
        message.MSGTYPE |= PCAN_MESSAGE_RTR; // we do not care about the payload
    else
        ::memcpy(message.DATA, payload.constData(), sizeof(message.DATA));

    const TPCANStatus st = ::CAN_Write(channelIndex, &message);
    if (st != PCAN_ERROR_OK)
        q->setError(systemErrorString(st), QCanBusDevice::WriteError);
    else
        emit q->framesWritten(qint64(1));

    if (q->hasOutgoingFrames() && !writeNotifier->isActive())
        writeNotifier->start();
}

void PeakCanBackendPrivate::startRead()
{
    Q_Q(PeakCanBackend);

    QVector<QCanBusFrame> newFrames;

    forever {
        TPCANMsg message;
        ::memset(&message, 0, sizeof(message));
        TPCANTimestamp timestamp;
        ::memset(&timestamp, 0, sizeof(timestamp));

        const TPCANStatus st = ::CAN_Read(channelIndex, &message, &timestamp);
        if (st != PCAN_ERROR_OK) {
            if (st != PCAN_ERROR_XMTFULL)
                q->setError(systemErrorString(st), QCanBusDevice::ReadError);
            break;
        }

        QCanBusFrame frame(message.ID, QByteArray(reinterpret_cast<const char *>(message.DATA), int(message.LEN)));
        frame.setTimeStamp(QCanBusFrame::TimeStamp(timestamp.millis / 1000, timestamp.micros));
        frame.setExtendedFrameFormat(message.MSGTYPE & PCAN_MESSAGE_EXTENDED);
        frame.setFrameType((message.MSGTYPE & PCAN_MESSAGE_RTR) ? QCanBusFrame::RemoteRequestFrame : QCanBusFrame::DataFrame);

        newFrames.append(frame);
    }

    q->enqueueReceivedFrames(newFrames);
}

bool PeakCanBackendPrivate::verifyBitRate(int bitrate)
{
    Q_Q(PeakCanBackend);

    if (isOpen) {
        q->setError(PeakCanBackend::tr("Impossible to reconfigure bitrate for the opened device"),
                    QCanBusDevice::ConfigurationError);
        return false;
    }

    if (bitrateCodeFromBitrate(bitrate) == -1) {
        q->setError(PeakCanBackend::tr("Unsupported bitrate value"),
                    QCanBusDevice::ConfigurationError);
        return false;
    }

    return true;
}

PeakCanBackend::PeakCanBackend(const QString &name, QObject *parent)
    : QCanBusDevice(parent)
    , d_ptr(new PeakCanBackendPrivate(this))
{
    Q_D(PeakCanBackend);

    d->setupChannel(name.toLatin1());
    d->setupDefaultConfigurations();
}

PeakCanBackend::~PeakCanBackend()
{
    Q_D(PeakCanBackend);

    if (d->isOpen)
        close();

    delete d_ptr;
}

bool PeakCanBackend::open()
{
    Q_D(PeakCanBackend);

    if (!d->isOpen) {
        if (!d->open())
            return false;

        // apply all stored configurations except bitrate, because
        // the bitrate can not be applied after opening of device
        foreach (int key, configurationKeys()) {
            if (key == QCanBusDevice::BitRateKey)
                continue;
            const QVariant param = configurationParameter(key);
            const bool success = d->setConfigurationParameter(key, param);
            if (!success) {
                qWarning() << "Cannot apply parameter:" << key
                           << "with value:" << param;
            }
        }
    }

    setState(QCanBusDevice::ConnectedState);
    return true;
}

void PeakCanBackend::close()
{
    Q_D(PeakCanBackend);

    d->close();

    setState(QCanBusDevice::UnconnectedState);
}

void PeakCanBackend::setConfigurationParameter(int key, const QVariant &value)
{
    Q_D(PeakCanBackend);

    if (d->setConfigurationParameter(key, value))
        QCanBusDevice::setConfigurationParameter(key, value);
}

bool PeakCanBackend::writeFrame(const QCanBusFrame &newData)
{
    Q_D(PeakCanBackend);

    if (state() != QCanBusDevice::ConnectedState)
        return false;

    if (!newData.isValid()) {
        setError(tr("Cannot write invalid QCanBusFrame"), QCanBusDevice::WriteError);
        return false;
    }

    if (newData.frameType() != QCanBusFrame::DataFrame
            && newData.frameType() != QCanBusFrame::RemoteRequestFrame) {
        setError(tr("Unable to write a frame with unacceptable type"),
                 QCanBusDevice::WriteError);
        return false;
    }

    // CAN FD frame format not implemented at this stage
    if (newData.payload().size() > 8) {
        setError(tr("CAN FD frame format not supported."), QCanBusDevice::WriteError);
        return false;
    }

    enqueueOutgoingFrame(newData);

    if (!d->writeNotifier->isActive())
        d->writeNotifier->start();

    return true;
}

// TODO: Implement me
QString PeakCanBackend::interpretErrorFrame(const QCanBusFrame &errorFrame)
{
    Q_UNUSED(errorFrame);

    return QString();
}

QT_END_NAMESPACE
