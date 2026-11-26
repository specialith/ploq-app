/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Ploq — network guard (our code, not upstream). See ploqnetguard.h.
 */
#include "ploqnetguard.h"

#include <QNetworkProxyFactory>
#include <QStringList>

#include <atomic>

namespace mu::collab::ploq {
static std::atomic<int> s_blocked { 0 };

static const QStringList& museGroupDomains()
{
    static const QStringList domains {
        "musescore.org",    // updates.musescore.org, extensions.musescore.org, handbook, forum
        "musescore.com",    // desktop.musescore.com (cloud scores, publish)
        "musehub.com",      // customers-api.musehub.com (Muse Sounds store/news)
        "audio.com",        // api.audio.com (audio publishing)
        "muse.group",
        "musegroup.com",
        "museclouds.com",
    };
    return domains;
}

bool isMuseGroupHost(const QString& rawHost)
{
    const QString host = rawHost.trimmed().toLower();
    if (host.isEmpty()) {
        return false;
    }
    for (const QString& d : museGroupDomains()) {
        if (host == d || host.endsWith("." + d)) {
            return true;
        }
    }
    return false;
}

bool isMuseGroupUrl(const QUrl& url)
{
    if (isMuseGroupHost(url.host())) {
        return true;
    }
    // Path-style S3 URLs: https://s3.amazonaws.com/extensions.musescore.org/...
    if (url.host().toLower().endsWith("amazonaws.com")) {
        const QString first = url.path().section('/', 1, 1);
        return isMuseGroupHost(first);
    }
    return false;
}

namespace {
class GuardFactory : public QNetworkProxyFactory
{
public:
    explicit GuardFactory(bool useSystem)
        : m_useSystem(useSystem) {}

    QList<QNetworkProxy> queryProxy(const QNetworkProxyQuery& query) override
    {
        const bool blocked = query.url().isValid() && !query.url().isEmpty()
                             ? isMuseGroupUrl(query.url())
                             : isMuseGroupHost(query.peerHostName());
        if (blocked) {
            ++s_blocked;
            qWarning("ploq: blocked a connection to a Muse Group server: %s",
                     qPrintable(query.url().isEmpty() ? query.peerHostName() : query.url().host()));
            // A SOCKS proxy on a closed local port: the connection fails at once,
            // for HTTP and for raw sockets alike, without touching the network.
            return { QNetworkProxy(QNetworkProxy::Socks5Proxy, "127.0.0.1", 1) };
        }
        if (m_logAll) {
            qInfo("ploq: network connection to %s",
                  qPrintable(query.url().isEmpty() ? query.peerHostName() : query.url().host()));
        }
        if (m_useSystem) {
            return systemProxyForQuery(query);
        }
        return { QNetworkProxy(QNetworkProxy::NoProxy) };
    }

private:
    bool m_useSystem = false;
    bool m_logAll = qEnvironmentVariableIsSet("PLOQ_LOG_NETWORK"); // for tests: log every host Qt connects to
};
}

void installNetworkGuard()
{
    const bool useSystem = QNetworkProxyFactory::usesSystemConfiguration();
    QNetworkProxyFactory::setApplicationProxyFactory(new GuardFactory(useSystem)); // Qt takes ownership
}

int blockedConnectionCount()
{
    return s_blocked.load();
}
}
