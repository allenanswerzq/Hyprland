#pragma once

#include "../output/Monitor.hpp"
#include "../helpers/math/Direction.hpp"
#include "../actors/ActorTypes.hpp"

#include <string_view>
#include <optional>

namespace Aquamarine {
    class IOutput;
}

namespace State {
    class IMonitorStateTracker;

    class CMonitorQuery {
      public:
        CMonitorQuery(const IMonitorStateTracker&);
        ~CMonitorQuery() = default;

        CMonitorQuery(const CMonitorQuery&) = delete;
        CMonitorQuery(CMonitorQuery&)       = delete;
        CMonitorQuery(CMonitorQuery&&)      = delete;

        CMonitorQuery&& id(MONITORID id) &&;
        CMonitorQuery&& name(std::string_view name) &&;
        CMonitorQuery&& description(std::string_view desc) &&;
        CMonitorQuery&& selector(std::string_view str) &&;
        CMonitorQuery&& configString(std::string_view str) &&;
        CMonitorQuery&& vec(Vector2D vec) &&;
        CMonitorQuery&& output(SP<Aquamarine::IOutput> output) &&;
        CMonitorQuery&& inDirection(Math::eDirection dir) &&;
        CMonitorQuery&& relativeTo(PHLMONITOR reference) &&;
        CMonitorQuery&& includeDisabled(bool inc) &&;
        // Only consider monitors of this actor. Selector, config string and direction queries
        // default to the actor of relativeTo(), or the human, so a config string or a
        // direction never reaches another actor's desktop. Exact queries (id, name, output,
        // vec) are unfiltered unless an actor is set.
        CMonitorQuery&& actor(Actors::ActorID actor) &&;

        PHLMONITOR      run() &&;

      private:
        const std::vector<PHLMONITOR>&         mons() const;
        std::optional<Actors::ActorID>         actorFilter() const;

        std::optional<MONITORID>               m_id;
        std::optional<std::string_view>        m_name, m_desc, m_str, m_configString;
        std::optional<SP<Aquamarine::IOutput>> m_output;
        std::optional<PHLMONITOR>              m_relativeTo;
        std::optional<Math::eDirection>        m_dir;
        std::optional<Vector2D>                m_vec;
        std::optional<Actors::ActorID>         m_actor;
        bool                                   m_includeDisabled = false;
        const IMonitorStateTracker&            m_tracker;
    };
}
