/* Copyright (C) 2006 - 2012 Jan Kundrát <jkt@flaska.net>

   This file is part of the Trojita Qt IMAP e-mail client,
   http://trojita.flaska.net/

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public
   License as published by the Free Software Foundation; either
   version 2 of the License, or the version 3 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; see the file COPYING.  If not, write to
   the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
   Boston, MA 02110-1301, USA.
*/

#include "MessageComposer.h"
#include <QBuffer>
#include <QCoreApplication>
#include <QMimeData>
#include <QUrl>
#include <QUuid>
#include "Imap/Encoders.h"
#include "Imap/Model/ComposerAttachments.h"
#include "Imap/Model/Model.h"
#include "Imap/Model/Utils.h"

namespace Imap {
namespace Mailbox {

MessageComposer::MessageComposer(Model *model, QObject *parent) :
    QAbstractListModel(parent), m_model(model), m_shouldPreload(false)
{
}

MessageComposer::~MessageComposer()
{
    qDeleteAll(m_attachments);
}

int MessageComposer::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_attachments.size();
}

QVariant MessageComposer::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.column() != 0 || index.row() < 0 || index.row() >= m_attachments.size())
        return QVariant();

    switch (role) {
    case Qt::DisplayRole:
        return m_attachments[index.row()]->caption();
    case Qt::ToolTipRole:
        return m_attachments[index.row()]->tooltip();
    }
    return QVariant();
}

Qt::DropActions MessageComposer::supportedDropActions() const
{
    return Qt::CopyAction | Qt::MoveAction;
}

Qt::ItemFlags MessageComposer::flags(const QModelIndex &index) const
{
    Qt::ItemFlags f = QAbstractListModel::flags(index);

    if (index.isValid()) {
        f |= Qt::ItemIsDragEnabled;
    }
    f |= Qt::ItemIsDropEnabled;
    return f;
}

QMimeData *MessageComposer::mimeData(const QModelIndexList &indexes) const
{
    QByteArray encodedData;
    QDataStream stream(&encodedData, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_4_6);

    QList<AttachmentItem*> items;
    Q_FOREACH(const QModelIndex &index, indexes) {
        if (index.model() != this || !index.isValid() || index.column() != 0 || index.parent().isValid())
            continue;
        if (index.row() < 0 > index.row() >= m_attachments.size())
            continue;
        items << m_attachments[index.row()];
    }

    if (items.isEmpty())
        return 0;

    stream << items.size();
    Q_FOREACH(const AttachmentItem *attachment, items) {
        attachment->asDroppableMimeData(stream);
    }
    QMimeData *res = new QMimeData();
    res->setData(QLatin1String("application/x-trojita-attachments-list"), encodedData);
    return res;
}

bool MessageComposer::dropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column, const QModelIndex &parent)
{
    if (action == Qt::IgnoreAction)
        return true;

    if (column > 0)
        return false;

    if (!m_model)
        return false;

    Q_UNUSED(row);
    Q_UNUSED(parent);
    // FIXME: would be cool to support attachment reshuffling and to respect the desired drop position

    static QString xTrojitaAttachmentList = QLatin1String("application/x-trojita-attachments-list");
    static QString xTrojitaMessageList = QLatin1String("application/x-trojita-message-list");
    static QString xTrojitaImapPart = QLatin1String("application/x-trojita-imap-part");

    if (data->hasFormat(xTrojitaAttachmentList)) {
        QByteArray encodedData = data->data(xTrojitaAttachmentList);
        QDataStream stream(&encodedData, QIODevice::ReadOnly);
        return dropAttachmentList(stream);
    } else if (data->hasFormat(xTrojitaMessageList)) {
        QByteArray encodedData = data->data(xTrojitaMessageList);
        QDataStream stream(&encodedData, QIODevice::ReadOnly);
        return dropImapMessage(stream);
    } else if (data->hasFormat(xTrojitaImapPart)) {
        QByteArray encodedData = data->data(xTrojitaImapPart);
        QDataStream stream(&encodedData, QIODevice::ReadOnly);
        return dropImapPart(stream);
    } else {
        bool attached = false;
        QList<QUrl> urls = data->urls();
        foreach (const QUrl &url, urls) {
            if (url.isLocalFile()) {
                addFileAttachment(url.path());
                attached = true;
            }
        }
        return attached;
    }
}

/** @short Container wrapper which calls qDeleteAll on all items which remain in the list at the time of destruction */
template <typename T>
class WillDeleteAll {
public:
    T d;
    ~WillDeleteAll() {
        qDeleteAll(d);
    }
};

/** @short Handle a drag-and-drop of a list of attachments */
bool MessageComposer::dropAttachmentList(QDataStream &stream)
{
    stream.setVersion(QDataStream::Qt_4_6);
    if (stream.atEnd()) {
        qDebug() << "drag-and-drop: cannot decode data: end of stream";
        return false;
    }
    int num;
    stream >> num;
    if (stream.status() != QDataStream::Ok) {
        qDebug() << "drag-and-drop: stream failed:" << stream.status();
        return false;
    }
    if (num < 0) {
        qDebug() << "drag-and-drop: invalid number of items";
        return false;
    }

    // A crude RAII here; there are many places where the validation might fail even though we have already allocated memory
    WillDeleteAll<QList<AttachmentItem*> > items;

    for (int i = 0; i < num; ++i) {
        int kind = -1;
        stream >> kind;

        switch (kind) {
        case AttachmentItem::ATTACHMENT_IMAP_MESSAGE:
        {
            QString mailbox;
            uint uidValidity;
            QList<uint> uids;
            stream >> mailbox >> uidValidity >> uids;
            if (!validateDropImapMessage(stream, mailbox, uidValidity, uids))
                return false;
            if (uids.size() != 1) {
                qDebug() << "drag-and-drop: malformed data for a single message in a mixed list: too many UIDs";
                return false;
            }
            items.d << new ImapMessageAttachmentItem(m_model, mailbox, uidValidity, uids.front());
            break;
        }

        case AttachmentItem::ATTACHMENT_IMAP_PART:
        {
            QString mailbox;
            uint uidValidity;
            uint uid;
            QString partId;
            QString trojitaPath;
            if (!validateDropImapPart(stream, mailbox, uidValidity, uid, partId, trojitaPath))
                return false;
            items.d << new ImapPartAttachmentItem(m_model, mailbox, uidValidity, uid, partId, trojitaPath);
            break;
        }

        case AttachmentItem::ATTACHMENT_FILE:
        {
            QString fileName;
            stream >> fileName;
            items.d << new FileAttachmentItem(fileName);
            break;
        }

        default:
            qDebug() << "drag-and-drop: invalid kind of attachment";
            return false;
        }
    }

    beginInsertRows(QModelIndex(), m_attachments.size(), m_attachments.size() + items.d.size() - 1);
    Q_FOREACH(AttachmentItem *attachment, items.d) {
        if (m_shouldPreload)
            attachment->preload();
        m_attachments << attachment;
    }
    items.d.clear();
    endInsertRows();

    return true;
}

/** @short Check that the data representing a list of messages is correct */
bool MessageComposer::validateDropImapMessage(QDataStream &stream, QString &mailbox, uint &uidValidity, QList<uint> &uids) const
{
    if (stream.status() != QDataStream::Ok) {
        qDebug() << "drag-and-drop: stream failed:" << stream.status();
        return false;
    }

    TreeItemMailbox *mboxPtr = m_model->findMailboxByName(mailbox);
    if (!mboxPtr) {
        qDebug() << "drag-and-drop: mailbox not found";
        return false;
    }

    if (uids.size() < 1) {
        qDebug() << "drag-and-drop: no UIDs passed";
        return false;
    }
    if (!uidValidity) {
        qDebug() << "drag-and-drop: invalid UIDVALIDITY";
        return false;
    }

    return true;
}

/** @short Handle a drag-and-drop of a list of messages */
bool MessageComposer::dropImapMessage(QDataStream &stream)
{
    stream.setVersion(QDataStream::Qt_4_6);
    if (stream.atEnd()) {
        qDebug() << "drag-and-drop: cannot decode data: end of stream";
        return false;
    }
    QString mailbox;
    uint uidValidity;
    QList<uint> uids;
    stream >> mailbox >> uidValidity >> uids;
    if (!validateDropImapMessage(stream, mailbox, uidValidity, uids))
        return false;
    if (!stream.atEnd()) {
        qDebug() << "drag-and-drop: cannot decode data: too much data";
        return false;
    }

    beginInsertRows(QModelIndex(), m_attachments.size(), m_attachments.size() + uids.size() - 1);
    Q_FOREACH(const uint uid, uids) {
        m_attachments << new ImapMessageAttachmentItem(m_model, mailbox, uidValidity, uid);
        if (m_shouldPreload)
            m_attachments.back()->preload();
    }
    endInsertRows();

    return true;
}

/** @short Check that the data representing a single message part are correct */
bool MessageComposer::validateDropImapPart(QDataStream &stream, QString &mailbox, uint &uidValidity, uint &uid, QString &partId, QString &trojitaPath) const
{
    stream >> mailbox >> uidValidity >> uid >> partId >> trojitaPath;
    if (stream.status() != QDataStream::Ok) {
        qDebug() << "drag-and-drop: stream failed:" << stream.status();
        return false;
    }
    TreeItemMailbox *mboxPtr = m_model->findMailboxByName(mailbox);
    if (!mboxPtr) {
        qDebug() << "drag-and-drop: mailbox not found";
        return false;
    }

    if (!uidValidity || !uid || partId.isEmpty()) {
        qDebug() << "drag-and-drop: invalid data";
        return false;
    }
    return true;
}

/** @short Handle a drag-adn-drop of a list of message parts */
bool MessageComposer::dropImapPart(QDataStream &stream)
{
    stream.setVersion(QDataStream::Qt_4_6);
    if (stream.atEnd()) {
        qDebug() << "drag-and-drop: cannot decode data: end of stream";
        return false;
    }
    QString mailbox;
    uint uidValidity;
    uint uid;
    QString partId;
    QString trojitaPath;
    if (!validateDropImapPart(stream, mailbox, uidValidity, uid, partId, trojitaPath))
        return false;
    if (!stream.atEnd()) {
        qDebug() << "drag-and-drop: cannot decode data: too much data";
        return false;
    }

    beginInsertRows(QModelIndex(), m_attachments.size(), m_attachments.size());
    m_attachments << new ImapPartAttachmentItem(m_model, mailbox, uidValidity, uid, partId, trojitaPath);
    if (m_shouldPreload)
        m_attachments.back()->preload();
    endInsertRows();

    return true;
}

QStringList MessageComposer::mimeTypes() const
{
    return QStringList() << QLatin1String("application/x-trojita-message-list") <<
                            QLatin1String("application/x-trojita-imap-part") <<
                            QLatin1String("application/x-trojita-attachments-list") <<
                            QLatin1String("text/uri-list");
}

void MessageComposer::setFrom(const Message::MailAddress &from)
{
    m_from = from;
}

void MessageComposer::setRecipients(const QList<QPair<RecipientKind, Message::MailAddress> > &recipients)
{
    m_recipients = recipients;
}

void MessageComposer::setInReplyTo(const QByteArray &inReplyTo)
{
    m_inReplyTo = inReplyTo;
}

void MessageComposer::setTimestamp(const QDateTime &timestamp)
{
    m_timestamp = timestamp;
}

void MessageComposer::setSubject(const QString &subject)
{
    m_subject = subject;
}

void MessageComposer::setText(const QString &text)
{
    m_text = text;
}

bool MessageComposer::isReadyForSerialization() const
{
    return true;
}

QByteArray MessageComposer::generateMessageId(const Imap::Message::MailAddress &sender)
{
    if (sender.host.isEmpty()) {
        // There's no usable domain, let's just bail out of here
        return QByteArray();
    }
    return QUuid::createUuid()
#if QT_VERSION >= 0x040800
            .toByteArray()
#else
            .toString().toAscii()
#endif
            .replace("{", "").replace("}", "") + "@" + sender.host.toUtf8();
}

/** @short Generate a random enough MIME boundary */
QByteArray MessageComposer::generateMimeBoundary()
{
    // Usage of "=_" is recommended by RFC2045 as it's guaranteed to never occur in a quoted-printable source
    return QByteArray("trojita=_") + QUuid::createUuid()
#if QT_VERSION >= 0x040800
            .toByteArray()
#else
            .toString().toAscii()
#endif
            .replace("{", "").replace("}", "");
}

QByteArray MessageComposer::encodeHeaderField(const QString &text)
{
    /* This encodes an "unstructured" header field */
    /* FIXME: Don't apply RFC2047 if it isn't needed */
    return Imap::encodeRFC2047String(text);
}

void MessageComposer::writeCommonMessageBeginning(QIODevice *target, const QByteArray boundary) const
{
    // The From header
    target->write(QByteArray("From: ").append(m_from.asMailHeader()).append("\r\n"));

    // All recipients
    QByteArray recipientHeaders;
    for (QList<QPair<RecipientKind,Imap::Message::MailAddress> >::const_iterator it = m_recipients.begin();
         it != m_recipients.end(); ++it) {
        switch(it->first) {
        case Recipient_To:
            recipientHeaders.append("To: ").append(it->second.asMailHeader()).append("\r\n");
            break;
        case Recipient_Cc:
            recipientHeaders.append("Cc: ").append(it->second.asMailHeader()).append("\r\n");
            break;
        case Recipient_Bcc:
            break;
        }
    }
    target->write(recipientHeaders);

    // Other message metadata
    target->write(QByteArray("Subject: ").append(encodeHeaderField(m_subject)).append("\r\n").
            append("Date: ").append(Imap::dateTimeToRfc2822(m_timestamp)).append("\r\n").
            append("User-Agent: ").append(
                QString::fromUtf8("%1/%2; %3")
                .arg(qApp->applicationName(), qApp->applicationVersion(), Imap::Mailbox::systemPlatformVersion()).toUtf8()
                ).append("\r\n").
            append("MIME-Version: 1.0\r\n"));
    QByteArray messageId = generateMessageId(m_from);
    if (!messageId.isEmpty()) {
        target->write(QByteArray("Message-ID: <").append(messageId).append(">\r\n"));
    }
    if (!m_inReplyTo.isEmpty()) {
        target->write(QByteArray("In-Reply-To: ").append(m_inReplyTo).append("\r\n"));
    }


    // Headers depending on actual message body data
    if (!m_attachments.isEmpty()) {
        target->write("Content-Type: multipart/mixed;\r\n\tboundary=\"" + boundary + "\"\r\n"
                      "\r\nThis is a multipart/mixed message in MIME format.\r\n\r\n"
                      "--" + boundary + "\r\n");
    }

    target->write("Content-Type: text/plain; charset=utf-8\r\n"
                  "Content-Transfer-Encoding: quoted-printable\r\n"
                  "\r\n");
    target->write(Imap::quotedPrintableEncode(m_text.toUtf8()));
}

bool MessageComposer::writeAttachmentHeader(QIODevice *target, QString *errorMessage, const AttachmentItem *attachment, const QByteArray &boundary) const
{
    if (!attachment->isAvailableLocally() && attachment->imapUrl().isEmpty()) {
        *errorMessage = tr("Attachment %1 is not available").arg(attachment->caption());
        return false;
    }
    target->write("\r\n--" + boundary + "\r\n"
                  "Content-Type: " + attachment->mimeType() + "\r\n");
    target->write(attachment->contentDispositionHeader());

    switch (attachment->suggestedCTE()) {
    case AttachmentItem::CTE_BASE64:
        target->write("Content-Transfer-Encoding: base64\r\n");
        break;
    case AttachmentItem::CTE_7BIT:
        target->write("Content-Transfer-Encoding: 7bit\r\n");
        break;
    case AttachmentItem::CTE_8BIT:
        target->write("Content-Transfer-Encoding: 8bit\r\n");
        break;
    case AttachmentItem::CTE_BINARY:
        target->write("Content-Transfer-Encoding: binary\r\n");
        break;
    }

    target->write("\r\n");
    return true;
}

bool MessageComposer::writeAttachmentBody(QIODevice *target, QString *errorMessage, const AttachmentItem *attachment) const
{
    if (!attachment->isAvailableLocally()) {
        *errorMessage = tr("Attachment %1 is not available").arg(attachment->caption());
        return false;
    }
    QSharedPointer<QIODevice> io = attachment->rawData();
    if (!io) {
        *errorMessage = tr("Attachment %1 disappeared").arg(attachment->caption());
        return false;
    }
    while (!io->atEnd()) {
        switch (attachment->suggestedCTE()) {
        case AttachmentItem::CTE_BASE64:
            // Base64 maps 6bit chunks into a single byte. Output shall have no more than 76 characters per line
            // (not counting the CRLF pair).
            target->write(io->read(76*6/8).toBase64() + "\r\n");
            break;
        default:
            target->write(io->readAll());
        }
    }
    return true;
}

bool MessageComposer::asRawMessage(QIODevice *target, QString *errorMessage) const
{
    // We don't bother with checking that our boundary is not present in the individual parts. That's arguably wrong,
    // but we don't have much choice if we ever plan to use CATENATE.  It also looks like this is exactly how other MUAs
    // oeprate as well, so let's just join the universal dontcareism here.
    QByteArray boundary(generateMimeBoundary());

    writeCommonMessageBeginning(target, boundary);

    if (!m_attachments.isEmpty()) {
        Q_FOREACH(const AttachmentItem *attachment, m_attachments) {
            if (!writeAttachmentHeader(target, errorMessage, attachment, boundary))
                return false;
            if (!writeAttachmentBody(target, errorMessage, attachment))
                return false;
        }
        target->write("\r\n--" + boundary + "--\r\n");
    }
    return true;
}

bool MessageComposer::asCatenateData(QList<CatenatePair> &target, QString *errorMessage) const
{
    target.clear();
    QByteArray boundary(generateMimeBoundary());
    target.append(qMakePair(CATENATE_TEXT, QByteArray()));

    // write the initial data
    {
        QBuffer io(&target.back().second);
        io.open(QIODevice::ReadWrite);
        writeCommonMessageBeginning(&io, boundary);
    }

    if (!m_attachments.isEmpty()) {
        Q_FOREACH(const AttachmentItem *attachment, m_attachments) {
            if (target.back().first != CATENATE_TEXT) {
                target.append(qMakePair(CATENATE_TEXT, QByteArray()));
            }
            QBuffer io(&target.back().second);
            io.open(QIODevice::Append);

            if (!writeAttachmentHeader(&io, errorMessage, attachment, boundary))
                return false;

            QByteArray url = attachment->imapUrl();
            if (url.isEmpty()) {
                // Cannot use CATENATE here
                if (!writeAttachmentBody(&io, errorMessage, attachment))
                    return false;
            } else {
                target.append(qMakePair(CATENATE_URL, url));
            }
        }
        if (target.back().first != CATENATE_TEXT) {
            target.append(qMakePair(CATENATE_TEXT, QByteArray()));
        }
        QBuffer io(&target.back().second);
        io.open(QIODevice::Append);
        io.write("\r\n--" + boundary + "--\r\n");
    }
    return true;
}

QDateTime MessageComposer::timestamp() const
{
    return m_timestamp;
}

QByteArray MessageComposer::rawFromAddress() const
{
    return m_from.asSMTPMailbox();
}

QList<QByteArray> MessageComposer::rawRecipientAddresses() const
{
    QList<QByteArray> res;

    for (QList<QPair<RecipientKind,Imap::Message::MailAddress> >::const_iterator it = m_recipients.begin();
         it != m_recipients.end(); ++it) {
        res << it->second.asSMTPMailbox();
    }

    return res;
}

void MessageComposer::addFileAttachment(const QString &path)
{
    beginInsertRows(QModelIndex(), m_attachments.size(), m_attachments.size());
    m_attachments << new FileAttachmentItem(path);
    endInsertRows();
}

void MessageComposer::removeAttachment(const QModelIndex &index)
{
    if (!index.isValid() || index.column() != 0 || index.row() < 0 || index.row() >= m_attachments.size())
        return;

    beginRemoveRows(QModelIndex(), index.row(), index.row());
    delete m_attachments.takeAt(index.row());
    endRemoveRows();
}

void MessageComposer::setPreloadEnabled(const bool preload)
{
    m_shouldPreload = preload;
}

}
}
