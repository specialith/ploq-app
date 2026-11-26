/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2021 MuseScore Limited and others
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

import Muse.Ui
import Muse.UiComponents

BaseSection {
    id: root

    title: qsTrc("preferences", "Automatic update check")

    property bool isAppUpdatable: true
    property alias needCheckForNewAppVersion: needCheckBox.checked
    property string museScorePrivacyPolicyUrl

    signal needCheckForNewAppVersionChangeRequested(bool check)

    CheckBox {
        id: needCheckBox
        width: parent.width

        text: "Check to see if a new version of Ploq is available" // COLLAB: was "...MuseScore Studio..."

        visible: root.isAppUpdatable

        navigation.name: "NeedCheckBox"
        navigation.panel: root.navigation
        navigation.row: 0

        onClicked: {
            root.needCheckForNewAppVersionChangeRequested(!checked)
        }
    }

    StyledTextLabel {
        width: parent.width

        // COLLAB: Ploq's own text (was MuseScore's, with MuseScore's privacy policy link)
        text: "Update checking asks the Ploq server (collab.ploq.app) whether there is a newer version. Ploq only installs updates signed by the Ploq team."

        horizontalAlignment: Qt.AlignLeft
        wrapMode: Text.WordWrap
        maximumLineCount: 3
    }
}
