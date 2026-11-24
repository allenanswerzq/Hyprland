#include <actors/ActorRegistry.hpp>
#include <macros.hpp>

#include <gtest/gtest.h>
#include <hyprutils/os/FileDescriptor.hpp>

#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using namespace Actors;
using namespace Hyprutils::OS;

namespace {
    class CTemporaryRuntimeDir {
      public:
        CTemporaryRuntimeDir() {
            std::string pattern = (std::filesystem::temp_directory_path() / "hyprland-actor-test-XXXXXX").string();
            if (mkdtemp(pattern.data()))
                m_path = pattern;
        }

        ~CTemporaryRuntimeDir() {
            if (!m_path.empty())
                std::filesystem::remove_all(m_path);
        }

        const std::string& path() const {
            return m_path;
        }

      private:
        std::string m_path;
    };

    std::array<CFileDescriptor, 2> socketPair() {
        std::array<int, 2> sockets = {-1, -1};
        EXPECT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets.data()), 0);
        return {CFileDescriptor{sockets[0]}, CFileDescriptor{sockets[1]}};
    }

    CFileDescriptor connectTo(const std::filesystem::path& path) {
        CFileDescriptor fd{socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)};
        EXPECT_TRUE(fd.isValid());

        sockaddr_un address = {};
        address.sun_family  = AF_UNIX;
        const auto PATH     = path.string();
        EXPECT_LT(PATH.size(), sizeof(address.sun_path));
        std::memcpy(address.sun_path, PATH.c_str(), PATH.size() + 1);
        EXPECT_EQ(connect(fd.get(), rc<sockaddr*>(&address), sizeof(address)), 0);
        return fd;
    }
}

TEST(ActorRegistry, ParsesAgentCount) {
    EXPECT_EQ(parseAgentCount("0").value(), 0U);
    EXPECT_EQ(parseAgentCount("16").value(), 16U);
    EXPECT_EQ(parseAgentCount("64").value(), 64U);

    EXPECT_FALSE(parseAgentCount(""));
    EXPECT_FALSE(parseAgentCount("-1"));
    EXPECT_FALSE(parseAgentCount("1x"));
    EXPECT_FALSE(parseAgentCount("65"));
}

TEST(ActorRegistry, NamesAgentsAndSockets) {
    EXPECT_EQ(agentName(1), "agent-001");
    EXPECT_EQ(agentSocketName(16), "wayland-agent-016");
}

class CActorRegistryTest : public ::testing::Test {
  protected:
    void SetUp() override {
        m_display.reset(wl_display_create());
        ASSERT_TRUE(m_display);
        m_registry = makeUnique<CActorRegistry>(m_display.get());
    }

    void TearDown() override {
        if (m_display)
            wl_display_destroy_clients(m_display.get());
        m_registry.reset();
    }

    std::unique_ptr<wl_display, decltype(&wl_display_destroy)> m_display{nullptr, wl_display_destroy};
    UP<CActorRegistry>                                         m_registry;
};

TEST_F(CActorRegistryTest, ClientsFromTheMainDisplayBelongToTheHuman) {
    auto sockets = socketPair();
    auto client  = wl_client_create(m_display.get(), sockets[0].get());
    ASSERT_NE(client, nullptr);
    sockets[0].take();

    EXPECT_EQ(m_registry->ownerOf(client), HUMAN_ACTOR_ID);
    EXPECT_EQ(m_registry->clientCount(HUMAN_ACTOR_ID), 1U);

    wl_client_destroy(client);
    EXPECT_FALSE(m_registry->ownerOf(client));
    EXPECT_EQ(m_registry->clientCount(HUMAN_ACTOR_ID), 0U);
}

TEST_F(CActorRegistryTest, RejectsClientsForUnknownActors) {
    auto sockets = socketPair();

    EXPECT_EQ(m_registry->createClient(sockets[0].get(), 99), nullptr);
    EXPECT_NE(fcntl(sockets[0].get(), F_GETFD), -1);
}

TEST_F(CActorRegistryTest, AgentSocketAssignsTheAgentBeforeTheClientRuns) {
    CTemporaryRuntimeDir runtimeDir;
    ASSERT_FALSE(runtimeDir.path().empty());
    ASSERT_TRUE(m_registry->createAgentSockets(1, runtimeDir.path()));

    ASSERT_EQ(m_registry->actors().size(), 2U);
    const auto AGENT = m_registry->actors()[1];
    EXPECT_EQ(AGENT.id, 1U);
    EXPECT_EQ(AGENT.kind, eActorKind::AGENT);
    EXPECT_EQ(AGENT.name, "agent-001");
    EXPECT_EQ(AGENT.waylandSocket, "wayland-agent-001");

    auto connection = connectTo(std::filesystem::path{runtimeDir.path()} / AGENT.waylandSocket);
    ASSERT_TRUE(connection.isValid());
    ASSERT_GE(wl_event_loop_dispatch(wl_display_get_event_loop(m_display.get()), 0), 0);

    EXPECT_EQ(m_registry->clientCount(AGENT.id), 1U);
    EXPECT_EQ(m_registry->clientCount(HUMAN_ACTOR_ID), 0U);
}

TEST_F(CActorRegistryTest, AgentSocketPoolIsAllOrNothing) {
    CTemporaryRuntimeDir runtimeDir;
    ASSERT_FALSE(runtimeDir.path().empty());
    ASSERT_TRUE(m_registry->createAgentSockets(2, runtimeDir.path()));

    std::unique_ptr<wl_display, decltype(&wl_display_destroy)> otherDisplay{wl_display_create(), wl_display_destroy};
    ASSERT_TRUE(otherDisplay);
    {
        CActorRegistry otherRegistry{otherDisplay.get()};
        EXPECT_FALSE(otherRegistry.createAgentSockets(2, runtimeDir.path()));
        EXPECT_EQ(otherRegistry.actors().size(), 1U);
    }

    EXPECT_TRUE(std::filesystem::exists(std::filesystem::path{runtimeDir.path()} / "wayland-agent-001"));
    EXPECT_TRUE(std::filesystem::exists(std::filesystem::path{runtimeDir.path()} / "wayland-agent-002"));
}

TEST_F(CActorRegistryTest, BorrowChangesTheControllerButNotTheOwner) {
    CTemporaryRuntimeDir runtimeDir;
    ASSERT_FALSE(runtimeDir.path().empty());
    ASSERT_TRUE(m_registry->createAgentSockets(1, runtimeDir.path()));

    auto sockets = socketPair();
    auto client  = m_registry->createClient(sockets[0].get(), 1);
    ASSERT_NE(client, nullptr);
    sockets[0].take();

    EXPECT_EQ(m_registry->ownerOf(client), 1U);
    EXPECT_EQ(m_registry->controllerOf(client), 1U);

    ASSERT_TRUE(m_registry->setController(client, HUMAN_ACTOR_ID));
    EXPECT_EQ(m_registry->ownerOf(client), 1U);
    EXPECT_EQ(m_registry->controllerOf(client), HUMAN_ACTOR_ID);
    EXPECT_EQ(m_registry->clientCount(1), 1U);
    EXPECT_EQ(m_registry->clientCount(HUMAN_ACTOR_ID), 0U);

    ASSERT_TRUE(m_registry->setController(client, 1));
    EXPECT_EQ(m_registry->controllerOf(client), 1U);

    EXPECT_FALSE(m_registry->setController(client, 99));
    EXPECT_EQ(m_registry->controllerOf(client), 1U);
}

TEST_F(CActorRegistryTest, ClientDestroyedStillKnowsOwnerAndController) {
    CTemporaryRuntimeDir runtimeDir;
    ASSERT_FALSE(runtimeDir.path().empty());
    ASSERT_TRUE(m_registry->createAgentSockets(1, runtimeDir.path()));

    auto sockets = socketPair();
    auto client  = m_registry->createClient(sockets[0].get(), 1);
    ASSERT_NE(client, nullptr);
    sockets[0].take();
    ASSERT_TRUE(m_registry->setController(client, HUMAN_ACTOR_ID));

    bool       seen     = false;
    const auto LISTENER = m_registry->m_events.clientDestroyed.listen([&](wl_client* destroyed) {
        seen = true;
        EXPECT_EQ(destroyed, client);
        EXPECT_EQ(m_registry->ownerOf(destroyed), 1U);
        EXPECT_EQ(m_registry->controllerOf(destroyed), HUMAN_ACTOR_ID);
    });

    wl_client_destroy(client);

    EXPECT_TRUE(seen);
    EXPECT_FALSE(m_registry->ownerOf(client));
    EXPECT_FALSE(m_registry->controllerOf(client));
}

TEST_F(CActorRegistryTest, AgentOutputNamesAreReserved) {
    CTemporaryRuntimeDir runtimeDir;
    ASSERT_FALSE(runtimeDir.path().empty());
    ASSERT_TRUE(m_registry->createAgentSockets(1, runtimeDir.path()));

    EXPECT_EQ(m_registry->actorForOutputName("AGENT-001"), 1U);
    EXPECT_FALSE(m_registry->actorForOutputName("AGENT-002"));
    EXPECT_FALSE(m_registry->actorForOutputName("DP-1"));

    EXPECT_TRUE(m_registry->isReservedOutputName("AGENT-001"));
    EXPECT_TRUE(m_registry->isReservedOutputName("AGENT-999"));
    EXPECT_FALSE(m_registry->isReservedOutputName("DP-1"));
}

TEST(ActorRegistryLifetime, RegistryCanBeDestroyedBeforeClients) {
    std::unique_ptr<wl_display, decltype(&wl_display_destroy)> display{wl_display_create(), wl_display_destroy};
    ASSERT_TRUE(display);

    auto registry = makeUnique<CActorRegistry>(display.get());
    auto sockets  = socketPair();
    auto client   = wl_client_create(display.get(), sockets[0].get());
    ASSERT_NE(client, nullptr);
    sockets[0].take();

    registry.reset();
    wl_client_destroy(client);
}
