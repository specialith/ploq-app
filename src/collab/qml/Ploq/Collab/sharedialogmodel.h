/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Ploq — the Share dialog's view of ShareController.
 */
#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <qqmlintegration.h>

namespace mu::collab {
class ShareDialogModel : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString scoreTitle READ scoreTitle NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
    Q_PROPERTY(QString inviteLink READ inviteLink NOTIFY changed)
    Q_PROPERTY(bool linkCopied READ linkCopied NOTIFY changed)
    Q_PROPERTY(bool canInvite READ canInvite NOTIFY changed)
    Q_PROPERTY(bool hasUpgrade READ hasUpgrade NOTIFY changed)
    Q_PROPERTY(QString accountName READ accountName NOTIFY changed)
    Q_PROPERTY(bool signedIn READ signedIn NOTIFY changed)
    Q_PROPERTY(bool shared READ shared NOTIFY changed)
    Q_PROPERTY(QVariantList members READ members NOTIFY changed)

public:
    explicit ShareDialogModel(QObject* parent = nullptr);
    ~ShareDialogModel() override;

    QString status() const;
    QString scoreTitle() const;
    QString message() const;
    QString inviteLink() const;
    bool linkCopied() const;
    bool canInvite() const;
    bool hasUpgrade() const;
    QString accountName() const;
    bool signedIn() const;
    bool shared() const;
    QVariantList members() const;

    Q_INVOKABLE void copyLink();
    Q_INVOKABLE void openUpgrade();
    Q_INVOKABLE void manageOnWeb();
    Q_INVOKABLE void signOut();
    Q_INVOKABLE void retry();
    Q_INVOKABLE void reopenBrowser();
    Q_INVOKABLE void cancelSignIn();
    Q_INVOKABLE void closed();

signals:
    void changed();
};
}
