/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2021-2024 MuseScore Limited and others
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

import QtQuick
import QtQuick.Layouts

import Muse.Ui
import Muse.UiComponents

import MuseScore.AppShell

StyledDialogView {
    id: root

    title: "About Ploq" // COLLAB: was qsTrc("appshell/about", "About MuseScore Studio")

    contentHeight: 424
    contentWidth: 480

    AboutModel {
        id: aboutModel
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 30

        ColumnLayout {
            id: content

            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.topMargin: 36
            Layout.leftMargin: 40
            Layout.rightMargin: 40

            spacing: 32

            Image {
                id: logo
                Layout.alignment: Qt.AlignHCenter

                source: "qrc:/ploq/ploq_tile.svg" // COLLAB: Ploq logo (was resources/mu_logo.svg)
                sourceSize: Qt.size(100, 100)

                MouseArea {
                    anchors.fill: parent

                    property int clickCount: 0

                    onClicked: {
                        clickCount++

                        if (clickCount % 3 == 0) {
                            aboutModel.toggleDevMode()
                        }
                    }
                }
            }

            Column {
                spacing: 8
                Layout.fillWidth: true

                StyledTextLabel { // COLLAB: Ploq name and version, then the MuseScore credit
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "Ploq " + aboutModel.ploqVersion()
                    font: ui.theme.bodyBoldFont
                }

                StyledTextLabel { // COLLAB: credit (required): Ploq is based on MuseScore Studio
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "Based on MuseScore Studio " + aboutModel.museScoreVersion()
                }

                Row {
                    spacing: 4
                    anchors.horizontalCenter: parent.horizontalCenter

                    StyledTextLabel {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTrc("appshell/about", "Revision:") + " " + aboutModel.museScoreRevision()
                    }

                    FlatButton {
                        anchors.verticalCenter: parent.verticalCenter
                        icon: IconCode.COPY

                        onClicked: {
                            aboutModel.copyRevisionToClipboard()
                        }
                    }
                }
            }

            StyledTextLabel {
                Layout.fillWidth: true
                // COLLAB: Ploq's site, the MuseScore credit and a no-affiliation note (was MuseScore's links)
                text: "Visit <a href='https://ploq.app'>ploq.app</a> for new versions and more information.<br>"
                      + "Ploq is based on <a href='https://musescore.org'>MuseScore Studio</a>, free and open-source music notation software. "
                      + "Ploq is not made or endorsed by MuseScore Limited or Muse Group."
                property var upstreamText: {
                    let museScoreUrl = aboutModel.museScoreUrl()

                    //: %1 will be a link to the MuseScore website
                    let line1 = qsTrc("appshell/about", "Visit %1 for new versions and more information.")
                                .arg(`<a href="${museScoreUrl.url}">${museScoreUrl.displayName}</a>`)

                    let line2 = qsTrc("appshell/about", "Get <a href=\"%1\">help</a> with the program or <a href=\"%2\">contribute</a> to its development.")
                                .arg(aboutModel.museScoreForumUrl().url)
                                .arg(aboutModel.museScoreContributionUrl().url)

                    return line1 + "<br>" + line2
                }
                wrapMode: Text.WordWrap
                maximumLineCount: 4 // COLLAB: was 3
            }

            StyledTextLabel {
                Layout.fillWidth: true

                // COLLAB: Ploq's own privacy note (was MuseScore's privacy policy link)
                text: "Ploq sends nothing to Muse Group. Shared scores and your display name are stored on the Ploq server."

                wrapMode: Text.WordWrap
                maximumLineCount: 3
            }

            StyledTextLabel {
                Layout.fillWidth: true
                // COLLAB: MuseScore's copyright kept (GPL); Ploq's changes credited first
                text: "Ploq changes " + String.fromCharCode(169) + " 2026 the Ploq authors.<br>" + qsTrc("appshell/about", "Copyright © 1999-2026 MuseScore Limited and others.\nPublished under the <a href=\"%1\">GNU General Public License version 3</a>.")
                      .arg("https://www.gnu.org/licenses/gpl-3.0.html")
                      .replace("\n", "<br>")

                wrapMode: Text.WordWrap
                maximumLineCount: 3
            }
        }

        RowLayout {
            Layout.alignment: Qt.AlignRight
            Layout.rightMargin: 16
            Layout.bottomMargin: 16

            spacing: 12

            FlatButton {
                text: qsTrc("global", "OK")

                onClicked: {
                    root.hide()
                }
            }
        }
    }
}
