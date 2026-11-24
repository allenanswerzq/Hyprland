#include "MonitorState.hpp"
#include "FallbackState.hpp"

#include "../event/EventBus.hpp"
#include "../output/Monitor.hpp"
#include "../render/Renderer.hpp"
#include "../config/shared/monitor/MonitorRuleManager.hpp"
#include "../protocols/ColorManagement.hpp"
#include "../output/MonitorFrameScheduler.hpp"
#include "../Compositor.hpp"
#include "../managers/input/InputManager.hpp"
#include "../pointer/PointerManager.hpp"
#include "../pointer/PointerController.hpp"

#include <algorithm>

using namespace State;

// An agent's monitor (src/actors) shows the agent's desktop. It never joins the human's lists.
static bool isAgentMonitor(const PHLMONITOR& monitor) {
    return monitor && monitor->m_actor != Actors::HUMAN_ACTOR_ID;
}

UP<CMonitorStateTracker>& State::monitorState() {
    static UP<CMonitorStateTracker> p = makeUnique<CMonitorStateTracker>();
    return p;
}

CMonitorStateTracker::CMonitorStateTracker() {
    m_listeners.monitorAdded   = Event::bus()->m_events.monitor.added.listen([this](PHLMONITOR m) {
        if (!isAgentMonitor(m) && !std::ranges::contains(m_monitors, m))
            m_monitors.emplace_back(m);
        syncRenderMonitors();
    });
    m_listeners.monitorRemoved = Event::bus()->m_events.monitor.removed.listen([this](PHLMONITOR m) {
        std::erase(m_monitors, m);
        syncRenderMonitors();
    });

    // keep track of mirrors
    m_listeners.layoutChanged = Event::bus()->m_events.monitor.layoutChanged.listen([this]() { syncActiveMonitors(); });
}

void CMonitorStateTracker::syncActiveMonitors() {
    m_monitors.clear();

    for (const auto& rm : m_realMonitors) {
        if (!rm->m_enabled || rm->isMirror())
            continue;

        m_monitors.emplace_back(rm);
    }

    syncRenderMonitors();
}

void CMonitorStateTracker::syncRenderMonitors() {
    m_renderMonitors = m_monitors;

    for (const auto& m : m_agentMonitors) {
        if (m->m_enabled && !m->isMirror())
            m_renderMonitors.emplace_back(m);
    }
}

const std::vector<PHLMONITOR>& CMonitorStateTracker::allMonitors() const {
    return m_realMonitors;
}

const std::vector<PHLMONITOR>& CMonitorStateTracker::monitors() const {
    return m_monitors;
}

const std::vector<PHLMONITOR>& CMonitorStateTracker::agentMonitors() const {
    return m_agentMonitors;
}

const std::vector<PHLMONITOR>& CMonitorStateTracker::renderMonitors() const {
    return m_renderMonitors;
}

MONITORID CMonitorStateTracker::getNextAvailableMonitorID(const std::string& name) {
    // Human and agent monitors share one ID space.
    const auto USED = [this](MONITORID id) {
        return std::ranges::any_of(m_realMonitors, [id](const auto& m) { return m->m_id == id; }) ||
            std::ranges::any_of(m_agentMonitors, [id](const auto& m) { return m->m_id == id; });
    };

    // reuse ID if it's already in the map, and the monitor with that ID is not being used by another monitor
    if (m_monitorIDMap.contains(name) && !USED(m_monitorIDMap[name]))
        return m_monitorIDMap[name];

    // otherwise, find minimum available ID that is not in the map
    MONITORID nextID = 0;
    while (USED(nextID)) {
        nextID++;
    }
    m_monitorIDMap[name] = nextID;
    return nextID;
}

static void checkDefaultCursorWarp(PHLMONITOR monitor) {
    static auto PCURSORMONITOR    = CConfigValue<std::string>("cursor:default_monitor");
    static bool cursorDefaultDone = false;
    static bool firstLaunch       = true;

    const auto  POS = monitor->middle();

    // by default, cursor should be set to first monitor detected
    // this is needed as a default if the monitor given in config above doesn't exist
    if (firstLaunch) {
        firstLaunch = false;
        Pointer::pointerController()->warpTo(POS, true);
        g_pInputManager->refocus();
        return;
    }

    if (!cursorDefaultDone && *PCURSORMONITOR != STRVAL_EMPTY) {
        const auto MONITOR = State::monitorState()->query().configString(*PCURSORMONITOR).run();
        if (MONITOR && MONITOR->m_id == monitor->m_id) {
            cursorDefaultDone = true;
            Pointer::pointerController()->warpTo(POS, true);
            g_pInputManager->refocus();
            return;
        }
    }

    // modechange happened check if cursor is on that monitor and warp it to middle to not place it out of bounds if resolution changed.
    if (State::monitorState()->query().vec(Pointer::mgr()->untransformedPosition()).run() == monitor) {
        Pointer::pointerController()->warpTo(POS, true);
        g_pInputManager->refocus();
    }
}

void CMonitorStateTracker::add(PHLMONITOR mon) {

    mon->m_id   = getNextAvailableMonitorID(mon->m_name);
    mon->m_self = mon;

    const bool AGENT = isAgentMonitor(mon);
    if (AGENT)
        m_agentMonitors.emplace_back(mon);
    else
        m_realMonitors.emplace_back(mon);

    LOG(Log::DEBUG, "[CMonitorStateTracker] New monitor: {}", mon->m_name);

    Event::bus()->m_events.monitor.newMon.emit(mon);

    mon->onConnect(false);

    if (!mon->m_enabled)
        return;

    // ready to process if we have a real monitor

    if (!AGENT && (!g_pHyprRenderer->m_mostHzMonitor || mon->m_refreshRate > g_pHyprRenderer->m_mostHzMonitor->m_refreshRate) && mon->m_enabled)
        g_pHyprRenderer->m_mostHzMonitor = mon;

    Config::monitorRuleMgr()->scheduleReload();

    mon->scheduleFrame(Aquamarine::IOutput::AQ_SCHEDULE_NEW_MONITOR);

    // The cursor is the human's.
    if (!AGENT)
        checkDefaultCursorWarp(mon);

    mon->updateSurfaceScaleTransformDetails();

    g_pHyprRenderer->damageMonitor(mon);
    mon->m_frameScheduler->onFrame();

    if (PROTO::colorManagement && g_pCompositor->shouldChangePreferredImageDescription()) {
        LOG(Log::ERR, "FIXME: color management protocol is enabled, need a preferred image description id");
        PROTO::colorManagement->onImagePreferredChanged(0);
    }
}

void CMonitorStateTracker::add(SP<Aquamarine::IOutput> output) {
    add(makeShared<Monitor::CMonitor>(output));
}

void CMonitorStateTracker::remove(PHLMONITOR mon) {
    std::erase(m_realMonitors, mon);
    std::erase(m_monitors, mon);
    std::erase(m_agentMonitors, mon);
    syncRenderMonitors();

    Event::bus()->m_events.monitor.destroyMon.emit(mon);
}

void CMonitorStateTracker::finish() {
    m_realMonitors.clear();
    m_monitors.clear();
    m_agentMonitors.clear();
    m_renderMonitors.clear();
}
