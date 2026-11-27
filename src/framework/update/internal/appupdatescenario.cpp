/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-CLA-applies
 *
 * MuseScore
 * Music Composition & Notation
 *
 * Copyright (C) 2025 MuseScore Limited and others
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

#include "appupdatescenario.h"

#include "updateerrors.h"

#include "types/val.h"
#include "translation.h"
#include "defer.h"
#include "log.h"

using namespace muse;
using namespace muse::update;
using namespace muse::actions;
using namespace muse::async;

bool AppUpdateScenario::needCheckForUpdate() const
{
    return configuration()->needCheckForUpdate();
}

void AppUpdateScenario::checkForUpdate(bool manual)
{
    if (m_checkInProgress) {
        return;
    }

    m_checkInProgress = true;
    m_checkInProgressChanged.notify();

    service()->checkForUpdate().onResolve(this, [this, manual](const RetVal<ReleaseInfo>& res) {
        const bool noUpdate = res.ret.code() == static_cast<int>(Err::NoUpdate);

        if (manual) {
            if (noUpdate) {
                showNoUpdateMsg();
            } else if (!res.ret) {
                showServerErrorMsg();
            } else {
                showReleaseInfo(res.val);
            }
        } else if (!noUpdate) {
            LOGE() << res.ret.toString();
        }

        m_checkInProgress = false;
        m_checkInProgressChanged.notify();
    });
}

bool AppUpdateScenario::checkInProgress() const
{
    return m_checkInProgress;
}

async::Notification AppUpdateScenario::checkInProgressChanged() const
{
    return m_checkInProgressChanged;
}

bool AppUpdateScenario::hasUpdate() const
{
    if (m_checkInProgress) {
        return false;
    }

    const RetVal<ReleaseInfo>& lastCheckResult = service()->lastCheckResult();
    if (!lastCheckResult.ret) {
        return false;
    }

    if (lastCheckResult.ret.code() == static_cast<int>(Err::NoUpdate)) {
        return false;
    }

    return !shouldIgnoreUpdate(lastCheckResult.val);
}

Promise<Ret> AppUpdateScenario::showUpdate()
{
    const RetVal<ReleaseInfo>& lastCheckResult = service()->lastCheckResult();
    if (lastCheckResult.ret) {
        return showReleaseInfo(lastCheckResult.val);
    }
    return async::make_promise<Ret>([lastCheckResult](auto resolve, auto) {
        return resolve(lastCheckResult.ret);
    });
}

Promise<Ret> AppUpdateScenario::processUpdateError(int errorCode)
{
    const auto unknownError = async::make_promise<Ret>([](auto resolve, auto) {
        return resolve(muse::make_ret(Ret::Code::UnknownError));
    });

    IF_ASSERT_FAILED(errorCode >= static_cast<int>(Ret::Code::UpdateFirst)
                     && errorCode <= static_cast<int>(Ret::Code::UpdateLast)) {
        return unknownError;
    }

    const Err error = static_cast<Err>(errorCode);
    IF_ASSERT_FAILED(error != Err::NoError) {
        return unknownError;
    }

    auto message = error == Err::NoUpdate ? showNoUpdateMsg() : showServerErrorMsg();
    return message.then<Ret>(this, [errorCode](const IInteractive::Result&, auto resolve) {
        const Ret::Code code = static_cast<Ret::Code>(errorCode);
        return resolve(muse::make_ret(code));
    });
}

Promise<IInteractive::Result> AppUpdateScenario::showNoUpdateMsg()
{
    // COLLAB: Ploq's text (was "...latest version of MuseScore Studio. Please visit MuseScore.org...")
    const QString str = QString("You already have the latest version of Ploq. "
                                "Visit <a href=\"https://ploq.app\">ploq.app</a> for news on what’s coming next.");

    const IInteractive::Text text(str.toStdString(), IInteractive::TextFormat::RichText);
    const IInteractive::ButtonData okBtn = interactive()->buttonData(IInteractive::Button::Ok);

    return interactive()->info(muse::trc("update", "You’re up to date!"), text, { okBtn }, okBtn.btn,
                               IInteractive::Option::WithIcon);
}

Promise<Ret> AppUpdateScenario::showReleaseInfo(const ReleaseInfo& info)
{
    UriQuery query("muse://update/appreleaseinfo");
    query.addParam("notes", Val(info.notes));
    query.addParam("previousReleasesNotes", Val(releasesNotesToValList(info.previousReleasesNotes)));

    return interactive()->open(query).then<Ret>(this, [this, info](const Val& val, auto resolve) {
        const QString actionCode = val.toQString();
        if (actionCode == "remindLater") {
            return resolve(muse::make_ret(Ret::Code::Cancel));
        }

        if (actionCode == "skip") {
            configuration()->setSkippedReleaseVersion(info.version);
            return resolve(muse::make_ret(Ret::Code::Cancel));
        }

#ifndef Q_OS_WIN
        // COLLAB: Ploq on macOS/Linux installs nothing itself: the download page has the new .dmg / AppImage
        interactive()->openUrl(std::string("https://ploq.app/download"));
        return resolve(muse::make_ret(Ret::Code::Cancel)); // not Ok: Ok would mean "closing to install"
#endif

        //! NOTE: In test mode we skip the progress dialog and jump straight to the "needs to close" dialog...
        const bool testMode = configuration()->checkForUpdateTestMode();
        auto promise = testMode ? askToCloseAppAndCompleteInstall(/*installerPath*/ String()) : downloadRelease();
        promise.onResolve(this, [resolve](const Ret& ret) {
            (void)resolve(ret);
        });

        return Promise<Ret>::dummy_result();
    });
}

Promise<IInteractive::Result> AppUpdateScenario::showServerErrorMsg()
{
    // COLLAB: Ploq: also shown when a download fails its signature/SHA-256 check (was "Cannot connect to server")
    return interactive()->error("Couldn’t update Ploq",
                                "The update couldn’t be downloaded, or it didn’t pass Ploq’s safety checks, so nothing was installed. Please try again later.");
}

Promise<Ret> AppUpdateScenario::downloadRelease()
{
    RetVal<Val> rv = interactive()->openSync("muse://update/app?mode=download");
    if (!rv.ret) {
        return processUpdateError(rv.ret.code());
    }
    return askToCloseAppAndCompleteInstall(rv.val.toString());
}

Promise<Ret> AppUpdateScenario::askToCloseAppAndCompleteInstall(const io::path_t& installerPath)
{
    // COLLAB: Ploq wording (was "MuseScore Studio needs to close...")
    const std::string info = "Ploq needs to close to install the update. "
                             "If you have any unsaved changes, you will be asked to save them first. Ploq reopens when the update is done.";
    const int closeBtn = int(IInteractive::Button::CustomButton) + 1;
    const IInteractive::ButtonDatas buttons = {
        interactive()->buttonData(IInteractive::Button::Cancel),
        IInteractive::ButtonData(closeBtn, muse::trc("update", "Close"), true)
    };

    return interactive()->info("", info, buttons, closeBtn)
           .then<Ret>(this, [this, installerPath](const IInteractive::Result& res, auto resolve) {
        if (res.isButton(IInteractive::Button::Cancel)) {
            return resolve(muse::make_ret(Ret::Code::Cancel));
        }

        if (multiwindowsProvider()->windowCount() != 1) {
            multiwindowsProvider()->quitAllAndRunInstallation(installerPath);
        }

        dispatcher()->dispatch("quit", ActionData::make_arg2<bool, std::string>(false, installerPath.toStdString()));
        return resolve(muse::make_ok());
    });
}

bool AppUpdateScenario::shouldIgnoreUpdate(const ReleaseInfo& info) const
{
    return info.version == configuration()->skippedReleaseVersion() && !configuration()->checkForUpdateTestMode();
}
