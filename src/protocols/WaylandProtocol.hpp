#pragma once

#include "../defines.hpp"
#include "../helpers/memory/Memory.hpp"

#include <functional>

#define RESOURCE_OR_BAIL(resname)                                                                                                                                                  \
    const auto resname = (CWaylandResource*)wl_resource_get_user_data(resource);                                                                                                   \
    if (!resname)                                                                                                                                                                  \
        return;

#define PROTO NProtocols

class IWaylandProtocol;
struct SIWaylandProtocolDestroyWrapper {
    wl_listener       listener;
    IWaylandProtocol* parent = nullptr;
};

class IWaylandProtocol {
  public:
    // Decides which clients see the global. Without one, every client does (subject to the
    // compositor's global filter).
    using FClientFilter = std::function<bool(const wl_client*)>;

    IWaylandProtocol(const wl_interface* iface, const int& ver, const std::string& name, FClientFilter clientFilter = {});
    virtual ~IWaylandProtocol();

    // The protocol that created global, or nullptr for a global Hyprland did not create
    // through IWaylandProtocol. Works while wl_global_create is still running.
    static IWaylandProtocol*        fromGlobal(const wl_global* global);

    bool                            hasClientFilter() const;
    bool                            visibleTo(const wl_client* client) const;

    virtual void                    onDisplayDestroy();
    virtual void                    removeGlobal();
    virtual wl_global*              getGlobal();

    virtual void                    bindManager(wl_client* client, void* data, uint32_t ver, uint32_t id) = 0;

    SIWaylandProtocolDestroyWrapper m_liDisplayDestroy;

  private:
    std::string   m_name;
    FClientFilter m_clientFilter;
    wl_global*    m_global = nullptr;
};
