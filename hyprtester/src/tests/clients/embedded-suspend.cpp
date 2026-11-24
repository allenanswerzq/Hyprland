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

// A window embedded in another (src/embed) stays on its own workspace, often a hidden one, and
// shows inside its host too. Hyprland must suspend it only when neither shows it: a client may
// stop drawing while suspended (Chrome does), which freezes the window inside a visible host.
// suspend-probe shows in its title whether it is suspended.

static constexpr auto EMBEDDED = "hyprtester-embedded";
static constexpr auto HOST     = "hyprtester-embed-host";
static constexpr auto HOST2    = "hyprtester-embed-host2";
static constexpr auto TAB      = "hyprtester-embed-tab";

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

static std::string dispatch(const std::string& call) {
    return getFromSocket(std::format("/dispatch {}", call));
}

static std::string embedIn(const char* host) {
    return getFromSocket(std::format("/embed class:{} class:{} 0 0 100 100 0 0", EMBEDDED, host));
}

TEST_CASE(embeddedWindowIsSuspendedOnlyWhenNothingShowsIt) {
    CUniquePointer<CProcess> probe;
    CScopeGuard              guard = {[&]() {
        getFromSocket(std::format("/unembed class:{}", EMBEDDED));
        if (probe && Tests::processAlive(probe->pid()))
            kill(probe->pid(), SIGKILL);
        Tests::killAllWindows();
        dispatch("hl.dsp.focus({ workspace = '1' })");
    }};

    OK(dispatch("hl.dsp.focus({ workspace = 'name:embed-host' })"));
    auto host = Tests::spawnKitty(HOST);
    if (!host)
        FAIL_TEST("Could not spawn kitty with class: {}", HOST);

    probe = makeUnique<CProcess>(std::format("{}/suspend-probe", binaryDir), std::vector<std::string>{EMBEDDED});
    probe->addEnv("WAYLAND_DISPLAY", WLDISPLAY);
    if (!probe->runAsync())
        FAIL_TEST("Could not start suspend-probe");
    Tests::waitUntilWindowsN(2);
    ASSERT(Tests::windowCount(), 2);
    ASSERT(probeIs("visible"), true);

    NLog::log("{}On a hidden workspace, nothing shows it", Colors::YELLOW);
    OK(dispatch(std::format("hl.dsp.window.move({{ workspace = 'special:embedded', follow = false, window = 'class:{}' }})", EMBEDDED)));
    EXPECT(probeIs("suspended"), true);

    NLog::log("{}Embedded in a window that shows", Colors::YELLOW);
    OK(embedIn(HOST));
    EXPECT(probeIs("visible"), true);

    NLog::log("{}Its host's workspace hidden, and shown again", Colors::YELLOW);
    OK(dispatch("hl.dsp.focus({ workspace = 'name:embed-away' })"));
    EXPECT(probeIs("suspended"), true);
    OK(dispatch("hl.dsp.focus({ workspace = 'name:embed-host' })"));
    EXPECT(probeIs("visible"), true);

    NLog::log("{}Its own workspace shown, while its host is hidden", Colors::YELLOW);
    OK(dispatch("hl.dsp.focus({ workspace = 'name:embed-away' })"));
    EXPECT(probeIs("suspended"), true);
    OK(dispatch("hl.dsp.workspace.toggle_special('embedded')"));
    EXPECT(probeIs("visible"), true);
    OK(dispatch("hl.dsp.workspace.toggle_special('embedded')"));
    EXPECT(probeIs("suspended"), true);
    OK(dispatch("hl.dsp.focus({ workspace = 'name:embed-host' })"));
    EXPECT(probeIs("visible"), true);

    NLog::log("{}Its host moved to a hidden workspace, and back", Colors::YELLOW);
    OK(dispatch(std::format("hl.dsp.window.move({{ workspace = 'name:embed-away', follow = false, window = 'class:{}' }})", HOST)));
    EXPECT(probeIs("suspended"), true);
    OK(dispatch(std::format("hl.dsp.window.move({{ workspace = 'name:embed-host', follow = false, window = 'class:{}' }})", HOST)));
    EXPECT(probeIs("visible"), true);

    NLog::log("{}Its host a hidden tab of a group, and the shown one again", Colors::YELLOW);
    OK(dispatch(std::format("hl.dsp.focus({{ window = 'class:{}' }})", HOST)));
    OK(dispatch("hl.dsp.group.toggle()"));
    auto tab = Tests::spawnKitty(TAB);
    if (!tab)
        FAIL_TEST("Could not spawn kitty with class: {}", TAB);
    EXPECT(probeIs("suspended"), true);
    OK(dispatch(std::format("hl.dsp.focus({{ window = 'class:{}' }})", HOST)));
    EXPECT(probeIs("visible"), true);
    OK(dispatch(std::format("hl.dsp.window.kill({{ window = 'class:{}' }})", TAB)));
    Tests::waitUntilWindowsN(2);

    NLog::log("{}Moved to a host on a hidden workspace, and back", Colors::YELLOW);
    OK(dispatch("hl.dsp.focus({ workspace = 'name:embed-away' })"));
    auto host2 = Tests::spawnKitty(HOST2);
    if (!host2)
        FAIL_TEST("Could not spawn kitty with class: {}", HOST2);
    OK(dispatch("hl.dsp.focus({ workspace = 'name:embed-host' })"));
    OK(embedIn(HOST2));
    EXPECT(probeIs("suspended"), true);
    OK(embedIn(HOST));
    EXPECT(probeIs("visible"), true);

    NLog::log("{}Unembedded, back on its hidden workspace only", Colors::YELLOW);
    OK(getFromSocket(std::format("/unembed class:{}", EMBEDDED)));
    EXPECT(probeIs("suspended"), true);

    NLog::log("{}Its host closed", Colors::YELLOW);
    OK(embedIn(HOST));
    EXPECT(probeIs("visible"), true);
    OK(dispatch(std::format("hl.dsp.window.kill({{ window = 'class:{}' }})", HOST)));
    EXPECT(probeIs("suspended"), true);
}
