#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/state/ViewState.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/desktop/view/window/WindowPresentation.hpp>
#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>

#define private public
#include <hyprland/src/layout/algorithm/tiled/scrolling/ScrollTapeController.hpp>
#include <hyprland/src/layout/algorithm/tiled/scrolling/ScrollingAlgorithm.hpp>
#undef private

using namespace Layout;
using namespace Layout::Tiled;

namespace {

    constexpr float MIN_COLUMN_WIDTH = 0.05F;
    constexpr float MAX_COLUMN_WIDTH = 1.0F;
    constexpr float MIN_ROW_HEIGHT   = 0.1F;
    constexpr float MAX_ROW_HEIGHT   = 1.0F;

    HANDLE               g_handle           = nullptr;
    CFunctionHook*       g_hookCameraOffset = nullptr;
    CFunctionHook*       g_hookNewTarget    = nullptr;
    CFunctionHook*       g_hookRemoveTarget = nullptr;
    CFunctionHook*       g_hookResizeTarget = nullptr;

    WP<ITarget>          g_draggedTarget;
    std::optional<float> g_draggedWidth;

    using CalculateCameraOffsetFn = double (*)(CScrollTapeController*, const CBox&, bool);
    using NewTargetFn             = void (*)(CScrollingAlgorithm*, SP<ITarget>);
    using RemoveTargetFn          = void (*)(CScrollingAlgorithm*, SP<ITarget>);
    using ResizeTargetFn          = void (*)(CScrollingAlgorithm*, const Vector2D&, SP<ITarget>, eRectCorner);
    bool isTapeBeingDragged(const CScrollTapeController* ctrl) {
        const auto dragTarget = g_layoutManager->dragController()->target();
        if (!dragTarget)
            return false;

        for (const auto& s : ctrl->m_strips) {
            if (!s.userData)
                continue;
            for (const auto& d : s.userData->targetDatas) {
                if (d->target == dragTarget)
                    return true;
            }
        }
        return false;
    }

    // 1) Right-aligned camera for SCROLL_DIR_LEFT:
    //    - When all columns fit in the viewport, keep offset at 0.0 (flush right)
    //      instead of centering.
    //    - When columns overflow, clamp offset to [0.0, maxExtent - usablePrimary]
    //      so neither edge ever develops dead space.
    double hkCalculateCameraOffset(CScrollTapeController* self, const CBox& usableArea, bool fullscreenOnOne) {
        if (!self || self->getDirection() != SCROLL_DIR_LEFT)
            return reinterpret_cast<CalculateCameraOffsetFn>(g_hookCameraOffset->m_original)(self, usableArea, fullscreenOnOne);

        if (isTapeBeingDragged(self))
            return self->getOffset();

        const double maxExtent     = self->calculateMaxExtent(usableArea, fullscreenOnOne);
        const double usablePrimary = usableArea.w;

        if (maxExtent <= usablePrimary) {
            self->setOffset(0.0);
        } else {
            const double maxOffset = maxExtent - usablePrimary;
            const double cur       = self->getOffset();
            if (cur < 0.0)
                self->setOffset(0.0);
            else if (cur > maxOffset)
                self->setOffset(maxOffset);
        }

        return self->getOffset();
    }

    // 2) Stash a target's column width before removeTarget destroys SColumnData
    //    so both mouse DnD and workspace-to-workspace moves preserve its width.
    void hkRemoveTarget(CScrollingAlgorithm* self, SP<ITarget> target) {
        if (self && target) {
            if (const auto data = self->dataFor(target); data && data->column) {
                g_draggedTarget = target;
                g_draggedWidth  = data->column->getColumnWidth();
            }
        }
        reinterpret_cast<RemoveTargetFn>(g_hookRemoveTarget->m_original)(self, target);
    }

    // 3) Preserve column width on DnD and workspace moves across all scrolling workspaces,
    //    plus reversed-aware DnD drop + rightmost spawn for SCROLL_DIR_LEFT:
    //    - Under SCROLL_DIR_LEFT, index 0 is rightmost and higher indices are further left.
    //    - DnD outer 30% left edge -> insert at idx + 1 (add(idx)), visually left.
    //    - DnD outer 30% right edge -> insert at idx (add(idx - 1)), visually right.
    //    - New non-DnD windows always spawn at index 0 (rightmost).
    void hkNewTarget(CScrollingAlgorithm* self, SP<ITarget> target) {
        const bool isDnd = g_layoutManager->dragController()->wasDraggingWindow() &&
                           g_layoutManager->dragController()->draggingTiled();

        std::optional<float> savedWidth;
        if (g_draggedTarget.lock() == target) {
            savedWidth = g_draggedWidth;
            g_draggedTarget.reset();
            g_draggedWidth.reset();
        }
        if (!self || !self->m_scrollingData || !self->m_scrollingData->controller ||
            self->m_scrollingData->controller->getDirection() != SCROLL_DIR_LEFT) {
            reinterpret_cast<NewTargetFn>(g_hookNewTarget->m_original)(self, target);
            if (savedWidth.has_value() && self && self->m_scrollingData) {
                if (const auto data = self->dataFor(target); data && data->column && data->column->targetDatas.size() == 1) {
                    data->column->setColumnWidth(*savedWidth);
                    self->m_scrollingData->fitCol(data->column.lock());
                    self->m_scrollingData->recalculate();
                }
            }
            return;
        }

        auto droppingOn = Desktop::focusState()->window();
        if (droppingOn && droppingOn->layoutTarget() == target)
            droppingOn = Desktop::viewState()->hitTest().windowAt(
                g_pInputManager->getMouseCoordsInternal(),
                Desktop::View::RESERVED_EXTENTS | Desktop::View::INPUT_EXTENTS);

        SP<SScrollingTargetData> droppingData   = droppingOn ? self->dataFor(droppingOn->layoutTarget()) : nullptr;
        SP<SColumnData>          droppingColumn = droppingData ? droppingData->column.lock() : nullptr;
        const auto               ruleWidth =
            (target && target->window()) ? target->window()->m_ruleApplicator->static_.scrollingWidth : std::nullopt;
        const auto               dndWidth = savedWidth.has_value() ? savedWidth : ruleWidth;

        if (isDnd) {
            if (!droppingColumn) {
                // Dropped onto empty space (to the left of the right-aligned tape):
                // append at the end (leftmost column).
                auto col = self->m_scrollingData->add(dndWidth);
                col->add(target);
                self->m_scrollingData->fitCol(col);
            } else {
                const auto  mousePos    = g_pInputManager->getMouseCoordsInternal();
                const auto  droppingBox = *droppingOn->logicalBox();
                const float threshold   = 0.3F;

                const bool withinLeft  = mousePos.x <= droppingBox.x + (droppingBox.w * threshold);
                const bool withinRight = mousePos.x >= droppingBox.x + (droppingBox.w * (1.0F - threshold));

                if (!withinLeft && !withinRight) {
                    const auto idx = droppingColumn->idx(droppingOn->layoutTarget());
                    const bool top = droppingOn->getWindowIdealBoundingBoxIgnoreReserved().middle().y > mousePos.y;
                    droppingColumn->add(target, top ? (idx == 0 ? -1 : static_cast<int>(idx) - 1) : static_cast<int>(idx));
                } else {
                    const auto colIdx = static_cast<int>(self->m_scrollingData->idx(droppingColumn));
                    // Reversed axis: visual left = higher index (after colIdx),
                    // visual right = lower index (after colIdx - 1).
                    if (withinLeft)
                        droppingColumn = self->m_scrollingData->add(colIdx, dndWidth);
                    else
                        droppingColumn = self->m_scrollingData->add(colIdx - 1, dndWidth);

                    droppingColumn->add(target);
                }

                self->m_scrollingData->fitCol(droppingColumn);
            }
        } else {
            // New non-DnD window (or workspace move): spawn as rightmost column (index 0).
            auto col = self->m_scrollingData->add(-1, dndWidth);
            col->add(target);
            self->m_scrollingData->fitCol(col);
        }

        self->m_scrollingData->recalculate();
    }
    // 3) Reversed-aware mouse resize for SCROLL_DIR_LEFT:
    //    Visual X for column i is [usablePrimary - currentEnd + offset, usablePrimary - currentStart + offset].
    //    - Dragging left edge (edgeLeft): grow/shrink width alone; visual right edge stays stationary.
    //    - Dragging right edge (edgeRight): grow/shrink width + adjustOffset(+delta * usablePrimary)
    //      so visual left edge stays stationary and visual right edge tracks the cursor.
    void hkResizeTarget(CScrollingAlgorithm* self, const Vector2D& delta, SP<ITarget> target, eRectCorner corner) {
        if (!self || !self->m_scrollingData || !self->m_scrollingData->controller ||
            self->m_scrollingData->controller->getDirection() != SCROLL_DIR_LEFT) {
            reinterpret_cast<ResizeTargetFn>(g_hookResizeTarget->m_original)(self, delta, target, corner);
            return;
        }

        if (!target || !Desktop::View::validMapped(target->window()))
            return;

        const auto data = self->dataFor(target);
        if (!data || !data->column || !data->column->scrollingData) {
            reinterpret_cast<ResizeTargetFn>(g_hookResizeTarget->m_original)(self, delta, target, corner);
            return;
        }

        static const auto pfsonone = CConfigValue<Config::INTEGER>("scrolling:fullscreen_on_one_column");

        const auto        usable      = self->usableArea();
        const auto        deltaAsPerc = delta / usable.size();
        Vector2D          modDelta    = delta;

        const auto        currColumn  = data->column.lock();
        const int64_t     colIdx      = self->m_scrollingData->idx(currColumn);
        if (colIdx < 0)
            return;

        auto&             ctrl          = self->m_scrollingData->controller;
        const double      currentStart  = ctrl->calculateStripStart(colIdx, usable, *pfsonone);
        const double      currentSize   = ctrl->calculateStripSize(colIdx, usable, *pfsonone);
        const double      currentEnd    = currentStart + currentSize;
        const double      cameraOffset  = ctrl->getOffset();
        const double      usablePrimary = usable.w;

        const double      visLeft       = usablePrimary - currentEnd + cameraOffset;
        const double      visRight      = usablePrimary - currentStart + cameraOffset;

        ctrl->setOffset(cameraOffset);

        if (edgeLeft(corner)) {
            // Dragging the left half: grow leftward until the left edge hits 0,
            // then spill remaining growth rightward (pushing columns to the right).
            const float oldWidth       = currColumn->getColumnWidth();
            const float requestedDelta = -static_cast<float>(deltaAsPerc.x);
            const float newWidth       = std::clamp(oldWidth + requestedDelta, MIN_COLUMN_WIDTH, MAX_COLUMN_WIDTH);
            const float actualDelta    = newWidth - oldWidth;

            if (actualDelta > 0.0F) {
                const double reqPx     = actualDelta * usablePrimary;
                const double growLeft  = std::min(reqPx, std::max(0.0, visLeft));
                const double growRight = std::min(reqPx - growLeft, std::max(0.0, usablePrimary - visRight));
                const float  totalGrow = static_cast<float>((growLeft + growRight) / usablePrimary);
                if (totalGrow > 0.0F) {
                    currColumn->setColumnWidth(oldWidth + totalGrow);
                    if (growRight > 0.0)
                        ctrl->adjustOffset(growRight);
                }
            } else if (actualDelta < 0.0F) {
                currColumn->setColumnWidth(oldWidth + actualDelta);
                if (visLeft <= 1.0 && cameraOffset > 0.0)
                    ctrl->adjustOffset(std::max(actualDelta * usablePrimary, -cameraOffset));
            }
        } else if (colIdx == 0 && visRight >= usablePrimary - 1.0) {
            // Rightmost column flush with the right edge: right half also grows/shrinks
            // leftward while keeping the right edge pinned (drag right = grow, left = shrink).
            const float oldWidth       = currColumn->getColumnWidth();
            const float requestedDelta = static_cast<float>(deltaAsPerc.x);
            const float newWidth       = std::clamp(oldWidth + requestedDelta, MIN_COLUMN_WIDTH, MAX_COLUMN_WIDTH);
            float       actualDelta    = newWidth - oldWidth;

            if (actualDelta > 0.0F && actualDelta * usablePrimary > std::max(0.0, visLeft))
                actualDelta = static_cast<float>(std::max(0.0, visLeft) / usablePrimary);

            if (actualDelta != 0.0F)
                currColumn->setColumnWidth(oldWidth + actualDelta);
        } else {
            const float oldWidth       = currColumn->getColumnWidth();
            const float requestedDelta = static_cast<float>(deltaAsPerc.x);
            const float newWidth       = std::clamp(oldWidth + requestedDelta, MIN_COLUMN_WIDTH, MAX_COLUMN_WIDTH);
            float       actualDelta    = newWidth - oldWidth;

            const double roomRight = std::max(0.0, usablePrimary - visRight);
            if (actualDelta > 0.0F && actualDelta * usablePrimary > roomRight)
                actualDelta = static_cast<float>(roomRight / usablePrimary);

            if (actualDelta != 0.0F) {
                currColumn->setColumnWidth(oldWidth + actualDelta);
                ctrl->adjustOffset(std::max(actualDelta * usablePrimary, -cameraOffset));
            }
        }

        if (data->column->targetDatas.size() > 1) {
            const auto& currTd = data;
            const auto  nextTd = data->column->next(data);
            const auto  prevTd = data->column->prev(data);
            if (corner == CORNER_NONE) {
                if (!prevTd)
                    corner = CORNER_BOTTOMRIGHT;
                else {
                    corner = CORNER_TOPRIGHT;
                    modDelta.y *= -1.0;
                }
            }

            if (edgeBottom(corner) && nextTd) {
                const float nextSize = currColumn->getTargetSize(nextTd);
                const float currSize = currColumn->getTargetSize(currTd);
                if (!(nextSize <= MIN_ROW_HEIGHT && modDelta.y >= 0)) {
                    const float adjust = std::clamp(static_cast<float>(modDelta.y / usable.h),
                                                    -currSize + MIN_ROW_HEIGHT,
                                                    nextSize - MIN_ROW_HEIGHT);
                    currColumn->setTargetSize(nextTd, std::clamp(nextSize - adjust, MIN_ROW_HEIGHT, MAX_ROW_HEIGHT));
                    currColumn->setTargetSize(currTd, std::clamp(currSize + adjust, MIN_ROW_HEIGHT, MAX_ROW_HEIGHT));
                }
            } else if (edgeTop(corner) && prevTd) {
                const float prevSize = currColumn->getTargetSize(prevTd);
                const float currSize = currColumn->getTargetSize(currTd);
                if (!((prevSize <= MIN_ROW_HEIGHT && modDelta.y <= 0) || (currSize <= MIN_ROW_HEIGHT && delta.y >= 0))) {
                    const float adjust = std::clamp(static_cast<float>(modDelta.y / usable.h),
                                                    -(prevSize - MIN_ROW_HEIGHT),
                                                    currSize - MIN_ROW_HEIGHT);
                    currColumn->setTargetSize(prevTd, std::clamp(prevSize + adjust, MIN_ROW_HEIGHT, MAX_ROW_HEIGHT));
                    currColumn->setTargetSize(currTd, std::clamp(currSize - adjust, MIN_ROW_HEIGHT, MAX_ROW_HEIGHT));
                }
            }
        }

        self->m_scrollingData->recalculate(true);
    }

    CFunctionHook* installHook(const std::string& searchName, const std::string& demangledSubstr, void* detour) {
        const auto matches = HyprlandAPI::findFunctionsByName(g_handle, searchName);
        for (const auto& m : matches) {
            if (m.demangled.find(demangledSubstr) != std::string::npos) {
                auto* hook = HyprlandAPI::createFunctionHook(g_handle, m.address, detour);
                if (hook && hook->hook())
                    return hook;
                if (hook)
                    HyprlandAPI::removeFunctionHook(g_handle, hook);
                throw std::runtime_error("failed to hook " + demangledSubstr);
            }
        }
        throw std::runtime_error("symbol not found: " + demangledSubstr);
    }

    void removeAllHooks() {
        auto removeOne = [](CFunctionHook*& hook) {
            if (hook) {
                HyprlandAPI::removeFunctionHook(g_handle, hook);
                hook = nullptr;
            }
        };
        removeOne(g_hookResizeTarget);
        removeOne(g_hookRemoveTarget);
        removeOne(g_hookNewTarget);
        removeOne(g_hookCameraOffset);
        g_draggedTarget.reset();
        g_draggedWidth.reset();
    }

} // namespace

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    g_handle = handle;

    const std::string hash       = __hyprland_api_get_hash();
    const std::string clientHash = __hyprland_api_get_client_hash();
    if (hash != clientHash) {
        HyprlandAPI::addNotification(
            g_handle,
            "[hypr-scrolling-rtl] Version mismatch: rebuild against current Hyprland headers",
            CHyprColor{1.0, 0.2, 0.2, 1.0},
            5000);
        throw std::runtime_error("[hypr-scrolling-rtl] Version mismatch");
    }

    try {
        g_hookCameraOffset = installHook(
            "calculateCameraOffset",
            "CScrollTapeController::calculateCameraOffset(",
            reinterpret_cast<void*>(&hkCalculateCameraOffset));
        g_hookNewTarget = installHook(
            "newTarget",
            "CScrollingAlgorithm::newTarget(",
            reinterpret_cast<void*>(&hkNewTarget));
        g_hookRemoveTarget = installHook(
            "removeTarget",
            "CScrollingAlgorithm::removeTarget(",
            reinterpret_cast<void*>(&hkRemoveTarget));
        g_hookResizeTarget = installHook(
            "resizeTarget",
            "CScrollingAlgorithm::resizeTarget(",
            reinterpret_cast<void*>(&hkResizeTarget));
    } catch (const std::exception& e) {
        removeAllHooks();
        HyprlandAPI::addNotification(
            g_handle,
            std::string{"[hypr-scrolling-rtl] Init failed: "} + e.what(),
            CHyprColor{1.0, 0.2, 0.2, 1.0},
            5000);
        throw;
    }

    return {"hypr-scrolling-rtl", "Right-aligned scrolling and reversed-direction resize/DnD fixes", "ZiadAbdelati", "0.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    removeAllHooks();
}
