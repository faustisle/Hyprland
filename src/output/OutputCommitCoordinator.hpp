#pragma once

#include "DamageRing.hpp"
#include "../SharedDefs.hpp"
#include "../desktop/DesktopTypes.hpp"

#include <aquamarine/output/Output.hpp>
#include <optional>
#include <chrono>

namespace Monitor {
    class CMonitor;

    class COutputCommitCoordinator {
      public:
        enum eFrameKind : uint8_t {
            FRAME_COMPOSED = 0,
            FRAME_DIRECT_SCANOUT,
        };

        enum eSubmitResult : uint8_t {
            SUBMIT_FAILED = 0,
            SUBMIT_SYNCHRONOUS,
            SUBMIT_ASYNC,
            // Rendered, but the backend already has a commit in flight. The frame
            // is held and handed to the backend as soon as that flip completes.
            SUBMIT_DEFERRED,
        };

        struct SFrame {
            eFrameKind                               kind = FRAME_COMPOSED;
            std::optional<CDamageRing::CTransaction> damage;
            CRegion                                  renderedDamage;
            PHLWINDOWREF                             scanoutCandidate;
            bool                                     rollbackSwapchain = false;
            bool                                     tearing           = false;
            bool                                     vrr               = false;
            bool                                     copyFBPrepared    = false;
            std::optional<uint32_t>                  previousFormat;
            uint64_t                                 id = 0;
            // Latency diagnostics. submitTime is taken when the frame is handed
            // to the backend, renderUs is how long producing it took.
            std::chrono::steady_clock::time_point submitTime{};
            uint64_t                              renderUs = 0;
        };

        struct SStagedRender {
            CRegion damage;
            bool    copyFBPrepared = false;
        };

        explicit COutputCommitCoordinator(CMonitor* monitor);
        ~COutputCommitCoordinator();

        bool                         asyncEnabled() const;
        bool                         canBeginFrame() const;
        bool                         hasPendingCommit() const;
        bool                         ownsCommit(uint64_t id) const;
        bool                         shouldForwardCommitEvent() const;
        bool                         deferStateCommit();
        void                         stageRenderedDamage(const CRegion& damage, bool copyFBPrepared);
        std::optional<SStagedRender> takeStagedRender();
        void                         setLastRenderDuration(uint64_t us);
        eSubmitResult                submit(SFrame&& frame);
        void                         onCommitResult(const Aquamarine::IOutput::SCommitResult& result);
        void                         onPresented(uint64_t id, bool presented);
        void                         cancelPending();

      private:
        bool          canSubmitAsync(const SFrame& frame) const;
        eSubmitResult submitSynchronously(SFrame&& frame);
        eSubmitResult submitFrame(SFrame&& frame);
        eSubmitResult deferFrame(SFrame&& frame);
        void          submitted(SFrame& frame, bool async);
        void          failed(SFrame&& frame, bool rollbackSwapchain);
        void          scheduleRecovery();
        void          flushDeferredStateCommit();
        void          submitQueued();
        void          discardQueued();
        void          noteSubmitStats(eSubmitResult result, bool tearing, uint64_t renderUs);
        void          logStatsIfDue();
        bool          statsEnabled();

        // Per-monitor frame statistics, printed once a second when
        // HYPRLAND_TEARING_STATS is set or forced tearing is active. Used to
        // tell "we are slow" apart from "we are being paced to vblank" without
        // a profiler.
        struct SStats {
            uint64_t async = 0, sync = 0, deferred = 0, failed = 0;
            uint64_t renderUsSum = 0, renderUsMax = 0, submitted = 0;
            uint64_t presentUsSum = 0, presentUsMax = 0, presented = 0;
            // Time spent between the previous present and the next submit: the
            // part of the frame interval the compositor did not account for.
            uint64_t idleUsSum = 0, idleUsMax = 0, idle = 0;
            // Backend-side counter of commits that wanted an async flip and had
            // to give it up, sampled once per window.
            uint64_t                              lastAsyncDrops = 0;
            std::chrono::steady_clock::time_point windowStart;
        };

        CMonitor*                             m_monitor = nullptr;
        std::optional<SFrame>                 m_pending;
        std::optional<SFrame>                 m_queued;
        std::optional<SStagedRender>          m_stagedRender;
        bool                                  m_stateCommitPending = false;
        uint64_t                              m_lastRenderUs       = 0;
        SStats                                m_stats;
        std::chrono::steady_clock::time_point m_lastPresentTime;
        bool                                  m_havePresentTime = false;
    };
}
