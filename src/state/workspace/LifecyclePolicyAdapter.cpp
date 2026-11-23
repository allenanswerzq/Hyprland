#include "LifecyclePolicyAdapter.hpp"

#include "LifecyclePolicy.hpp"
#include "../../actors/Actors.hpp"
#include "../../animation/WorkspaceAnimationController.hpp"
#include "../../config/shared/workspace/WorkspaceRuleManager.hpp"
#include "../../debug/log/Logger.hpp"
#include "../../workspace/HLWorkspace.hpp"
#include "../../output/Monitor.hpp"
#include "../MonitorState.hpp"
#include "PlacementController.hpp"
#include "Resolver.hpp"
#include "State.hpp"

#include <algorithm>
#include <limits>
#include <ranges>
#include <utility>

using namespace State::Workspace;

static UP<CMonitorLifecyclePolicy>& lifecyclePolicy() {
    static UP<CMonitorLifecyclePolicy> policy = makeUnique<CMonitorLifecyclePolicy>();
    return policy;
}

static SWorkspaceIdentity workspaceIdentity(const PHLWORKSPACE& workspace) {
    return {
        .id      = workspace->id(),
        .address = workspace->addressableName(),
        .type    = workspace->type(),
    };
}

static void finishLifecycleTransition(const PHLWORKSPACE& workspace) {
    Animation::Workspace::startAnimation(workspace, Animation::Workspace::ANIMATION_TYPE_IN, true, true);
}

static SMonitorSnapshot monitorSnapshot(const PHLMONITOR& monitor) {
    return {
        .address         = monitor->m_name,
        .activeWorkspace = monitor->m_activeWorkspace ? std::optional{workspaceIdentity(monitor->m_activeWorkspace)} : std::nullopt,
        .fallback        = monitor->m_isUnsafeFallback,
    };
}

class CHyprlandPolicyContext final : public IPolicyContext {
  public:
    // Each actor has its own desktop, so the policy only sees monitors and workspaces of the
    // event monitor's actor. A human monitor unplug never moves agent workspaces, and the
    // other way around.
    CHyprlandPolicyContext(PHLMONITOR eventMonitor) :
        m_eventMonitor(std::move(eventMonitor)), m_actor(Actors::actorOf(m_eventMonitor)), m_transitionWorkspaces(state()->workspacesCopy()) {
        ;
    }

    std::vector<SMonitorSnapshot> monitors() const override {
        std::vector<SMonitorSnapshot> snapshots;
        snapshots.reserve(State::monitorState()->monitors().size() + 1);

        for (const auto& monitor : State::monitorState()->monitors()) {
            if (Actors::actorOf(monitor) != m_actor)
                continue;

            snapshots.emplace_back(monitorSnapshot(monitor));
        }

        if (m_eventMonitor && std::ranges::none_of(snapshots, [&](const auto& candidate) { return candidate.address == m_eventMonitor->m_name; }))
            snapshots.emplace_back(monitorSnapshot(m_eventMonitor));

        return snapshots;
    }

    std::vector<SWorkspaceSnapshot> workspaces() const override {
        std::vector<SWorkspaceSnapshot> snapshots;
        snapshots.reserve(std::ranges::distance(state()->workspaces()));

        for (const auto& workspace : state()->workspaces()) {
            if (!workspace)
                continue;

            if (workspace->actor() != m_actor)
                continue;

            const auto MONITOR = workspace->m_monitor.lock();
            snapshots.emplace_back(SWorkspaceSnapshot{
                .identity       = workspaceIdentity(workspace.lock()),
                .monitorAddress = MONITOR ? std::optional{MONITOR->m_name} : std::nullopt,
            });
        }

        return snapshots;
    }

    std::optional<SWorkspaceIdentity> configuredDefaultWorkspace(std::string_view monitorAddress) const override {
        const auto MONITOR = findMonitor(monitorAddress);
        if (!MONITOR || Actors::isAgentMonitor(MONITOR))
            return std::nullopt;

        const auto CONFIGURED = Config::workspaceRuleMgr()->getDefaultWorkspaceFor(*MONITOR);
        if (CONFIGURED.empty())
            return std::nullopt;

        const auto TARGET = resolver()->getWorkspaceTargetFromString(CONFIGURED, MONITOR);
        if (!TARGET.valid() || TARGET.type == ::Workspace::eWorkspaceType::SPECIAL) {
            LOG(Log::DEBUG, "Invalid workspace= directive name in monitor parsing, workspace name \"{}\" is invalid.", CONFIGURED);
            return std::nullopt;
        }

        if (const auto WORKSPACE = state()->find(TARGET); WORKSPACE)
            return workspaceIdentity(WORKSPACE);

        return SWorkspaceIdentity{*TARGET.id, TARGET.address, TARGET.type};
    }

    std::vector<SDefaultWorkspaceCandidate> defaultWorkspaceCandidates(std::string_view monitorAddress) const override {
        const auto MONITOR = findMonitor(monitorAddress);
        if (!MONITOR)
            return {};

        // An agent has exactly one workspace, named after the agent.
        if (Actors::isAgentMonitor(MONITOR)) {
            return {{
                .identity            = SWorkspaceIdentity{::Workspace::SWorkspaceSpecialID{}, Actors::nameOf(MONITOR->m_actor), ::Workspace::eWorkspaceType::NORMAL},
                .boundMonitorAddress = std::nullopt,
            }};
        }

        for (::Workspace::WorkspaceIDContainer id = 1; id < std::numeric_limits<::Workspace::WorkspaceIDContainer>::max(); ++id) {
            const auto               ADDRESS = std::to_string(id);
            const SWorkspaceIdentity IDENTITY{::Workspace::SWorkspaceNumberedID{id}, ADDRESS};
            if (findWorkspace(IDENTITY))
                continue;

            const auto BOUND = Config::workspaceRuleMgr()->getBoundMonitorStringForWS(ADDRESS);
            if (!BOUND.empty() && !MONITOR->matchesStaticSelector(BOUND))
                continue;

            return {{
                .identity            = IDENTITY,
                .boundMonitorAddress = BOUND.empty() ? std::nullopt : std::optional{MONITOR->m_name},
            }};
        }

        return {};
    }

    void createWorkspace(const SWorkspaceIdentity& identity, std::string_view monitorAddress) override {
        const auto MONITOR = findMonitor(monitorAddress);
        if (!MONITOR || findWorkspace(identity))
            return;

        const STarget TARGET{.id = identity.id, .address = identity.address, .displayName = identity.address, .type = identity.type};
        if (!TARGET.valid() || TARGET.type == ::Workspace::eWorkspaceType::SPECIAL) {
            LOG(Log::ERR, "Failed to create lifecycle workspace {} on monitor {}", identity.address, monitorAddress);
            return;
        }

        if (const auto ws = state()->create(TARGET, MONITOR)) {
            m_createdWorkspace = ws;
            ws->ready();
            finishLifecycleTransition(ws);
        }
    }

    void moveWorkspace(const SWorkspaceIdentity& identity, std::string_view monitorAddress, bool replace) override {
        const auto WORKSPACE = findWorkspace(identity);
        placementController()->moveWorkspaceToMonitor(WORKSPACE, findMonitor(monitorAddress), true, false, replace);
        finishLifecycleTransition(WORKSPACE);
    }

    void activateWorkspace(const SWorkspaceIdentity& identity, std::string_view monitorAddress) override {
        const auto MONITOR   = findMonitor(monitorAddress);
        const auto WORKSPACE = findWorkspace(identity);
        if (!MONITOR || !WORKSPACE)
            return;

        MONITOR->changeWorkspace(WORKSPACE, true, false, false);
        WORKSPACE->setVisible(true);
        finishLifecycleTransition(WORKSPACE);
    }

  private:
    PHLMONITOR findMonitor(std::string_view address) const {
        if (m_eventMonitor && m_eventMonitor->m_name == address)
            return m_eventMonitor;

        const auto MONITOR = std::ranges::find(State::monitorState()->monitors(), address, &Monitor::CMonitor::m_name);
        if (MONITOR == State::monitorState()->monitors().end() || Actors::actorOf(*MONITOR) != m_actor)
            return nullptr;

        return *MONITOR;
    }

    static PHLWORKSPACE findWorkspace(const SWorkspaceIdentity& identity) {
        return state()->query().identity(identity.id, identity.address, identity.type).run();
    }

    PHLMONITOR                m_eventMonitor;
    Actors::ActorID           m_actor;
    std::vector<PHLWORKSPACE> m_transitionWorkspaces;
    PHLWORKSPACE              m_createdWorkspace;
};

void State::Workspace::monitorConnected(PHLMONITOR monitor) {
    if (!monitor)
        return;

    CHyprlandPolicyContext context{monitor};
    lifecyclePolicy()->monitorConnected(monitorSnapshot(monitor), context);
}

void State::Workspace::monitorDisconnected(PHLMONITOR monitor) {
    if (!monitor)
        return;

    CHyprlandPolicyContext context{monitor};
    lifecyclePolicy()->monitorDisconnected(monitorSnapshot(monitor), context);
}

void State::Workspace::workspaceDestroyed(const SWorkspaceIdentity& identity) {
    lifecyclePolicy()->workspaceDestroyed(identity);
}

void State::Workspace::workspaceIdentityChanged(const SWorkspaceIdentity& oldIdentity, const SWorkspaceIdentity& newIdentity) {
    lifecyclePolicy()->workspaceIdentityChanged(oldIdentity, newIdentity);
}

void State::Workspace::clearLifecyclePolicy() {
    lifecyclePolicy()->clear();
}
