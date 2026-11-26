/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Ploq — the Share dialog. Built from MuseScore's own components and theme
 * (StyledDialogView, FlatButton, TextInputField...) so it looks like the rest
 * of the app. All names and titles come from other people: shown as plain text.
 */
import QtQuick
import QtQuick.Layouts

import Muse.Ui
import Muse.UiComponents

StyledDialogView {
    id: root

    objectName: "PloqShareDialog"
    title: "Share"

    contentWidth: 460
    contentHeight: content.implicitHeight
    margins: 20

    modal: true

    ShareDialogModel {
        id: shareModel
    }

    onClosed: shareModel.closed()

    onNavigationActivateRequested: {
        if (copyButton.visible) {
            copyButton.navigation.requestActive()
        } else {
            doneButton.navigation.requestActive()
        }
    }

    readonly property string status: shareModel.status
    // Not "idle": that's where a declined sign-in ends up, and nothing is under way then.
    readonly property bool busy: status === "working" || status === "signin"

    NavigationPanel {
        id: navPanel
        name: "PloqShareDialog"
        section: root.navigationSection
        direction: NavigationPanel.Horizontal
        order: 1
    }

    ColumnLayout {
        id: content

        width: parent.width
        spacing: 16

        // --- heading ---------------------------------------------------------
        StyledTextLabel {
            Layout.fillWidth: true

            text: root.status === "limit" ? "You've used all your hosted scores"
                  : root.status === "frozen" ? "Download-only"
                  : "Share “%1”".arg(shareModel.scoreTitle)
            textFormat: Text.PlainText
            font: ui.theme.largeBodyBoldFont
            horizontalAlignment: Text.AlignLeft
            elide: Text.ElideRight
        }

        // --- waiting (signing in in the browser / uploading) -----------------
        RowLayout {
            Layout.fillWidth: true
            visible: root.busy
            spacing: 12

            StyledBusyIndicator {
                Layout.preferredWidth: 24
                Layout.preferredHeight: 24
                running: root.busy
            }

            StyledTextLabel {
                Layout.fillWidth: true
                text: shareModel.message !== "" ? shareModel.message : "Working…"
                textFormat: Text.PlainText
                horizontalAlignment: Text.AlignLeft
                wrapMode: Text.Wrap
            }
        }

        // --- the invite link -------------------------------------------------
        ColumnLayout {
            Layout.fillWidth: true
            visible: root.status === "ready" && shareModel.canInvite
            spacing: 8

            StyledTextLabel {
                Layout.fillWidth: true
                text: "Anyone with this link can join and edit with you."
                horizontalAlignment: Text.AlignLeft
                wrapMode: Text.Wrap
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                TextInputField {
                    id: linkField
                    Layout.fillWidth: true
                    readOnly: true
                    currentText: shareModel.inviteLink
                    navigation.panel: navPanel
                    navigation.column: 0
                    navigation.accessible.name: "Invite link"
                }

                FlatButton {
                    id: copyButton
                    text: "Copy link"
                    accentButton: !shareModel.linkCopied
                    navigation.panel: navPanel
                    navigation.column: 1
                    onClicked: shareModel.copyLink()
                }
            }

            Row {
                spacing: 6
                visible: shareModel.linkCopied

                StyledIconLabel {
                    anchors.verticalCenter: parent.verticalCenter
                    iconCode: IconCode.TICK_RIGHT_ANGLE
                    color: ui.theme.accentColor
                }

                StyledTextLabel {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Link copied"
                }
            }
        }

        // --- a member who can't invite, limit, frozen, errors: a message -----
        StyledTextLabel {
            Layout.fillWidth: true
            visible: (root.status === "ready" && !shareModel.canInvite) || root.status === "limit"
                     || root.status === "frozen" || root.status === "error"
                     || (root.status === "idle" && shareModel.message !== "")
            text: shareModel.message
            textFormat: Text.PlainText
            horizontalAlignment: Text.AlignLeft
            wrapMode: Text.Wrap
        }

        StyledTextLabel {
            Layout.fillWidth: true
            visible: root.status === "limit"
            text: "Upgrade on the website to share more scores, or delete one you no longer need there."
            horizontalAlignment: Text.AlignLeft
            wrapMode: Text.Wrap
        }

        StyledTextLabel {
            Layout.fillWidth: true
            visible: root.status === "frozen"
            text: "This score opens without live editing. Changes you make stay on this computer."
            horizontalAlignment: Text.AlignLeft
            wrapMode: Text.Wrap
        }

        // --- who's in the score ----------------------------------------------
        ColumnLayout {
            Layout.fillWidth: true
            visible: root.status === "ready" && shareModel.members.length > 0
            spacing: 8

            SeparatorLine {
                Layout.fillWidth: true
            }

            StyledTextLabel {
                Layout.fillWidth: true
                text: "People in this score"
                font: ui.theme.bodyBoldFont
                horizontalAlignment: Text.AlignLeft
            }

            Repeater {
                model: root.status === "ready" ? shareModel.members : []

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10

                    // Filled: in the score right now. Ring: not right now.
                    Rectangle {
                        Layout.preferredWidth: 8
                        Layout.preferredHeight: 8
                        radius: 4
                        color: modelData.online ? ui.theme.accentColor : "transparent"
                        border.width: modelData.online ? 0 : 1
                        border.color: ui.theme.fontSecondaryColor
                    }

                    StyledTextLabel {
                        Layout.fillWidth: true
                        text: modelData.name + (modelData.you ? " (you)" : "")
                        textFormat: Text.PlainText
                        horizontalAlignment: Text.AlignLeft
                        elide: Text.ElideRight
                    }

                    StyledTextLabel {
                        text: modelData.online ? "In the score now" : ""
                        visible: modelData.online
                        color: ui.theme.fontSecondaryColor
                    }

                    StyledTextLabel {
                        text: "Owner"
                        visible: modelData.owner
                        color: ui.theme.fontSecondaryColor
                    }
                }
            }
        }

        // --- footer ----------------------------------------------------------
        // Two rows: who's signed in, then the buttons. On one row they need ~580 px
        // (FlatButtons are at least 132 wide) and a layout wider than the dialog
        // pushes every row past its right edge.
        SeparatorLine {
            Layout.fillWidth: true
            Layout.topMargin: 4
        }

        RowLayout {
            Layout.fillWidth: true
            visible: shareModel.signedIn
            spacing: 8

            StyledTextLabel {
                Layout.fillWidth: true
                text: "Signed in as %1".arg(shareModel.accountName)
                textFormat: Text.PlainText
                color: ui.theme.fontSecondaryColor
                horizontalAlignment: Text.AlignLeft
                elide: Text.ElideRight
            }

            FlatButton {
                text: "Sign out"
                transparent: true
                isNarrow: true
                navigation.panel: navPanel
                navigation.column: 2
                onClicked: {
                    shareModel.signOut()
                    root.hide()
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Item {
                Layout.fillWidth: true
            }

            FlatButton {
                visible: root.status === "signin"
                text: "Open browser again"
                navigation.panel: navPanel
                navigation.column: 3
                onClicked: shareModel.reopenBrowser()
            }

            FlatButton {
                visible: root.status === "error"
                text: "Try again"
                navigation.panel: navPanel
                navigation.column: 3
                onClicked: shareModel.retry()
            }

            FlatButton {
                visible: shareModel.shared && root.status !== "signin" && root.status !== "limit"
                text: "Manage on web"
                navigation.panel: navPanel
                navigation.column: 4
                onClicked: {
                    Qt.callLater(shareModel.manageOnWeb)
                    root.hide()
                }
            }

            FlatButton {
                visible: root.status === "limit" && shareModel.hasUpgrade
                text: "See plans"
                accentButton: true
                navigation.panel: navPanel
                navigation.column: 5
                onClicked: {
                    Qt.callLater(shareModel.openUpgrade)
                    root.hide()
                }
            }

            FlatButton {
                id: doneButton
                text: root.status === "signin" ? "Cancel"
                      : root.status === "limit" ? "Not now"
                      : root.status === "ready" ? "Done" : "Close"
                accentButton: root.status === "ready" && shareModel.linkCopied
                navigation.panel: navPanel
                navigation.column: 6
                onClicked: {
                    if (root.status === "signin") {
                        shareModel.cancelSignIn()
                    }
                    root.hide()
                }
            }
        }
    }
}
