#include "FloatingAlgorithm.hpp"
#include "Algorithm.hpp"
#include "../space/Space.hpp"
#include "../target/Target.hpp"
#include "../../desktop/view/Window.hpp"
#include "../../managers/fullscreen/FullscreenController.hpp"
#include "../../managers/fullscreen/handler/FullscreenHandler.hpp"

using namespace Layout;

void IFloatingAlgorithm::recalculate(eRecalculateReason reason) {
    if (!m_parent || !m_parent->space())
        return;

    // Avoid further pos recalc if in fullscreen
    if (Fullscreen::controller()->hasFullscreen(m_parent->space()->workspace(), true)) {
        m_defaultFullscreenHandler->syncTargetSizeAndPosition();
        return;
    }
}

bool IFloatingAlgorithm::respawnIfBornFullscreen(SP<ITarget> t) {
    const auto WINDOW = t->window();

    if (!WINDOW || !WINDOW->m_bornFullscreen)
        return false;

    WINDOW->m_bornFullscreen = false;

    // only the client ever knew its windowed size — ask for it (0x0 configure)
    // and adopt its answer at commit. X11 semantics: the client owns its
    // normal geometry.
    WINDOW->requestClientSize();
    if (WINDOW->m_sizeFromClientSerial)
        return true;

    // X11: place like a fresh spawn
    t->rememberFloatingSize(FLOATING_DEFAULT_SIZE);
    t->setPositionGlobal({m_parent->space()->workArea().middle() - FLOATING_DEFAULT_SIZE / 2.F, FLOATING_DEFAULT_SIZE});
    return true;
}

void IFloatingAlgorithm::recenter(SP<ITarget> t) {
    if (respawnIfBornFullscreen(t))
        return;

    const auto LAST = t->lastFloatingSize();

    if (LAST.x <= 5 || LAST.y <= 5)
        return;

    t->setPositionGlobal({m_parent->space()->workArea().middle() - LAST / 2.F, LAST});
}
