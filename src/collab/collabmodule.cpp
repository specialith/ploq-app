/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Collab fork — real-time collaboration module.
 */
#include "collabmodule.h"

#include <QCoreApplication>
#include <QDir>
#include <QFontDatabase>
#include <QFileInfo>
#include <QSettings>

#include "ui/iinteractiveuriregister.h"
#include "ui/iuiactionsregister.h"

#include "internal/collabsyncservice.h"
#include "internal/collabuiactions.h"
#include "ploqapp.h"
#ifdef Q_OS_LINUX
#include "ploqlinux.h"
#endif

#include "log.h"

using namespace mu::collab;

//! Make ploq:// links (website "Open in Ploq", invite links)
//! open this copy of the app. Windows hands the link to
//! <package>\Open-Link.ps1, which treats it strictly as text and accepts only
//! exact link shapes. Never route links through cmd.exe / a .bat file: "&" or
//! quotes in a link would run commands. Only for the portable package (that
//! script exists next to bin\) and only for the current Windows user (HKCU).
//! Re-done every start, so moving the folder works - and an older,
//! vulnerable Join.bat registration gets replaced.
static void registerLinkHandler()
{
#ifdef Q_OS_WIN
    const QString script = QDir(QCoreApplication::applicationDirPath() + "/..").absoluteFilePath("Open-Link.ps1");
    if (!QFileInfo::exists(script)) {
        return; // a dev build: no portable launchers
    }
    // Ploq.exe opens the links itself (main.cpp, ploqHandleLink): no PowerShell console flashing up.
    // The Open-Link.ps1 check above only tells an installed/portable Ploq from a dev build.
    const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    // Remove the pre-rename "collabscore://" handler: older packages pointed it
    // at a Join.bat that was vulnerable to command injection.
    QSettings classes("HKEY_CURRENT_USER\\Software\\Classes", QSettings::NativeFormat);
    classes.remove("collabscore");

    QSettings reg("HKEY_CURRENT_USER\\Software\\Classes\\ploq", QSettings::NativeFormat);
    reg.setValue("Default", "URL:Ploq link");
    reg.setValue("URL Protocol", "");
    reg.setValue("shell/open/command/Default", "\"" + exe + "\" --ploq-link \"%1\"");
    LOGI() << "collab: ploq:// links open " << exe;
#endif
#ifdef Q_OS_LINUX
    ploq::linuxRegisterLinkHandler(); // the AppImage only: a hidden desktop entry for x-scheme-handler/ploq (ploqlinux.cpp)
#endif
}

std::string CollabModule::moduleName() const
{
    return "collab";
}

void CollabModule::registerExports()
{
    m_syncService = std::make_shared<CollabSyncService>(iocContext());

    // Inter for collaborators' name tags (cursorlayer.cpp).
    for (const char* f : { ":/ploq/fonts/Inter-Light.ttf", ":/ploq/fonts/Inter-Regular.ttf" }) {
        if (QFontDatabase::addApplicationFont(f) < 0) {
            LOGW() << "ploq: couldn't load font " << f;
        }
    }
}

void CollabModule::resolveImports()
{
    // Share / Manage on web / Sign out (handled by ShareController).
    if (auto ar = ioc()->resolve<muse::ui::IUiActionsRegister>(moduleName())) {
        ar->reg(std::make_shared<CollabUiActions>());
    }
    if (auto ir = ioc()->resolve<muse::ui::IInteractiveUriRegister>(moduleName())) {
        ir->registerQmlUri(muse::Uri("musescore://ploq/share"), "Ploq.Collab", "ShareDialog");
    }
}

void CollabModule::onAllInited(const muse::IApplication::RunMode& mode)
{
    if (mode != muse::IApplication::RunMode::GuiApp) {
        return;
    }
    LOGI() << "ploq: version " << ploq::version();
    for (const QString& note : ploq::firstLaunchNotes()) {
        LOGI() << "ploq: " << note;
    }
    registerLinkHandler();
    m_syncService->init();
}

void CollabModule::onDeinit()
{
    if (m_syncService) {
        m_syncService->deinit();
    }
}
