/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Ploq — the app's own actions: Share, Manage on web, Sign out. They appear
 * in the notation toolbar, the File menu and the Publish toolbar (one
 * "// COLLAB:" line each) and are handled by ShareController.
 */
#pragma once

#include "ui/iuiactionsmodule.h"

namespace mu::collab {
class CollabUiActions : public muse::ui::IUiActionsModule
{
public:
    const muse::ui::UiActionList& actionsList() const override;
    bool actionEnabled(const muse::ui::UiAction& act) const override;
    muse::async::Channel<muse::actions::ActionCodeList> actionEnabledChanged() const override;
    bool actionChecked(const muse::ui::UiAction& act) const override;
    muse::async::Channel<muse::actions::ActionCodeList> actionCheckedChanged() const override;

    //! Sign-in state changed: menus re-check which actions are enabled.
    static void notifyEnabledChanged();

private:
    static const muse::ui::UiActionList m_actions;
};
}
