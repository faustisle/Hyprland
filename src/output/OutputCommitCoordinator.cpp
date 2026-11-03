#include "OutputCommitCoordinator.hpp"

#include "Monitor.hpp"
#include "../config/ConfigValue.hpp"
#include "../managers/eventLoop/EventLoopManager.hpp"
#include "../managers/screenshare/ScreenshareManager.hpp"
#include "../protocols/PresentationTime.hpp"
#include "../render/Renderer.hpp"
#include "../desktop/view/window/Window.hpp"
#include "../debug/log/Logger.hpp"

#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <utility>

using namespace Monitor;

// Latency diagnostics: prints one line per second with render cost, submit mix,
// flip latency and idle time for every output. Always on for forced tearing
// (where the numbers matter), opt-in otherwise.
bool COutputCommitCoordinator::statsEnabled() {
    static const bool ENABLED = getenv("HYPRLAND_TEARING_STATS") != nullptr;
    return ENABLED || (m_monitor && m_monitor->forceTearing());
}

static uint64_t usSince(const std::chrono::steady_clock::time_point& from, const std::chrono::steady_clock::time_point& to) {
    return std::chrono::duration_cast<std::chrono::microseconds>(to - from).count();
}

COutputCommitCoordinator::COutputCommitCoordinator(CMonitor* monitor) : m_monitor(monitor) {
    if (m_monitor && m_monitor->m_output)
        m_monitor->m_output->setPipelinedFrames(false);
}

COutputCommitCoordinator::~COutputCommitCoordinator() {
    cancelPending();
}

bool COutputCommitCoordinator::asyncEnabled() const {
    static auto PASYNCCOMMIT = CConfigValue<Config::INTEGER>("render:async_commit");

    // Forced tearing is only useful when commits are not paced to vblank, so
    // it implies the asynchronous commit path.
    const bool FORCETEARING = m_monitor && m_monitor->forceTearingSupported();

    return (*PASYNCCOMMIT || FORCETEARING) && m_monitor && m_monitor->m_output &&
        (m_monitor->m_output->commitCapabilities() & Aquamarine::IOutput::AQ_OUTPUT_COMMIT_CAPABILITY_QUEUED);
}

bool COutputCommitCoordinator::canBeginFrame() const {
    // Render only when KMS has room for another commit. Rendering ahead of the
    // in-flight flip does not lower latency (the frame cannot be handed to the
    // backend until the flip completes) and the backend refuses commits while a
    // flip is armed, so it only burns GPU time and buffers.
    if (m_monitor && m_monitor->m_output && m_monitor->m_output->pendingPageFlip())
        return false;

    return !m_pending.has_value();
}

bool COutputCommitCoordinator::hasPendingCommit() const {
    return m_pending.has_value() || (m_monitor && m_monitor->m_output && m_monitor->m_output->pendingPageFlip());
}

bool COutputCommitCoordinator::ownsCommit(uint64_t id) const {
    return id && m_pending && m_pending->id == id;
}

bool COutputCommitCoordinator::shouldForwardCommitEvent() const {
    return !m_pending || !m_pending->id;
}

bool COutputCommitCoordinator::deferStateCommit() {
    // KMS can only take one commit at a time. A state commit attempted while a
    // flip is armed must wait for it, exactly like a rendered frame.
    if (!m_pending && (!m_monitor || !m_monitor->m_output || !m_monitor->m_output->pendingPageFlip()))
        return false;

    m_stateCommitPending = true;
    return true;
}

void COutputCommitCoordinator::stageRenderedDamage(const CRegion& damage, bool copyFBPrepared) {
    m_stagedRender = SStagedRender{
        .damage         = damage,
        .copyFBPrepared = copyFBPrepared,
    };
}

std::optional<COutputCommitCoordinator::SStagedRender> COutputCommitCoordinator::takeStagedRender() {
    return std::exchange(m_stagedRender, std::nullopt);
}

void COutputCommitCoordinator::setLastRenderDuration(uint64_t us) {
    m_lastRenderUs = us;
}

bool COutputCommitCoordinator::canSubmitAsync(const SFrame& frame) const {
    if (!asyncEnabled() || m_pending)
        return false;

    const auto CAPS = m_monitor->m_output->commitCapabilities();
    if (frame.vrr && !(CAPS & Aquamarine::IOutput::AQ_OUTPUT_COMMIT_CAPABILITY_VRR))
        return false;
    if (frame.tearing && !(CAPS & Aquamarine::IOutput::AQ_OUTPUT_COMMIT_CAPABILITY_TEARING))
        return false;

    return true;
}

COutputCommitCoordinator::eSubmitResult COutputCommitCoordinator::submit(SFrame&& frame) {
    // Deferred frames are re-submitted here, so only take the render duration
    // once; refresh the submit time, that is what the flip latency is measured
    // from.
    if (m_lastRenderUs) {
        frame.renderUs = std::exchange(m_lastRenderUs, 0);
    }
    frame.submitTime = std::chrono::steady_clock::now();

    const bool     TEARING   = frame.tearing;
    const uint64_t RENDER_US = frame.renderUs;

    if (m_havePresentTime) {
        const uint64_t IDLE_US = usSince(m_lastPresentTime, frame.submitTime);
        m_stats.idleUsSum += IDLE_US;
        m_stats.idleUsMax = std::max(m_stats.idleUsMax, IDLE_US);
        ++m_stats.idle;
    }

    const auto RESULT = submitFrame(std::move(frame));

    if (statsEnabled()) {
        noteSubmitStats(RESULT, TEARING, RENDER_US);
        logStatsIfDue();
    }

    return RESULT;
}

COutputCommitCoordinator::eSubmitResult COutputCommitCoordinator::submitFrame(SFrame&& frame) {
    if (!m_monitor || !m_monitor->m_output) {
        if (m_monitor)
            PROTO::presentation->discardUntagged(m_monitor->m_self.lock());
        return SUBMIT_FAILED;
    }

    const auto CAPS = m_monitor->m_output->commitCapabilities();

    LOG(Log::TRACE, "COutputCommitCoordinator: submit() with caps={}", m_monitor->m_name, sc<uint32_t>(CAPS));

    // KMS refuses a second commit while a flip is armed. That is a deferral,
    // not a failure: treating it as one (damageEntire + immediate retry) turns
    // every frame into a full-screen recomposite storm until the flip lands.
    if (m_pending || m_monitor->m_output->pendingPageFlip()) {
        // A direct scanout locks the client buffer right after submit, which
        // cannot be handed over from a later callback, so let it fail and
        // compose instead.
        if (frame.kind == FRAME_DIRECT_SCANOUT) {
            LOG(Log::TRACE, "COutputCommitCoordinator: refusing direct scanout for {} while a flip is in flight", m_monitor->m_name);
            PROTO::presentation->discardUntagged(m_monitor->m_self.lock());
            return SUBMIT_FAILED;
        }

        return deferFrame(std::move(frame));
    }

    // Not busy anymore: a frame held from before is stale, the caller already
    // has a newer render to hand over.
    discardQueued();

    if (!canSubmitAsync(frame))
        return submitSynchronously(std::move(frame));

    LOG(Log::TRACE, "COutputCommitCoordinator: submitting asynchronously for {}", m_monitor->m_name);

    Aquamarine::IOutput::SCommitOptions options;

    if (!frame.tearing && !frame.vrr && (CAPS & Aquamarine::IOutput::AQ_OUTPUT_COMMIT_CAPABILITY_TIMED))
        options.targetPresentation = m_monitor->m_output->nextVBlank();
    options.lateCursor = m_monitor->m_output->hasCursorPlane() && (CAPS & Aquamarine::IOutput::AQ_OUTPUT_COMMIT_CAPABILITY_LATE_CURSOR);
    // Torn frames are not paced to vblank, so there is nothing for the commit
    // thread to time: hand the commit to KMS from this thread and skip the hop.
    options.immediate = frame.tearing;

    const auto SUBMISSION = m_monitor->m_output->commitAsync(options);
    if (!SUBMISSION.id) {
        if (SUBMISSION.error == ENOTSUP)
            return submitSynchronously(std::move(frame));
        if (SUBMISSION.error == EBUSY)
            return deferFrame(std::move(frame));

        LOG(Log::TRACE, "Async output commit for {} rejected: {}", m_monitor->m_name, strerror(SUBMISSION.error));
        PROTO::presentation->discardUntagged(m_monitor->m_self.lock());
        failed(std::move(frame), true);
        return SUBMIT_FAILED;
    }

    frame.id = SUBMISSION.id;
    if (frame.damage) {
        frame.damage->commit();
        frame.damage.reset();
    }
    m_pending = std::move(frame);
    PROTO::presentation->tagQueued(m_monitor->m_self.lock(), SUBMISSION.id, m_pending->tearing, m_pending->vrr);

    return SUBMIT_ASYNC;
}

COutputCommitCoordinator::eSubmitResult COutputCommitCoordinator::deferFrame(SFrame&& frame) {
    LOG(Log::TRACE, "COutputCommitCoordinator: holding rendered frame for {} until the pending flip completes", m_monitor->m_name);
    discardQueued();

    // Retire the damage transaction now. The frame is already rendered, and a
    // later frame cannot be queued before this one is submitted, so the ring
    // keeps the right order and the next frame's buffer-age damage stays valid.
    if (frame.damage) {
        frame.damage->commit();
        frame.damage.reset();
    }

    m_queued = std::move(frame);
    return SUBMIT_DEFERRED;
}

COutputCommitCoordinator::eSubmitResult COutputCommitCoordinator::submitSynchronously(SFrame&& frame) {
    // A synchronous commit cannot be queued in KMS either: it would bounce off
    // the "page-flip is awaiting" check inside the backend. Hold the frame
    // until the current flip completes instead of failing and re-damaging the
    // whole output.
    if (m_monitor->m_output->pendingPageFlip())
        return deferFrame(std::move(frame));

    LOG(Log::TRACE, "COutputCommitCoordinator: submitting synchronously for {}", m_monitor->m_name);

    PROTO::presentation->tagQueued(m_monitor->m_self.lock(), 0, frame.tearing, frame.vrr);

    bool ok = frame.kind == FRAME_DIRECT_SCANOUT ? m_monitor->m_output->commit() : m_monitor->m_state.commit();
    if (!ok && frame.kind == FRAME_COMPOSED && m_monitor->m_inFence.isValid()) {
        m_monitor->m_output->state->resetExplicitFences();
        ok = m_monitor->m_state.commit();
    }

    if (!ok) {
        PROTO::presentation->discardQueued(m_monitor->m_self.lock(), 0);
        failed(std::move(frame), true);
        return SUBMIT_FAILED;
    }

    submitted(frame, false);
    return SUBMIT_SYNCHRONOUS;
}

void COutputCommitCoordinator::onCommitResult(const Aquamarine::IOutput::SCommitResult& result) {
    if (!ownsCommit(result.id))
        return;

    if (result.status != Aquamarine::IOutput::AQ_OUTPUT_COMMIT_SUBMITTED) {
        auto frame = std::move(*m_pending);
        m_pending.reset();
        PROTO::presentation->discardQueued(m_monitor->m_self.lock(), result.id);
        LOG(Log::ERR, "Async output commit {} for {} failed: {}", result.id, m_monitor->m_name,
            result.status == Aquamarine::IOutput::AQ_OUTPUT_COMMIT_CANCELLED ? "cancelled" : strerror(result.error));
        failed(std::move(frame), false);
        flushDeferredStateCommit();
        return;
    }

    if (result.missedTarget)
        LOG(Log::TRACE, "Async output commit {} for {} missed its target", result.id, m_monitor->m_name);
    else
        LOG(Log::TRACE, "Async output commit {} for {} submitted successfully", result.id, m_monitor->m_name);

    submitted(*m_pending, true);
    m_monitor->m_events.commit.emit();
}

void COutputCommitCoordinator::submitted(SFrame& frame, bool async) {
    LOG(Log::TRACE, "COutputCommitCoordinator: submitted {}, async={}", m_monitor->m_name, async);

    if (frame.damage)
        frame.damage->commit();

    if (frame.kind == FRAME_COMPOSED) {
        if (!frame.copyFBPrepared)
            m_monitor->resources()->markMirrorFBStale(frame.renderedDamage);
        if (!m_monitor->m_mirrors.empty())
            g_pHyprRenderer->damageMirrorsWith(m_monitor->m_self.lock(), frame.renderedDamage);
    } else if (!frame.copyFBPrepared)
        m_monitor->resources()->markMirrorFBStale();

    if (frame.kind == FRAME_DIRECT_SCANOUT) {
        const auto CANDIDATE = frame.scanoutCandidate.lock();
        if (CANDIDATE) {
            if (m_monitor->m_lastScanout.expired())
                LOG(Log::DEBUG, "Entered a direct scanout to {:x}: \"{}\"", rc<uintptr_t>(CANDIDATE.get()), CANDIDATE->metadata().title());
            m_monitor->m_lastScanout           = CANDIDATE;
            m_monitor->m_directScanoutIsActive = true;
        }
    }

    if (!async && frame.kind == FRAME_COMPOSED)
        m_monitor->m_directScanoutIsActive = false;

    if (!async && frame.kind == FRAME_COMPOSED && Screenshare::mgr())
        Screenshare::mgr()->onOutputCommit(m_monitor->m_self.lock(), frame.copyFBPrepared);
}

void COutputCommitCoordinator::noteSubmitStats(eSubmitResult result, bool tearing, uint64_t renderUs) {
    if (renderUs) {
        m_stats.renderUsSum += renderUs;
        m_stats.renderUsMax = std::max(m_stats.renderUsMax, renderUs);
        ++m_stats.submitted;
    }

    switch (result) {
        case SUBMIT_ASYNC: ++m_stats.async; break;
        case SUBMIT_SYNCHRONOUS: ++m_stats.sync; break;
        case SUBMIT_DEFERRED: ++m_stats.deferred; break;
        default: ++m_stats.failed; break;
    }
}

void COutputCommitCoordinator::logStatsIfDue() {
    if (!statsEnabled() || !m_monitor)
        return;

    const auto NOW = std::chrono::steady_clock::now();
    if (m_stats.windowStart.time_since_epoch().count() == 0) {
        m_stats.windowStart = NOW;
        return;
    }

    const auto ELAPSED = usSince(m_stats.windowStart, NOW);
    if (ELAPSED < 1000000)
        return;

    const uint64_t TOTAL = m_stats.async + m_stats.sync + m_stats.deferred + m_stats.failed;
    const double   SECS  = ELAPSED / 1000000.0;

    const uint64_t DROPS   = m_monitor->m_output ? m_monitor->m_output->asyncFlipDrops() : 0;
    const uint64_t DROPPED = DROPS - m_stats.lastAsyncDrops;

    LOG(Log::INFO,
        "tearing stats {}: {:.1f} fps | async {} sync {} deferred {} failed {} | dropped {} | render avg {}us max {}us | flip avg {}us max {}us | idle avg {}us max {}us | tearing "
        "{} pmode {}",
        m_monitor->m_name, TOTAL / SECS, m_stats.async, m_stats.sync, m_stats.deferred, m_stats.failed, DROPPED, m_stats.submitted ? m_stats.renderUsSum / m_stats.submitted : 0,
        m_stats.renderUsMax, m_stats.presented ? m_stats.presentUsSum / m_stats.presented : 0, m_stats.presentUsMax, m_stats.idle ? m_stats.idleUsSum / m_stats.idle : 0,
        m_stats.idleUsMax, m_monitor->m_tearingState.activelyTearing, m_monitor->m_output ? sc<uint32_t>(m_monitor->m_output->state->state().presentationMode) : 0);

    m_stats                = SStats{};
    m_stats.lastAsyncDrops = DROPS;
    m_stats.windowStart    = NOW;
}

void COutputCommitCoordinator::onPresented(uint64_t id, bool presented) {
    bool presentedNow = false;

    if (id) {
        if (!ownsCommit(id))
            return;

        if (presented && m_pending->kind == FRAME_COMPOSED && Screenshare::mgr())
            Screenshare::mgr()->onOutputCommit(m_monitor->m_self.lock(), m_pending->copyFBPrepared);

        if (presented && statsEnabled()) {
            const auto US = usSince(m_pending->submitTime, std::chrono::steady_clock::now());
            m_stats.presentUsSum += US;
            m_stats.presentUsMax = std::max(m_stats.presentUsMax, US);
            ++m_stats.presented;
            presentedNow = true;
        }

        if (!presented) {
            auto frame = std::move(*m_pending);
            m_pending.reset();
            failed(std::move(frame), false);
            flushDeferredStateCommit();
            return;
        }

        m_pending.reset();
    } else if (!presented)
        return; // synchronous commits do not report failure through the present event
    else
        presentedNow = true; // commitID 0: a synchronous commit completed

    // A synchronous commit completed (commitID 0). It may have been holding
    // back a rendered frame or a deferred state commit.
    submitQueued();
    flushDeferredStateCommit();

    // canBeginFrame() may have latched a frame because the output was busy;
    // render it now that KMS is free again.
    if (m_monitor->m_pendingFrame)
        m_monitor->scheduleFrame(Aquamarine::IOutput::AQ_SCHEDULE_RENDER_MONITOR);

    // Measured after the deferred frame is handed over, so the idle metric is
    // "how long did the compositor wait after the last present", not "how long
    // the queue hop took".
    if (presentedNow) {
        m_lastPresentTime = std::chrono::steady_clock::now();
        m_havePresentTime = true;
    }

    logStatsIfDue();
}

void COutputCommitCoordinator::failed(SFrame&& frame, bool rollbackSwapchain) {
    LOG(Log::TRACE, "COutputCommitCoordinator: failed for {}", m_monitor->m_name);

    // Anything rendered for after the failed frame is stale.
    discardQueued();

    if (frame.damage)
        frame.damage->rollback();
    if (rollbackSwapchain && frame.rollbackSwapchain && m_monitor->m_output && m_monitor->m_output->swapchain)
        m_monitor->m_output->swapchain->rollback();

    if (m_monitor->m_output)
        m_monitor->m_output->state->resetExplicitFences();

    if (frame.kind == FRAME_DIRECT_SCANOUT) {
        if (frame.previousFormat) {
            m_monitor->m_drmFormat = *frame.previousFormat;
            m_monitor->m_output->state->setFormat(*frame.previousFormat);
        }
        m_monitor->m_output->state->setBuffer(nullptr);
        m_monitor->m_lastScanout.reset();
        m_monitor->m_previousFSWindow.reset();
    }

    m_monitor->m_tearingState.busy = false;
    m_monitor->m_damage.damageEntire();
    scheduleRecovery();
}

void COutputCommitCoordinator::scheduleRecovery() {
    g_pEventLoopManager->doLater([monitor = m_monitor->m_self] {
        if (monitor)
            monitor->scheduleFrame(Aquamarine::IOutput::AQ_SCHEDULE_RENDER_MONITOR);
    });
}

void COutputCommitCoordinator::flushDeferredStateCommit() {
    if (!m_stateCommitPending || m_pending || (m_monitor->m_output && m_monitor->m_output->pendingPageFlip()))
        return;

    m_stateCommitPending = false;
    if (!m_monitor->m_state.commit()) {
        m_monitor->m_damage.damageEntire();
        scheduleRecovery();
    }
}

void COutputCommitCoordinator::cancelPending() {
    m_stagedRender.reset();
    m_stateCommitPending = false;

    discardQueued();

    if (!m_pending)
        return;

    PROTO::presentation->discardQueued(m_monitor->m_self.lock(), m_pending->id);
    auto frame = std::move(*m_pending);
    m_pending.reset();
    if (frame.damage)
        frame.damage->rollback();
}

void COutputCommitCoordinator::submitQueued() {
    if (!m_queued)
        return;

    auto frame = std::move(*m_queued);
    m_queued.reset();

    // The flip that blocked this frame is done, so it can go to the backend now.
    submit(std::move(frame));
}

void COutputCommitCoordinator::discardQueued() {
    if (!m_queued)
        return;

    if (m_queued->damage)
        m_queued->damage->rollback();
    m_queued.reset();
}
