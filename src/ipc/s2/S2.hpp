#pragma once

#include <string>

#include "../../actors/ActorTypes.hpp"
#include "../../helpers/memory/Memory.hpp"

namespace IPC::Socket2 {
    struct SEvent {
        std::string event;
        std::string data;
        // What the event is about. It decides whose socket2 gets the event: the human's, or
        // the socket2 of the agent whose desktop the subject is on. No subject means the
        // human's (Actors::eventAudience).
        Actors::SEventSubject subject = {};
    };

    class IImplementation;

    class CSocket2 {
      public:
        // directory holds .socket2.sock; the instance path by default.
        explicit CSocket2(std::string directory = "");
        ~CSocket2() = default;

        // Sends the event to its audience (see SEvent::subject).
        void postEvent(SEvent&& event);
        // Sends the event to this socket's listeners, whoever it is about.
        void send(const SEvent& event);

      private:
        UP<IImplementation> m_impl;
    };

    UP<CSocket2>& sock();
};
