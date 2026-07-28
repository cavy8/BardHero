#include "PCH.h"
#include "game/BandStage.h"

#include "game/BandFormationLogic.h"
#include "game/BandLifecycleLogic.h"
#include "game/BandPerformanceLogic.h"
#include "game/SgtVm.h"

#include "RE/B/BGSArtObject.h"
#include "RE/B/BGSSoundDescriptorForm.h"
#include "RE/B/BSAudioManager.h"
#include "RE/T/TESDataHandler.h"
#include "RE/T/TESEffectShader.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <string>
#include <string_view>
#include <thread>

namespace bard::BandStage {
    namespace {
        constexpr auto kAddon = "Bard Hero - Doom Lute.esp";
        constexpr RE::FormID kBassLocal = 0x805;
        constexpr RE::FormID kGuitarWeaponLocal = 0x80A;
        constexpr auto kSkyrim = "Skyrim.esm";
        constexpr RE::FormID kDrumForm = 0x000DABA9;
        // ---- THE BAND'S ARRIVAL EFFECTS ----------------------------------
        //
        // The conjuration sample lives inside SummonTargetFX's NIF - no
        // form field, no volume lever - so the COUNT of art objects is the
        // count of sounds, and band::BearsArrivalArt decides who carries
        // it. Rejected on 2026-07-27, do not retry: a one-per-pulse cascade
        // (the band arrives together), ducking AudioCategorySFX (too
        // blunt), borrowing the shader's ambientSound (0x0EA8FF sets none;
        // measured).
        constexpr RE::FormID kMaterialiseShaderLocal = 0x0EA8FF;
        constexpr RE::FormID kSummonTargetArtLocal = 0x03F811;
        constexpr RE::FormID kGhostShaderForm = 0x000D2057;
        constexpr RE::FormID kLuteAnimObjectForm = 0x00093643;
        constexpr RE::FormID kDrinkPotionAnimObjectForm = 0x000D36C8;
        constexpr auto kBassAnimModel =
            "BardHeroElectric\\BassAnimObject.nif";
        constexpr auto kGuitarAnimModel =
            "BardHeroElectric\\RhythmGuitarAnimObject.nif";
        constexpr auto kMicrophoneAnimModel =
            "BardHeroElectric\\MicrophoneAnimObject.nif";
        constexpr auto kVanillaDrinkPotionAnimModel =
            "Meshes\\AnimObjects\\AnimObjectDrinkPotion.nif";
        constexpr auto kDismissDelay = std::chrono::milliseconds(
            static_cast<int>(band::kDismissDeleteDelaySeconds * 1000.0f));

        band::Lifecycle g_lifecycle;
        std::array<RE::ActorHandle, band::kFormation.size()> g_performers;
        std::array<RE::NiPoint3, band::kFormation.size()> g_stagePositions;
        std::array<bool, band::kFormation.size()> g_performanceStarted{};
        std::array<bool, band::kFormation.size()> g_arrivalRevealed{};
        std::array<bool, band::kFormation.size()> g_ghostApplied{};
        // Executed pulses each performer has spent below the stage with
        // loaded 3D. Feeds band::ArrivalDue - the arrival's one timing
        // knob.
        std::array<std::uint32_t, band::kFormation.size()>
            g_arrivalWarmupPulses{};
        std::array<bool, band::kFormation.size()> g_headTrackingDisabled{};
        band::StemAvailability g_stemAvailability;
        RE::ObjectRefHandle g_drumProp;
        float g_stageHeading = 0.0f;
        std::atomic_bool g_activeMirror{ false };
        std::atomic<double> g_nextPerformancePulse{ -1.0 };
        std::atomic<std::uint64_t> g_performancePulse{ 0 };
        // Game-thread-only, like the arrays above. Pulse tasks queue on the
        // session thread's cadence but the game thread drains its backlog in
        // one frame after loads/menu churn, so real spacing must be measured
        // here, where the tasks actually execute.
        bool g_anyPulseExecuted = false;
        std::chrono::steady_clock::time_point g_lastPulseExecuted{};
        std::chrono::steady_clock::time_point g_bassStartedAt{};
        // The shared AnimObjectLute record was re-pointed at a band
        // member's instrument and the session baseline is due back one
        // capture interval later. Without the re-assert the record stays
        // aimed at the LAST member's model - bass, whenever no rhythm stem
        // exists - and every later capture wears it (field 2026-07-28,
        // "sometimes we get the wrong guitar, or even a bass"). See
        // SgtVm::ReassertAnimObjectBaseline.
        bool g_lutePropDirty = false;
        std::chrono::steady_clock::time_point g_lutePropDirtyAt{};


        // ---- THE FALLBACK SOUND PATH, NOT THE PRIMARY ONE ----------------
        //
        // Fallback sound, used ONLY when SummonTargetFX cannot be resolved:
        // the shader's own ambientSound, captured before it is nulled, so
        // no form ID is ever guessed. Shader 0x0EA8FF sets none today, so
        // this stays null - kept because the capture costs nothing and a
        // verified descriptor handed to this pointer is the whole change.
        RE::BGSSoundDescriptorForm* g_materialiseSound = nullptr;
        // Guards the fallback descriptor only - the art object is not gated
        // on it.
        bool g_arrivalSoundPlayed = false;
        // How many performers received the conjuration art this arrival;
        // the log line distinguishes "went on all four" from "ran four
        // times".
        std::size_t g_arrivalArtApplied = 0;
        // 1.0 is the sample's natural level - one of these was never loud,
        // four at once were. Trim here if a single one still bites.
        constexpr float kMaterialiseVolume = 1.0f;

        void PlayMaterialiseSound(const RE::NiPoint3& a_at) {
            if (!g_materialiseSound) { return; }
            auto* audio = RE::BSAudioManager::GetSingleton();
            if (!audio) { return; }
            RE::BSSoundHandle handle{};
            if (!audio->BuildSoundDataFromDescriptor(handle,
                                                     g_materialiseSound)) {
                spdlog::warn("[band] could not build the arrival sound");
                return;
            }
            handle.SetVolume(kMaterialiseVolume);
            handle.SetPosition(a_at);
            handle.Play();
            spdlog::info("[band] arrival conjuration played once at {:.2f}",
                         kMaterialiseVolume);
        }

        // The shader the band materialises and vanishes with, with its
        // ambient sound removed once.
        //
        // TESEffectShader::EffectShaderData carries `ambientSound`
        // (BGSSoundDescriptorForm*, 0x140). Session.cpp does the same thing
        // to FireCloakFXShader for the streak fire - keep the two in step if
        // either changes. Cleared once and left cleared: a shader effect
        // starts its ambient sound when it first TICKS, not synchronously
        // inside InstantiateHitShader, so restoring after the call would hand
        // the sound straight back.
        //
        // This is a VANILLA record, so anything else using it this session
        // loses its ambient layer too. That is the trade for a band that can
        // arrive and leave without hurting.
        RE::TESEffectShader* MaterialiseShader() {
            auto* data = RE::TESDataHandler::GetSingleton();
            if (!data) { return nullptr; }
            auto* shader = data->LookupForm<RE::TESEffectShader>(
                kMaterialiseShaderLocal, kSkyrim);
            if (!shader) { return nullptr; }
            static bool silenced = false;
            if (!silenced) {
                silenced = true;
                if (shader->data.ambientSound) {
                    // CAPTURE BEFORE NULLING. This is the only handle we get
                    // on a conjuration sound that certainly belongs to this
                    // effect; once the field is cleared it is gone.
                    g_materialiseSound = shader->data.ambientSound;
                    spdlog::info(
                        "[band] captured arrival sound 0x{:08X} and cleared "
                        "it from the shader - it now plays once, not once "
                        "per performer",
                        g_materialiseSound->GetFormID());
                    shader->data.ambientSound = nullptr;
                } else {
                    spdlog::warn(
                        "[band] materialise shader has no ambientSound - the "
                        "arrival will be silent, and there is no descriptor "
                        "here to play instead");
                }
            }
            return shader;
        }

        // The vanilla conjuration art. Nothing is nulled or altered on it -
        // its sound is inside the NIF, out of reach.
        RE::BGSArtObject* SummonArt() {
            auto* data = RE::TESDataHandler::GetSingleton();
            if (!data) { return nullptr; }
            return data->LookupForm<RE::BGSArtObject>(
                kSummonTargetArtLocal, kSkyrim);
        }

        [[nodiscard]] bool HasStage() {
            for (const auto& handle : g_performers) {
                if (handle) { return true; }
            }
            return static_cast<bool>(g_drumProp);
        }

        auto TakePerformers() {
            auto result = g_performers;
            for (auto& handle : g_performers) { handle.reset(); }
            return result;
        }

        auto TakeDrumProp() {
            auto result = g_drumProp;
            g_drumProp.reset();
            return result;
        }

        void DeleteStage(
            const std::array<
                RE::ActorHandle,
                band::kFormation.size()>& a_handles,
            const RE::ObjectRefHandle& a_drumProp) {
            std::size_t deleted = 0;
            for (const auto& handle : a_handles) {
                if (auto actor = handle.get()) {
                    actor->StopCombat();
                    actor->Disable();
                    actor->SetDelete(true);
                    ++deleted;
                }
            }
            if (auto drum = a_drumProp.get()) {
                drum->Disable();
                drum->SetDelete(true);
            }
            spdlog::info(
                "[band] {} conjured performer(s) and drum prop disabled "
                "and marked for delete",
                deleted);
        }

        void RollBackSpawn() {
            DeleteStage(TakePerformers(), TakeDrumProp());
            (void)g_lifecycle.End();
            g_activeMirror.store(false, std::memory_order_release);
        }

        RE::NiPoint3 StagePosition(
            const RE::NiPoint3& a_playerPos,
            float a_heading,
            const band::PerformerSpec& a_spec) {
            return {
                a_playerPos.x + std::cos(a_heading) * a_spec.right
                    - std::sin(a_heading) * a_spec.forward,
                a_playerPos.y + std::sin(a_heading) * a_spec.right
                    + std::cos(a_heading) * a_spec.forward,
                a_playerPos.z,
            };
        }

        bool SendFirstAccepted(
            RE::Actor& a_actor,
            const band::EventCandidates& a_events,
            std::string_view& a_acceptedEvent) {
            for (const auto event : a_events) {
                if (a_actor.NotifyAnimationGraph(
                        RE::BSFixedString{ event.data() })) {
                    a_acceptedEvent = event;
                    return true;
                }
            }
            a_acceptedEvent = a_events.front();
            return false;
        }
    }

    void Begin(
        int a_instrumentContext,
        const band::StemAvailability& a_stems) {
        const auto decision = g_lifecycle.Begin(a_instrumentContext);
        if (decision == band::BeginDecision::kIgnore) { return; }
        if (decision == band::BeginDecision::kAlreadyActive) {
            spdlog::debug("[band] conjured ensemble already active");
            return;
        }

        auto* data = RE::TESDataHandler::GetSingleton();
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* bass =
            data ? data->LookupForm<RE::TESObjectWEAP>(kBassLocal, kAddon)
                 : nullptr;
        auto* guitarWeapon =
            data ? data->LookupForm<RE::TESObjectWEAP>(
                       kGuitarWeaponLocal, kAddon)
                 : nullptr;
        // Resolve the winning record by full FormID. SGT overrides Skyrim's
        // drum, and plugin-local lookup returned null for that winning form
        // in the field even though the object itself was loaded.
        auto* drum =
            RE::TESForm::LookupByID<RE::TESBoundObject>(kDrumForm);
        const auto resourceDecision = band::DecideResources(
            player != nullptr, data != nullptr, bass != nullptr,
            guitarWeapon != nullptr, drum != nullptr);
        if (resourceDecision == band::ResourceDecision::kAbort) {
            spdlog::warn(
                "[band] ensemble unavailable: player={} data={} bass={} "
                "guitar={} drum={}",
                player != nullptr, data != nullptr, bass != nullptr,
                guitarWeapon != nullptr, drum != nullptr);
            RollBackSpawn();
            return;
        }
        if (resourceDecision
            == band::ResourceDecision::kSpawnWithoutDrum) {
            spdlog::warn(
                "[band] drummer prop unavailable; continuing with all "
                "four performers");
        }

        const auto playerPos = player->GetPosition();
        const float heading = player->GetAngleZ();
        g_stageHeading = heading;
        g_stemAvailability = a_stems;
        spdlog::info(
            "[band] explicit role stems={} mixedBacking={} bass={} "
            "rhythm={} drums={} vocals={}",
            a_stems.anyRoleDedicated, a_stems.mixedBacking, a_stems.bass,
            a_stems.rhythm, a_stems.drums, a_stems.vocals);
        for (std::size_t i = 0; i < band::kFormation.size(); ++i) {
            const auto& spec = band::kFormation[i];
            auto* actorBase =
                data->LookupForm<RE::TESNPC>(spec.actorLocalId, kAddon);
            if (!actorBase) {
                spdlog::warn(
                    "[band] missing {} actor base {:06X}",
                    spec.label, spec.actorLocalId);
                RollBackSpawn();
                return;
            }

            auto placed = player->PlaceObjectAtMe(actorBase, false);
            auto* actor = placed ? placed->As<RE::Actor>() : nullptr;
            if (!actor) {
                spdlog::warn(
                    "[band] failed to conjure {}", spec.label);
                RollBackSpawn();
                return;
            }

            const auto stagePos =
                StagePosition(playerPos, heading, spec);
            g_stagePositions[i] = stagePos;
            // Below the stage mark, out of render, until the arrival beat
            // lifts it - alpha cannot hide a loading actor (see
            // kSpawnConcealmentDepth).
            auto concealed = stagePos;
            concealed.z -= band::kSpawnConcealmentDepth;
            actor->SetPosition(concealed, true);
            actor->SetRotationZ(heading);
            actor->SetCollision(false);
            actor->StopCombat();
            // Belt-and-braces; concealment is what actually guarantees
            // nothing shows early.
            actor->SetAlpha(0.0f);

            g_performers[i] = actor->GetHandle();
            spdlog::info(
                "[band] conjured {} concealed {:.0f} below stage mark "
                "({:.1f}, {:.1f}, {:.1f}); lift comes with the portal",
                spec.label, band::kSpawnConcealmentDepth,
                stagePos.x, stagePos.y, stagePos.z);
        }

        spdlog::info(
            "[band] enchanted guitar conjured the full skeleton ensemble");
        g_performanceStarted.fill(false);
        g_arrivalRevealed.fill(false);
        // Per-band, not per-process: every arrival gets its own sound.
        g_arrivalSoundPlayed = false;
        g_arrivalArtApplied = 0;
        g_ghostApplied.fill(false);
        g_arrivalWarmupPulses.fill(0);
        g_headTrackingDisabled.fill(false);
        g_anyPulseExecuted = false;
        g_lutePropDirty = false;
        g_nextPerformancePulse.store(-1.0, std::memory_order_relaxed);
        g_performancePulse.store(0, std::memory_order_relaxed);
        g_activeMirror.store(true, std::memory_order_release);
    }

    bool Active() {
        return g_activeMirror.load(std::memory_order_acquire);
    }

    void Tick(double a_now) {
        if (!g_activeMirror.load(std::memory_order_acquire)) { return; }
        // No hide poll here, ever: a 50ms alpha re-assert fought the
        // engine's spawn fade-in and the alternation was visible stutter
        // (2026-07-27). Concealment needs no per-tick help.
        double expected =
            g_nextPerformancePulse.load(std::memory_order_relaxed);
        if (expected < 0.0) {
            if (!g_nextPerformancePulse.compare_exchange_strong(
                expected, a_now + band::kFirstPerformancePulseDelay,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
                return;
            }
            expected = a_now + band::kFirstPerformancePulseDelay;
        }
        if (a_now < expected) { return; }
        if (!g_nextPerformancePulse.compare_exchange_strong(
                expected, a_now + band::kPerformancePulseSeconds,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            return;
        }
        const auto pulse =
            g_performancePulse.fetch_add(1, std::memory_order_relaxed);
        SKSE::GetTaskInterface()->AddTask([pulse] {
            if (!g_activeMirror.load(std::memory_order_acquire)) { return; }
            const auto now = std::chrono::steady_clock::now();
            const double sinceLastPulse = g_anyPulseExecuted
                ? std::chrono::duration<double>(
                      now - g_lastPulseExecuted).count()
                : 0.0;
            if (band::PulseIsBacklog(g_anyPulseExecuted, sinceLastPulse)) {
                // The 14:57 field run drained ~12 queued pulses within 3ms
                // after the song-start load, which collapsed the
                // portal-before-reveal beat and re-pointed the shared
                // AnimObjectLute before the bassist's graph had captured
                // its bass. Backlog is dropped, not executed.
                spdlog::info(
                    "[band] pulse {} dropped as task backlog "
                    "({:.0f}ms after previous pulse)",
                    pulse, sinceLastPulse * 1000.0);
                return;
            }
            g_anyPulseExecuted = true;
            g_lastPulseExecuted = now;
            // A band swap of the shared lute record has had its capture
            // interval: put the record back on the session baseline so the
            // NEXT capture - the player's graph on a pose refresh, or
            // whatever performs after this band - gets the right
            // instrument. Before the member loop on purpose: a member
            // swapping this same pulse re-dirties it afterwards.
            if (g_lutePropDirty &&
                std::chrono::duration<double>(now - g_lutePropDirtyAt)
                        .count() >= band::kSharedPropCaptureSeconds) {
                g_lutePropDirty = false;
                SH::SgtVm::ReassertAnimObjectBaseline();
            }
            auto* ghostShader =
                RE::TESForm::LookupByID<RE::TESEffectShader>(
                    kGhostShaderForm);
            auto* materialiseShader = MaterialiseShader();
            auto* summonArt = SummonArt();
            auto* luteAnimObject =
                RE::TESForm::LookupByID<RE::TESObjectANIO>(
                    kLuteAnimObjectForm);
            auto* drinkPotionAnimObject =
                RE::TESForm::LookupByID<RE::TESObjectANIO>(
                    kDrinkPotionAnimObjectForm);
            for (std::size_t i = 0; i < band::kFormation.size(); ++i) {
                if (auto actor = g_performers[i].get()) {
                    const auto arrival = band::DecideArrival(
                        g_arrivalRevealed[i],
                        actor->Get3D() != nullptr,
                        summonArt != nullptr,
                        band::ArrivalDue(g_arrivalWarmupPulses[i]));
                    if (arrival.hide) {
                        // Warming up below the stage. The counter only runs
                        // once the 3D exists - the warm-up measures fade-in
                        // and graph time, both of which start with the 3D.
                        actor->SetAlpha(0.0f);
                        if (actor->Get3D()) {
                            ++g_arrivalWarmupPulses[i];
                        }
                    }
                    if (arrival.reveal) {
                        // THE ARRIVAL BEAT - everything on this one pulse,
                        // on a VISIBLE performer (attached art inherits
                        // actor alpha, so effects on a hidden actor vanish
                        // with it; see DecideArrival).
                        actor->SetPosition(g_stagePositions[i], true);
                        // Not 1.0: the performers are apparitions. Real
                        // translucency comes from actor alpha; the ghost
                        // membrane shader alone left them bone-opaque.
                        actor->SetAlpha(band::kApparitionAlpha);
                        g_arrivalRevealed[i] = true;
                        spdlog::info(
                            "[band] {} arrived after {} warm-up pulses",
                            band::kFormation[i].label,
                            g_arrivalWarmupPulses[i]);
                        if (!materialiseShader && !summonArt) {
                            spdlog::warn(
                                "[band] {} arrived with no materialise "
                                "effect at all; shader 0x{:06X} and art "
                                "0x{:06X} both unavailable",
                                band::kFormation[i].label,
                                kMaterialiseShaderLocal,
                                kSummonTargetArtLocal);
                        }
                    }
                    if (arrival.playShader && materialiseShader) {
                        (void)actor->InstantiateHitShader(
                            materialiseShader, band::kArrivalEffectSeconds,
                            actor.get(), false, false);
                        spdlog::info(
                            "[band] {} materialise shader applied",
                            band::kFormation[i].label);
                    }
                    if (arrival.playArt) {
                        // ⚠ NEVER DEREFERENCE WHAT THIS RETURNS - on
                        // 1.6.1170 InstantiateHitArt came back as literally
                        // 0x1 and reading through it crashed the game; the
                        // returned value is never followed.
                        //
                        // Who gets the art is band::BearsArrivalArt's call
                        // alone; the sound rides inside it. All bearers
                        // fire in this same pulse.
                        if (band::BearsArrivalArt(
                                band::kFormation[i].role)) {
                            (void)actor->InstantiateHitArt(
                                summonArt, band::kArrivalEffectSeconds,
                                actor.get(), false, false);
                            ++g_arrivalArtApplied;
                            spdlog::info(
                                "[band] conjuration art on {} ({} so far "
                                "this arrival, each one its own sound)",
                                band::kFormation[i].label,
                                g_arrivalArtApplied);
                        }
                    } else if (
                        arrival.reveal && !g_arrivalSoundPlayed
                        && !summonArt) {
                        // No art object resolvable: the captured-descriptor
                        // fallback still gives the arrival its one sound.
                        g_arrivalSoundPlayed = true;
                        spdlog::warn(
                            "[band] SummonTargetFX 0x{:06X} unavailable - "
                            "falling back to the captured descriptor",
                            kSummonTargetArtLocal);
                        PlayMaterialiseSound(actor->GetPosition());
                    }
                    // Field 2026-07-25: applying this on the first pulse
                    // put the membrane on an actor with NO 3D yet - it
                    // attached to nothing and the skeletons stayed
                    // opaque. Wait for loaded 3D and the started
                    // performance so the instrument is covered too, and
                    // trust the RESULT, not the call.
                    if (ghostShader
                        && band::GhostShaderShouldApply(
                               g_ghostApplied[i],
                               actor->Get3D() != nullptr,
                               g_performanceStarted[i]
                                   || !band::RoleIsLive(
                                          band::kFormation[i].role,
                                          g_stemAvailability))) {
                        auto* ghostFx = actor->InstantiateHitShader(
                            ghostShader, 3600.0f, actor.get(), false, false);
                        g_ghostApplied[i] = ghostFx != nullptr;
                        // Re-pin the apparition alpha alongside the
                        // membrane: at short warm-ups the spawn fade-in can
                        // push the actor back to 1.0 after the reveal. One
                        // call on an existing pulse - not a poll.
                        actor->SetAlpha(band::kApparitionAlpha);
                        spdlog::info(
                            "[band] {} spectral shader instantiated={}",
                            band::kFormation[i].label, ghostFx != nullptr);
                    }
                    // Dialogue and packages re-enable the headtracking
                    // modifier, so the graph booleans are re-cleared on
                    // every pulse; only the first SUCCESSFUL clear is
                    // logged (see ShouldLogHeadTrackingClear - pulse 0
                    // runs before the graph loads and always fails). The
                    // `HeadTrackingOff` EVENT looked right and even exists
                    // in 0_master.hkx, but the 14:57 field run rejected it
                    // on every performer on every pulse.
                    {
                        const bool cleared = actor->SetGraphVariableBool(
                            RE::BSFixedString{
                                band::kHeadTrackingVariable.data() },
                            false);
                        (void)actor->SetGraphVariableBool(
                            RE::BSFixedString{
                                band::kHeadTrackSpineVariable.data() },
                            false);
                        if (band::ShouldLogHeadTrackingClear(
                                g_headTrackingDisabled[i], cleared,
                                pulse)) {
                            g_headTrackingDisabled[i] = true;
                            spdlog::info(
                                "[band] {} head tracking graph variable "
                                "cleared={} (pulse {})",
                                band::kFormation[i].label, cleared, pulse);
                        }
                    }
                    // Never expose an unanimated skeleton ahead of its
                    // conjuration portal. The art receives one full pulse
                    // before reveal and performance setup.
                    if (!g_arrivalRevealed[i]) {
                        continue;
                    }
                    if (!band::RoleIsLive(
                            band::kFormation[i].role,
                            g_stemAvailability)) {
                        continue;
                    }
                    if (!band::ShouldAttemptPerformance(
                            band::kFormation[i].role,
                            g_performanceStarted[i], pulse)) {
                        continue;
                    }
                    if (band::kFormation[i].role
                        == band::Role::kRhythmGuitarist) {
                        // AnimObjectLute is shared. The bassist must hold it
                        // for a real capture interval before its model is
                        // re-pointed at the V guitar; a pulse count is not a
                        // clock (see PulseIsBacklog). kFormation[0] is the
                        // bassist.
                        const bool bassStarted = g_performanceStarted[0];
                        const double sinceBassStart = bassStarted
                            ? std::chrono::duration<double>(
                                  now - g_bassStartedAt).count()
                            : 0.0;
                        if (band::RhythmMustWaitForBass(
                                band::RoleIsLive(
                                    band::Role::kBassist,
                                    g_stemAvailability),
                                bassStarted, sinceBassStart)) {
                            continue;
                        }
                    }

                    // The human-behavior skeleton prototype uses real looped
                    // musician idles. Start each role once after its 3D is
                    // ready; only rejected events are retried.
                    actor->SetPosition(g_stagePositions[i], true);
                    actor->SetRotationZ(g_stageHeading);

                    const auto prop = band::PerformanceProp(
                        band::kFormation[i].role);
                    if (luteAnimObject
                        && prop == band::PropKind::kBassAnimationObject) {
                        luteAnimObject->SetModel(kBassAnimModel);
                        g_lutePropDirty   = true;
                        g_lutePropDirtyAt = now;
                    } else if (
                        luteAnimObject
                        && prop
                            == band::PropKind::kGuitarAnimationObject) {
                        luteAnimObject->SetModel(kGuitarAnimModel);
                        g_lutePropDirty   = true;
                        g_lutePropDirtyAt = now;
                    } else if (
                        drinkPotionAnimObject
                        && prop
                            == band::PropKind::kMicrophoneAnimationObject) {
                        drinkPotionAnimObject->SetModel(
                            kMicrophoneAnimModel);
                    }

                    std::string_view acceptedEvent;
                    const auto events = band::PerformanceEvents(
                        band::kFormation[i].role, pulse);
                    const bool accepted =
                        SendFirstAccepted(*actor, events, acceptedEvent);
                    g_performanceStarted[i] = accepted;
                    if (accepted
                        && band::kFormation[i].role
                            == band::Role::kBassist) {
                        // Starts the rhythm guitarist's shared-prop capture
                        // clock (RhythmMustWaitForBass).
                        g_bassStartedAt = now;
                    }
                    spdlog::info(
                        "[band] {} performance start attempt={} "
                        "event={} accepted={}",
                        band::kFormation[i].label, pulse,
                        acceptedEvent, accepted);
                }
            }
        });
    }

    void EndNow(std::string_view a_reason, bool a_immediate) {
        const bool wasActive = g_lifecycle.End();
        if (!wasActive && !HasStage()) {
            return;
        }
        if (!wasActive) {
            spdlog::warn(
                "[band] orphaned ensemble handles found during cleanup");
        }
        g_activeMirror.store(false, std::memory_order_release);
        g_nextPerformancePulse.store(-1.0, std::memory_order_relaxed);

        auto handles = TakePerformers();
        auto drumProp = TakeDrumProp();
        if (auto* drinkPotionAnimObject =
                RE::TESForm::LookupByID<RE::TESObjectANIO>(
                    kDrinkPotionAnimObjectForm)) {
            drinkPotionAnimObject->SetModel(
                kVanillaDrinkPotionAnimModel);
        }
        // The lute record got the same treatment as the drink potion only
        // implicitly, via the pulse-top re-assert - which never runs again
        // after the LAST swap of a run (no rhythm stem: record left on
        // BASS for every performance after this band, ours or vanilla's).
        // Teardown restores it unconditionally, exactly like the potion.
        g_lutePropDirty = false;
        SH::SgtVm::ReassertAnimObjectBaseline();
        // Dismissal mirrors arrival exactly: the shader on all four, the art
        // object on the bearer alone. This path is where the noise was still
        // audible after the arrival was first fixed - "we also hear the sfx
        // when you end the song" - because dismissal applied the loud ARTO to
        // ALL FOUR performers. Four was the problem, not one; a silent vanish
        // after a sounded arrival just felt lopsided.
        auto* materialiseShader = MaterialiseShader();
        auto* summonArt = SummonArt();
        bool dismissSoundPlayed = false;
        // Indexed rather than ranged: `handles` is index-aligned with
        // kFormation, and the bearer is chosen by ROLE, not by whichever
        // performer the loop happens to reach first.
        for (std::size_t i = 0; i < handles.size(); ++i) {
            if (auto actor = handles[i].get()) {
                actor->StopCombat();
                actor->NotifyAnimationGraph("attackStop");
                actor->NotifyAnimationGraph("shoutStop");
                actor->NotifyAnimationGraph("IdleStop");
                actor->NotifyAnimationGraph("IdleForceDefaultState");
                actor->DrawWeaponMagicHands(false);
                if (!a_immediate && materialiseShader) {
                    (void)actor->InstantiateHitShader(
                        materialiseShader, band::kDismissVisualSeconds,
                        actor.get(), false, false);
                    // Gated on !a_immediate with the visual: an immediate
                    // teardown is a save load or a hard abort, where a
                    // conjuration noise over the loading screen would be
                    // nobody's idea of correct.
                    // Symmetric with arrival by the same knob: whoever bears
                    // the art on the way in bears it on the way out, so the
                    // band does not vanish differently from how it appeared.
                    if (summonArt
                        && band::BearsArrivalArt(
                               band::kFormation[i].role)) {
                        (void)actor->InstantiateHitArt(
                            summonArt, band::kDismissVisualSeconds,
                            actor.get(), false, false);
                        spdlog::info(
                            "[band] conjuration art on {} departing",
                            band::kFormation[i].label);
                    } else if (!dismissSoundPlayed && !summonArt) {
                        dismissSoundPlayed = true;
                        PlayMaterialiseSound(actor->GetPosition());
                    }
                }
            }
        }

        spdlog::info(
            "[band] dismissing conjured ensemble: {} (immediate={})",
            a_reason, a_immediate);
        if (a_immediate) {
            DeleteStage(handles, drumProp);
            return;
        }

        std::thread([
            handles = std::move(handles),
            drumProp = std::move(drumProp)] {
            std::this_thread::sleep_for(kDismissDelay);
            SKSE::GetTaskInterface()->AddTask(
                [handles, drumProp] { DeleteStage(handles, drumProp); });
        }).detach();
    }

    void EndAsync(std::string_view a_reason) {
        std::string reason{ a_reason };
        SKSE::GetTaskInterface()->AddTask(
            [reason = std::move(reason)] { EndNow(reason); });
    }
}
