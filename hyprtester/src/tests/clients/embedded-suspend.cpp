#include "../../hyprctlCompat.hpp"
#include "../../shared.hpp"
#include "../shared.hpp"
#include "build.hpp"
#include "tests.hpp"

#include <chrono>
#include <csignal>
#include <format>
#include <string>
#include <thread>
#include <vector>

#include <hyprutils/os/Process.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>

using namespace Hyprutils::Memory;
using namespace Hyprutils::OS;
using namespace Hyprutils::Utils;

// A window embedded in another (src/embed) stays on a hidden workspace, but shows wherever
// its host shows. Hyprland must suspend it only while the host is out of sight: a client may
// stop drawing while suspended (Chrome does), which freezes the window inside a visible host.

static constexpr auto EMBEDDED = "hyprtester-embedded";
static constexpr auto HOST     = "hyprtester-embed-host";

// Waits until the probe's title says it is suspended, or visible.
static bool probeIs(const char* state) {
    const auto TITLE = std::format("title: {} {}", EMBEDDED, state);
    for (size_t i = 0; i < 50; ++i) {
        if (getFromSocket("/clients").contains(TITLE))
            return true;

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    return false;
}

TEST_CASE(embeddedWindowIsSuspendedOnlyWhenItsHostIsHidden) {
    CUniquePointer<CProcess> probe;
    CScopeGuard              guard = {[&]() {
        getFromSocket(std::format("/unembed class:{}", EMBEDDED));
        if (probe && Tests::processAlive(probe->pid()))
            kill(probe->pid(), SIGKILL);
        Tests::killAllWindows();
        getFromSocket("/dispatch hl.dsp.focus({ workspace = '1' })");
    }};

    OK(getFromSocket("/dispatch hl.dsp.focus({ workspace = 'name:embed-host' })"));
    auto host = Tests::spawnKitty(HOST);
    if (!host)
        FAIL_TEST("Could not spawn kitty with class: {}", HOST);

    probe = makeUnique<CProcess>(std::format("{}/suspend-probe", binaryDir), std::vector<std::string>{EMBEDDED});
    probe->addEnv("WAYLAND_DISPLAY", WLDISPLAY);
    if (!probe->runAsync())
        FAIL_TEST("Could not start suspend-probe");
    Tests::waitUntilWindowsN(2);
    ASSERT(Tests::windowCount(), 2);

    OK(getFromSocket(std::format("/dispatch hl.dsp.window.move({{ workspace = 'special:embedded', follow = false, window = 'class:{}' }})", EMBEDDED)));
    EXPECT(probeIs("suspended"), true);

    OK(getFromSocket(std::format("/embed class:{} class:{} 0 0 100 100 0 0", EMBEDDED, HOST)));
    EXPECT(probeIs("visible"), true);

    OK(getFromSocket("/dispatch hl.dsp.focus({ workspace = 'name:embed-away' })"));
    EXPECT(probeIs("suspended"), true);

    OK(getFromSocket("/dispatch hl.dsp.focus({ workspace = 'name:embed-host' })"));
    EXPECT(probeIs("visible"), true);

    OK(getFromSocket(std::format("/unembed class:{}", EMBEDDED)));
    EXPECT(probeIs("suspended"), true);
}
