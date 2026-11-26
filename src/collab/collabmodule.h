/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Collab fork — real-time collaboration module.
 * All collaboration code lives under src/collab/. Upstream files only get
 * small hooks marked "// COLLAB:" (git grep -n "COLLAB:" lists them).
 */
#pragma once

#include <memory>

#include "modularity/imodulesetup.h"

namespace mu::collab {
class CollabSyncService;

class CollabModule : public muse::modularity::IModuleSetup
{
public:
    std::string moduleName() const override;
    void registerExports() override;
    void resolveImports() override;
    void onAllInited(const muse::IApplication::RunMode& mode) override;
    void onDeinit() override;

private:
    std::shared_ptr<CollabSyncService> m_syncService;
};
}
