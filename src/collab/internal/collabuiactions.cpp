/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Ploq — the app's own actions (see collabuiactions.h).
 */
#include "collabuiactions.h"

#include "context/uicontext.h"
#include "context/shortcutcontext.h"
#include "types/translatablestring.h"
#include "ui/view/iconcodes.h"

#include "sharecontroller.h"

using namespace mu::collab;
using namespace muse;
using namespace muse::ui;
using namespace muse::actions;

const UiActionList CollabUiActions::m_actions = {
    UiAction("ploq-share",
             mu::context::UiCtxProjectOpened,
             mu::context::CTX_ANY,
             TranslatableString::untranslatable("&Share"),
             TranslatableString::untranslatable("Share this score: copy an invite link and see who's in it"),
             IconCode::Code::SHARE_FILE
             ),
    UiAction("ploq-manage-on-web",
             mu::context::UiCtxAny,
             mu::context::CTX_ANY,
             TranslatableString::untranslatable("&Manage on web"),
             TranslatableString::untranslatable("Open this score's page on the Ploq website"),
             IconCode::Code::OPEN_LINK
             ),
    UiAction("ploq-sign-out",
             mu::context::UiCtxAny,
             mu::context::CTX_ANY,
             TranslatableString::untranslatable("Sign out of Ploq"),
             TranslatableString::untranslatable("Sign out of Ploq on this computer")
             ),
};

static async::Channel<ActionCodeList>& enabledChannel()
{
    static async::Channel<ActionCodeList> ch;
    return ch;
}

const UiActionList& CollabUiActions::actionsList() const
{
    return m_actions;
}

bool CollabUiActions::actionEnabled(const UiAction& act) const
{
    const ShareController* share = ShareController::instance();
    if (!share) {
        return false; // collaboration is off (or a local test relay without accounts)
    }
    if (act.code == "ploq-sign-out") {
        return share->isSignedIn();
    }
    return true;
}

async::Channel<ActionCodeList> CollabUiActions::actionEnabledChanged() const
{
    return enabledChannel();
}

void CollabUiActions::notifyEnabledChanged()
{
    enabledChannel().send({ "ploq-share", "ploq-manage-on-web", "ploq-sign-out" });
}

bool CollabUiActions::actionChecked(const UiAction&) const
{
    return false;
}

async::Channel<ActionCodeList> CollabUiActions::actionCheckedChanged() const
{
    static async::Channel<ActionCodeList> ch;
    return ch;
}
