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

#include <csignal>

#include <QApplication>
#include <QStyleHints>
#include <QQuickWindow>
#include <QSslSocket>
#include <QDir> // COLLAB: Ploq: QA window grabs
#include <QFile>
#include <QImage>
#include <QRegularExpression>
#include <QTimer>

#include "appfactory.h"
#include "collab/ploqapp.h" // COLLAB: Ploq identity + first launch
#include "internal/commandlineparser.h"
#include "global/iapplication.h"

#include "muse_framework_config.h"
#include "app_config.h"

#include "log.h"

#ifndef MUSE_MODULE_DIAGNOSTICS_CRASHPAD_CLIENT
static void crashCallback(int signum)
{
    const char* signame = "UNKNOWN SIGNAME";
    const char* sigdescript = "";
    switch (signum) {
    case SIGILL:
        signame = "SIGILL";
        sigdescript = "Illegal Instruction";
        break;
    case SIGSEGV:
        signame = "SIGSEGV";
        sigdescript =  "Invalid memory reference";
        break;
    }
    LOGE() << "Oops! Application crashed with signal: [" << signum << "] " << signame << "-" << sigdescript;
    exit(EXIT_FAILURE);
}

#endif

// COLLAB: Ploq dev/QA only: COLLAB_CRASH_DUMP=<folder> writes a minidump there on a
// crash instead of the one-line "Oops" log, so a crash in a QA run has a stack.
// COLLAB: a dump holds process memory, which can include the Ploq sign-in token: don't share dumps.
#if defined(Q_OS_WIN) && !defined(MUSE_MODULE_DIAGNOSTICS_CRASHPAD_CLIENT)
#include <windows.h>
#include <dbghelp.h>
static wchar_t s_crashDumpDir[MAX_PATH] = { 0 };
static LONG WINAPI ploqWriteCrashDump(EXCEPTION_POINTERS* ep)
{
    using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                               PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
    HMODULE dbghelp = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32); // COLLAB: System32 only, never the app folder
    auto writeDump = dbghelp ? reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump")) : nullptr;
    if (writeDump) {
        wchar_t path[MAX_PATH];
        swprintf_s(path, L"%s\\Ploq-%lu-%llu.dmp", s_crashDumpDir, GetCurrentProcessId(),
                   static_cast<unsigned long long>(GetTickCount64()));
        HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION info { GetCurrentThreadId(), ep, FALSE };
            writeDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                      static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo),
                      &info, nullptr, nullptr);
            CloseHandle(f);
        }
    }
    LOGE() << "Oops! Application crashed (exception code " << static_cast<unsigned long>(ep->ExceptionRecord->ExceptionCode)
           << "); minidump written to COLLAB_CRASH_DUMP";
    return EXCEPTION_EXECUTE_HANDLER;
}

static bool ploqInstallCrashDump()
{
    const DWORD n = GetEnvironmentVariableW(L"COLLAB_CRASH_DUMP", s_crashDumpDir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return false;
    }
    SetUnhandledExceptionFilter(ploqWriteCrashDump);
    return true;
}

#endif

static void app_init_qrc()
{
    Q_INIT_RESOURCE(app);

#ifdef Q_OS_WIN
    Q_INIT_RESOURCE(app_win);
#endif
}

#ifdef MUSE_APP_UNSTABLE
// COLLAB: Ploq dev/QA only (tools/collab/qa-auto/shots.ps1): PLOQ_QA_GRAB=<folder> renders every
// visible window to <folder>/<n>-<title>.png once PLOQ_QA_GRAB_MS (default 9000) ms have passed, then
// lists them in <folder>/shots.done. Works on the offscreen platform, so nothing shows on screen.
static void ploqQaGrabIfAsked(QObject* ctx)
{
    const QString dir = qEnvironmentVariable("PLOQ_QA_GRAB");
    if (dir.isEmpty()) {
        return;
    }
    bool ok = false;
    int ms = qEnvironmentVariableIntValue("PLOQ_QA_GRAB_MS", &ok);
    QTimer::singleShot(ok ? ms : 9000, ctx, [dir]() {
        QDir().mkpath(dir);
        QStringList saved;
        int n = 0;
        for (QWindow* w : QGuiApplication::topLevelWindows()) {
            auto* qw = qobject_cast<QQuickWindow*>(w);
            if (!qw || !qw->isVisible()) {
                continue;
            }
            QString title = qw->title();
            title.replace(QRegularExpression("[^A-Za-z0-9 _-]"), "_");
            const QString path = QDir(dir).filePath(QString("%1-%2.png").arg(++n).arg(title.left(40)));
            const QImage img = qw->grabWindow();
            if (!img.isNull() && img.save(path)) {
                saved << path;
            }
            LOGI() << "ploq: QA grab of window '" << qw->title() << "' -> " << (saved.contains(path) ? path : QString("failed"));
        }
        QFile done(QDir(dir).filePath("shots.done"));
        if (done.open(QIODevice::WriteOnly)) {
            done.write(saved.join(QChar(u'\n')).toUtf8());
        }
    });
}

#endif

// COLLAB: Ploq: ploq:// links (and pasted invite URLs) are opened by Ploq.exe itself,
// registered as  "Ploq.exe" --ploq-link "%1"  (src/collab/collabmodule.cpp, the installer).
// No PowerShell in between, so no console window flashes up.
// Everything after --ploq-link on the raw command line is the link, as text: it must be one
// of the exact shapes below, and no other argument ever reaches the app, so a link can't
// smuggle in command-line options (e.g. a quote to end "%1" early). Same rules as
// tools/collab/portable/Open-Link.ps1.
#ifdef Q_OS_WIN
#include <windows.h>

static void ploqLinkMessage(const wchar_t* text)
{
    MessageBoxW(nullptr, text, L"Ploq", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
}

//! -1: not a link launch, start normally. -2: an invite / "Open in Ploq" link: start normally with
//! COLLAB_INVITE set and no other arguments. 0 / 1: a sign-in link handed over / a link refused: exit.
static int ploqHandleLink(int& argc, char** argv)
{
    static const char* const flagArg = "--ploq-link";
    if (argc < 2 || qstrcmp(argv[1], flagArg) != 0) {
        return -1; // only as the first argument, which is what the registration writes
    }
    // The link is the rest of the raw command line, not argv[2]: whatever a link does with
    // quotes and spaces, it all lands in this one string and fails the shape check below.
    const QString cmd = QString::fromWCharArray(GetCommandLineW());
    const qsizetype at = cmd.indexOf(QLatin1String(flagArg));
    if (at < 0) {
        ploqLinkMessage(L"That isn't a Ploq link, so nothing was opened.");
        return 1;
    }
    QString link = cmd.mid(at + qstrlen(flagArg)).trimmed();
    if (link.size() >= 2 && link.startsWith(u'"') && link.endsWith(u'"')) {
        link = link.mid(1, link.size() - 2);
    }

    static const QRegularExpression signin(QStringLiteral(R"(\Aploq://signin\?state=([0-9a-f]{32})&code=[0-9a-f]{32}/?\z)"));
    static const QRegularExpression invite(QStringLiteral(
        R"(\A(?:ploq://join/[0-9a-f]{24}|ploq://open/p_[0-9a-f]{24}\?code=[0-9a-f]{32}|https://collab\.ploq\.app/j/[0-9a-f]{24})/?\z)"));

    const QRegularExpressionMatch s = signin.match(link);
    if (s.hasMatch()) {
        // Back to the Ploq that's waiting for this sign-in (BrowserSignIn's QLocalServer).
        const std::wstring pipe = L"\\\\.\\pipe\\ploq-signin-" + s.captured(1).toStdWString();
        HANDLE h = INVALID_HANDLE_VALUE;
        for (int i = 0; i < 2 && h == INVALID_HANDLE_VALUE; ++i) {
            if (i > 0 && !WaitNamedPipeW(pipe.c_str(), 3000)) {
                break;
            }
            h = CreateFileW(pipe.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        }
        if (h == INVALID_HANDLE_VALUE) {
            ploqLinkMessage(L"This sign-in has expired, or it wasn't started in Ploq on this computer. "
                            L"In Ploq, click Share to sign in again.");
            return 1;
        }
        // If the waiting Ploq never reads, don't sit here as an invisible process forever.
        CreateThread(nullptr, 0, [](LPVOID) -> DWORD { Sleep(10000); ExitProcess(1); }, nullptr, 0, nullptr);
        const QByteArray bytes = (link + u'\n').toLatin1();
        DWORD written = 0;
        const bool ok = WriteFile(h, bytes.constData(), DWORD(bytes.size()), &written, nullptr) && written == DWORD(bytes.size());
        FlushFileBuffers(h);
        CloseHandle(h);
        return ok ? 0 : 1;
    }
    if (invite.match(link).hasMatch()) {
        if (!qEnvironmentVariableIsSet("COLLAB_SERVER")) {
            qputenv("COLLAB_SERVER", "https://collab.ploq.app");
        }
        if (!qEnvironmentVariableIsSet("COLLAB_COLOR")) {
            qputenv("COLLAB_COLOR", "#f58231");
        }
        qputenv("COLLAB_INVITE", link.toUtf8());
        // A leftover "restore the last session?" would hold up joining.
        QFile::remove(QDir::fromNativeSeparators(qEnvironmentVariable("LOCALAPPDATA")) + "/Ploq/Ploq/session/session.json");
        argc = 1; // nothing from the command line but the program name
        return -2;
    }
    ploqLinkMessage(L"That isn't a Ploq link, so nothing was opened.");
    return 1;
}
#endif

int main(int argc, char** argv)
{
#ifdef Q_OS_WIN
    {
        const int linkResult = ploqHandleLink(argc, argv); // COLLAB: Ploq: before anything else starts up
        if (linkResult >= 0) {
            return linkResult;
        }
    }
#endif

#ifndef MUSE_MODULE_DIAGNOSTICS_CRASHPAD_CLIENT
#ifdef Q_OS_WIN
    if (!ploqInstallCrashDump()) // COLLAB: Ploq: the signal handlers would swallow the crash before the dump
#endif
    {
        signal(SIGSEGV, crashCallback);
        signal(SIGILL, crashCallback);
        signal(SIGFPE, crashCallback);
    }
#endif

    // ====================================================
    // Setup global Qt application variables
    // ====================================================

    app_init_qrc();

    qputenv("QT_STYLE_OVERRIDE", "Fusion");
    qputenv("QML_DISABLE_DISK_CACHE", "true");

    // HACK: Workaround for crash #28840. This disables the incremental GC
    if (!qEnvironmentVariableIsSet("MU_QV4_GC_TIMELIMIT")) {
        qputenv("QV4_GC_TIMELIMIT", "0");
    }

    if (!qEnvironmentVariableIsSet("QT_QUICK_FLICKABLE_WHEEL_DECELERATION")) {
        qputenv("QT_QUICK_FLICKABLE_WHEEL_DECELERATION", "5000");
    }

#ifdef Q_OS_LINUX
    if (qEnvironmentVariable("MU_QT_QPA_PLATFORM") != "offscreen") {
        qputenv("QT_QPA_PLATFORMTHEME", "gtk3");
    }

    //! NOTE Forced X11, with Wayland there are a number of problems now
    if (qEnvironmentVariable("MU_QT_QPA_PLATFORM") == "") {
        qputenv("QT_QPA_PLATFORM", "xcb");
    }
#endif

#ifdef Q_OS_WIN
    // NOTE: There are some problems with rendering the application window on some integrated graphics processors
    //       see https://github.com/musescore/MuseScore/issues/8270
    if (!qEnvironmentVariableIsSet("QT_OPENGL_BUGLIST")) {
        qputenv("QT_OPENGL_BUGLIST", ":/resources/win_opengl_buglist.json");
    }
#endif

    QGuiApplication::styleHints()->setMousePressAndHoldInterval(250);

// Can't use MUSE_APP_TITLE until next major release, because this "application name" is used to determine
// where user settings are stored. Changing it would result in all user settings being lost.
// COLLAB: Ploq keeps its own settings (%APPDATA%\Ploq\Ploq.ini, %LOCALAPPDATA%\Ploq\Ploq) so it never
// touches a regular MuseScore install's files. See src/collab/ploqapp.h.
#ifdef MUSE_APP_UNSTABLE
    QCoreApplication::setApplicationName("PloqDevelopment"); // COLLAB: was MuseScore4Development
#else
    QCoreApplication::setApplicationName("Ploq"); // COLLAB: was MuseScore4
#endif
    QCoreApplication::setOrganizationName("Ploq"); // COLLAB: was MuseScore
    QCoreApplication::setOrganizationDomain("ploq.app"); // COLLAB: was musescore.org
    QCoreApplication::setApplicationVersion(MUSE_APP_VERSION);

#if !defined(Q_OS_WIN) && !defined(Q_OS_DARWIN) && !defined(Q_OS_WASM)
    // Any OS that uses Freedesktop.org Desktop Entry Specification (e.g. Linux, BSD)
#ifndef MUSE_APP_INSTALL_SUFFIX
#define MUSE_APP_INSTALL_SUFFIX ""
#endif
    QGuiApplication::setDesktopFileName("org.musescore.MuseScore" MUSE_APP_INSTALL_SUFFIX);
#endif

    using namespace muse;
    using namespace mu::app;

    auto fixSslBackend = []() {
#ifdef Q_OS_WIN
        // NOTE: Force schannel backend. Qt prefers OpenSSL when qopensslbackend.dll
        //       is present, which can crash on ABI mismatch with bundled OpenSSL.
        //       see https://github.com/musescore/MuseScore/issues/33401
        QSslSocket::setActiveBackend("schannel");
#endif
    };

    // ====================================================
    // Parse command line options
    // ====================================================
#ifdef MUE_ENABLE_CONSOLEAPP
    CommandLineParser commandLineParser;
    commandLineParser.init();
    commandLineParser.parse(argc, argv);

    IApplication::RunMode runMode = commandLineParser.runMode();
    QCoreApplication* qapp = nullptr;

    if (runMode == IApplication::RunMode::AudioPluginRegistration) {
        qapp = new QCoreApplication(argc, argv);
    } else {
        qapp = new QApplication(argc, argv);
    }

    fixSslBackend();

    commandLineParser.processBuiltinArgs(*qapp);
    CmdOptions opt = commandLineParser.options();

#else
    QCoreApplication* qapp = new QApplication(argc, argv);

    fixSslBackend();

    CmdOptions opt;
    opt.runMode = IApplication::RunMode::GuiApp;
#endif

    mu::collab::ploq::onAppCreated(); // COLLAB: Ploq version + first-launch settings (before settings load)

    AppFactory f;
    std::shared_ptr<muse::IApplication> app = f.newApp(opt);

    app->setup();

    app->setupNewContext();

    LOGI() << QString("SSL Info: supported: %1, build: %2, runtime: %3, active backend: %4, available backends: %5")
        .arg(QSslSocket::supportsSsl())
        .arg(QSslSocket::sslLibraryBuildVersionString())
        .arg(QSslSocket::sslLibraryVersionString())
        .arg(QSslSocket::activeBackend())
        .arg(QSslSocket::availableBackends().join(", "));

    // ====================================================
    // Run main loop
    // ====================================================
#ifdef MUSE_APP_UNSTABLE
    ploqQaGrabIfAsked(qapp); // COLLAB: Ploq
#endif

    int code = qapp->exec();

    // ====================================================
    // Quit
    // ====================================================

    app->finish();

    delete qapp;

    LOGI() << "Goodbye!! code: " << code;
    return code;
}
