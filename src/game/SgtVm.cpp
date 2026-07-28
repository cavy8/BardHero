// src/game/SgtVm.cpp
#include "PCH.h"
#include "game/SgtVm.h"

#include "Settings.h"
#include "game/AudienceLifecycleLogic.h"
#include "game/EndingLogic.h"
#include "game/GuitarPropLogic.h"

#include "RE/A/ActiveEffect.h"
#include "RE/A/AlchemyItem.h"
#include "RE/B/BGSMessage.h"
#include "RE/B/BGSScene.h"
#include "RE/B/BGSRefAlias.h"
#include "RE/B/BSString.h"
#include "RE/C/ControlMap.h"     // cancel-window control probe
#include "RE/E/Effect.h"
#include "RE/P/PlayerCamera.h"   // cancel-window control probe
#include "RE/I/IObjectHandlePolicy.h"
#include "RE/M/MagicItem.h"
#include "RE/M/MagicTarget.h"
#include "RE/M/Misc.h"
#include "RE/T/TESDataHandler.h"
#include "RE/T/TESGlobal.h"
#include "RE/T/TESIdleForm.h"
#include "RE/T/TESObjectANIO.h"
#include "RE/T/TESQuest.h"
#include "RE/T/TESTopic.h"
#include "RE/V/VirtualMachine.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace SH::SgtVm {
    namespace {
        constexpr const char* kScriptClass = "_Talent_PlayInstrument";
        constexpr const char* kSgtPlugin   = "SkyrimsGotTalent-Bards.esp";
        // audience quest local id (survey 2026-07-18)
        constexpr RE::FormID kAudienceQuestLocal = 0x00AFE0;

        class NoopCallback : public RE::BSScript::IStackCallbackFunctor {
        public:
            void operator()(RE::BSScript::Variable) override {}
            void SetObject(
                const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}
        };
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> Noop() {
            return RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>(
                new NoopCallback());
        }

        // Stores a Papyrus bool result into an atomic slot (-1 unknown /
        // 0 false / 1 true). The VM may invoke this on any thread; the
        // keeper consumes the slot on the game thread next pass - at the
        // 5s cadence one-pass-delayed data is fine.
        class BoolResultCallback : public RE::BSScript::IStackCallbackFunctor {
        public:
            explicit BoolResultCallback(std::atomic<int>& a_slot)
                : slot_(&a_slot) {}
            void operator()(RE::BSScript::Variable a_result) override {
                slot_->store(a_result.IsBool()
                                 ? (a_result.GetBool() ? 1 : 0)
                                 : -1);
            }
            void SetObject(
                const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}

        private:
            std::atomic<int>* slot_;
        };

        RE::ActiveEffect* FindPerformEffect(RE::FormID a_spellId) {
            auto* pc = RE::PlayerCharacter::GetSingleton();
            if (!pc) { return nullptr; }
            // This build is compiled HAS_SKYRIM_MULTI_TARGETING (AE+SE+VR
            // in one binary), so Actor does NOT statically inherit
            // MagicTarget (its multiple-inheritance layout differs between
            // SE and AE); AsMagicTarget() runtime-relocates to the correct
            // sub-object for whichever runtime is actually loaded.
            auto* mt = pc->AsMagicTarget();
            if (!mt) { return nullptr; }
            auto* list = mt->GetActiveEffectList();
            if (!list) { return nullptr; }
            for (auto* e : *list) {
                if (e && e->spell && e->spell->GetFormID() == a_spellId &&
                    !e->flags.any(RE::ActiveEffect::Flag::kDispelled)) {
                    return e;
                }
            }
            return nullptr;
        }

        RE::BSTSmartPointer<RE::BSScript::Object> ScriptObject(
            RE::ActiveEffect* a_effect) {
            RE::BSTSmartPointer<RE::BSScript::Object> obj;
            auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
            if (!vm) { return obj; }
            auto*      policy = vm->GetObjectHandlePolicy();
            const auto handle = policy->GetHandleForObject(
                RE::ActiveEffect::VMTYPEID, a_effect);
            if (handle == policy->EmptyHandle()) { return obj; }
            vm->FindBoundObject(handle, kScriptClass, obj);
            return obj;
        }

        // Bound "Scene" script object for a BGSScene form, mirroring the
        // ActiveEffect handle pattern (forms use their FormType as VMTypeID).
        RE::BSTSmartPointer<RE::BSScript::Object> SceneObject(
            RE::BGSScene* a_scene) {
            RE::BSTSmartPointer<RE::BSScript::Object> obj;
            if (!a_scene) { return obj; }
            auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
            if (!vm) { return obj; }
            auto*      policy = vm->GetObjectHandlePolicy();
            const auto handle = policy->GetHandleForObject(
                static_cast<RE::VMTypeID>(RE::FormType::Scene), a_scene);
            if (handle == policy->EmptyHandle()) { return obj; }
            vm->FindBoundObject(handle, "Scene", obj);
            return obj;
        }

        // Script VARIABLE (not property) by name. Object::variables is one
        // flat array for the whole class chain, BASE CLASSES FIRST; each
        // ObjectTypeInfo lists only its own variables - walk root->leaf
        // accumulating the offset.
        RE::BSScript::Variable* FindVariable(RE::BSScript::Object* a_obj,
                                             std::string_view a_name) {
            if (!a_obj) { return nullptr; }
            std::vector<RE::BSScript::ObjectTypeInfo*> chain;
            for (auto* ti = a_obj->GetTypeInfo(); ti; ti = ti->GetParent()) {
                chain.push_back(ti);
            }
            std::uint32_t base = 0;
            for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                auto*      ti   = *it;
                auto*      vars = ti->GetVariableIter();
                const auto n    = ti->GetNumVariables();
                for (std::uint32_t i = 0; vars && i < n; ++i) {
                    if (a_name == vars[i].name.c_str()) {
                        return std::addressof(a_obj->variables[base + i]);
                    }
                }
                base += n;
            }
            return nullptr;
        }

        // browse-standstill latches (game thread only; reset via
        // ResetStandstillLatch when a standstill begins)
        std::int32_t g_ssStopped      = 0;
        bool         g_ssIdleNeutered = false;  // property blank done
        bool         g_ssCancelSent   = false;  // one-shot fallback cancel
        // pick vs cancel gate (session thread writes at pick): the freeze
        // queues pass tasks that burst at unfreeze - after the pick - so
        // the blank must be retractable without a task round-trip
        std::atomic<bool> g_ssBlankArmed{ false };
        // originals saved at blank time so a pick that CONTINUES the
        // blanked instance can restore them (game thread only)
        RE::BSScript::Variable g_ssSavedIdle[2];
        bool                   g_ssSaved = false;
        // Browse CANCEL: armed at the cancel edge, cleared at the strip.
        // While armed every pass watches for SGT's start thread taking the
        // controls and hands them straight back (see ReleaseCancelControls).
        std::atomic<bool> g_ssFreeArmed{ false };
        bool              g_ssFreeLogged = false;  // game thread only

        // per-performance one-shot latches; a vanished effect resets them
        // (a new cast = a new effect instance = a new SongToPlay). The
        // *Warned latches are field diagnostics: Papyrus logging is off in
        // this game profile, so spdlog is the only eye - each anomaly must
        // log exactly once per performance, not every 5s keeper pass.
        std::int32_t g_stoppedInstance = 0;
        bool         g_timerSuppressed = false;
        bool         g_varWarned       = false;  // SongToPlay missing/non-int
        int          g_scriptMisses    = 0;      // passes with no script obj
        bool         g_scriptBoundLogged = false;  // bind-after-miss logged once
        bool         g_idleVarWarned   = false;  // bIdlePlaying unreadable
        // SGT's OWN exit animation for the lute pose, cached off its script
        // object while that object is still reachable (the effect is gone by
        // the time we release). SGT ends the pose with
        // PlayIdle(IdleStop_Loose) at psc:280, inside the cleanup that dies
        // with the dispelled effect - so nothing ever ends the idle and the
        // player is left unable to walk while jump still works, because jump
        // fires as a graph event from INSIDE an idle and walking needs the
        // idle to end. `IdleForceDefaultState` is accepted by the graph and
        // does not end it; accepted != transitioned.
        RE::BSFixedString g_stopIdleEvent{};

        // _Talent_EnableMovement (SGT global, local 0x0289A6 - read out of
        // SkyrimsGotTalent-Bards.esp; the same parse reproduces the three
        // known-good expertise globals 0x000D62/61/63, so it is trusted).
        //
        // This ONE global decides both halves of a field defect
        // (2026-07-20: "the lute is invisible... when i quit i cannot walk,
        // but i can jump"). SGT's OnEffectStart branches on it:
        //
        //   ==1 : DisablePlayerControls(abMovement=FALSE, ...)
        //         AddItem(DummyInstrument)      <- the visible instrument
        //   else: DisablePlayerControls(abMovement=TRUE,  ...)
        //         no prop at all
        //
        // At 0 the player loses movement (jump is not one of the args, which
        // is exactly the "can't walk but can jump" signature) AND never gets
        // the prop. Force it to 1 for the performance and put the player's
        // own value back afterwards, per spec section 7.
        //
        // ⚠ This governs SGT's behaviour, NOT ours. BardHero's MovementGuard
        // still blocks movement for the session by design - the two must not
        // be confused. What changes is that WE own that block and reliably
        // release it, instead of SGT taking movement and never giving it back.
        constexpr RE::FormID kEnableMovementLocal = 0x0289A6;
        RE::TESGlobal*       g_moveGlob           = nullptr;
        float                g_moveOrig           = -1.0f;  // <0 = none saved
        bool                 g_moveGlobWarned     = false;
        constexpr RE::FormID kLuteAnimObjectId    = 0x00093643;
        RE::TESObjectANIO* g_luteAnimObject       = nullptr;
        std::string        g_luteAnimOriginalModel;
        bool               g_luteAnimModelSaved   = false;
        bool               g_guitarAnimOverride   = false;
        bool               g_luteAnimObjectWarned = false;

        // Electric player-perform signal (band-animation session request
        // 2026-07-25): the "Player Guitar" OAR submod conditions the
        // player's animobjectluteloop replacement on this addon global
        // being nonzero. The player's electric perform IS a lute
        // performance with only the ANIO model swapped, so this global is
        // the only animation-layer signal that distinguishes them. It
        // lives in SAVES: never assume it starts at 0 (cleared at
        // kDataLoaded and on every post-load/new-game).
        constexpr RE::FormID kElectricPerformGlobLocal = 0x80C;
        constexpr auto kElectricAddon = "Bard Hero - Doom Lute.esp";
        RE::TESGlobal* g_electricPerformGlob   = nullptr;
        bool           g_electricGlobWarned    = false;

        // game thread; soft - an absent addon logs once and no-ops
        void WriteElectricPerformGlobal(float a_value) {
            if (!g_electricPerformGlob) {
                auto* dh = RE::TESDataHandler::GetSingleton();
                g_electricPerformGlob =
                    dh ? dh->LookupForm<RE::TESGlobal>(
                             kElectricPerformGlobLocal, kElectricAddon)
                       : nullptr;
            }
            if (!g_electricPerformGlob) {
                if (!g_electricGlobWarned) {
                    g_electricGlobWarned = true;
                    spdlog::warn(
                        "[guitar] {}|0x{:06X} "
                        "(BardHeroElectricPlayerPerform) not found; the "
                        "player electric body animation stays on the "
                        "lute clip",
                        kElectricAddon, kElectricPerformGlobLocal);
                }
                return;
            }
            if (g_electricPerformGlob->value != a_value) {
                spdlog::info(
                    "[guitar] electric perform global {} -> {}",
                    g_electricPerformGlob->value, a_value);
            }
            g_electricPerformGlob->value = a_value;
        }

        // duet observation state. Playing data comes from async Papyrus
        // Scene.IsPlaying() dispatched each pass and consumed the NEXT pass
        // (atomics: the VM callback may run off the game thread).
        constexpr int    kDuetUnknown = -3;
        std::atomic<int> g_duetAnswer[2] = {-1, -1};  // -1 no data / 0 / 1
        // kDuetUnknown no data yet / 0 none / 1|2 that scene playing /
        // -1|-2 that scene stopped (log fired)
        int  g_duetSeen        = kDuetUnknown;
        bool g_duetWarned      = false;  // scene unresolvable/unbound (once)
        bool g_duetResolveLogged = false;  // first-pass resolve status line
        int  g_duetRestarts    = 0;      // Start() re-dispatches (max 3)
        bool g_duetCapLogged   = false;  // restart cap hit (once)

        // Resolve a Scene-typed script property off the effect script, same
        // pattern as IdleToPlay (GetProperty -> GetObject -> Resolve). The
        // returned form feeds SceneObject() for the IsPlaying/Start dispatch.
        RE::BGSScene* ResolveScene(RE::BSScript::Object* a_obj,
                                   const char* a_prop) {
            if (!a_obj) { return nullptr; }
            auto* prop = a_obj->GetProperty(a_prop);
            if (!prop) { return nullptr; }
            auto sceneObj = prop->GetObject();
            if (!sceneObj) { return nullptr; }
            return static_cast<RE::BGSScene*>(sceneObj->Resolve(
                static_cast<RE::VMTypeID>(RE::FormType::Scene)));
        }

        // ---- the ovation global -----------------------------------------
        // SGT's MessageAndEXP sets this to 1 on its FIRST line, before it
        // has looked at anything, so the cheer animations play for a 0% run
        // exactly as they do for a perfect one. Resolved by editor ID for
        // the same reason MoodGlobals does it: a merged or repacked SGT
        // keeps working, and a renumber shows up as a log line rather than
        // a feature that silently stops.
        constexpr const char* kOvationEdid = "_Talent_ReceiveOvation";
        RE::TESGlobal*        g_ovation    = nullptr;
        bool                  g_ovationLooked = false;

        void SetOvation(bool a_on) {
            if (!g_ovationLooked) {
                g_ovationLooked = true;
                if (auto* dh = RE::TESDataHandler::GetSingleton()) {
                    for (auto* g : dh->GetFormArray<RE::TESGlobal>()) {
                        if (!g) { continue; }
                        const char* edid = g->GetFormEditorID();
                        if (edid && std::strcmp(edid, kOvationEdid) == 0) {
                            g_ovation = g;
                            break;
                        }
                    }
                }
                if (g_ovation) {
                    spdlog::info("[ending] {} = 0x{:08X}", kOvationEdid,
                                 g_ovation->formID);
                } else {
                    // Fail OPEN: leaving the global alone means the crowd
                    // behaves exactly as it did before this feature, which
                    // is a worse ending but not a broken one.
                    spdlog::warn(
                        "[ending] {} not found - the crowd will react the "
                        "old way (rank, not performance)", kOvationEdid);
                }
            }
            if (g_ovation) { g_ovation->value = a_on ? 1.0f : 0.0f; }
        }

        // Present SGT's authored reaction text, but not its mechanical
        // "(Active Debuff)" / "(Active Bonus)" suffix. The actual effect is
        // still applied below. Reading the BGSMessage description lets us
        // keep the same localized flavor without editing SGT's record or
        // loose/compiled scripts.
        bool ShowReactionMessage(RE::BSScript::Object* a_obj,
                                 const char* a_name,
                                 ending::Valence a_valence) {
            std::string text;
            bool resolved = false, sanitized = false;
            if (a_obj && a_name) {
                if (auto* prop = a_obj->GetProperty(a_name)) {
                    if (auto msgObj = prop->GetObject()) {
                        auto* message = static_cast<RE::BGSMessage*>(
                            msgObj->Resolve(static_cast<RE::VMTypeID>(
                                RE::FormType::Message)));
                        if (message) {
                            RE::BSString description;
                            message->GetDescription(description, message);
                            const std::string raw = description.c_str();
                            text = ending::SanitizeReactionText(raw);
                            resolved = !raw.empty();
                            sanitized = resolved && text != raw;
                        }
                    }
                }
            }
            if (text.empty()) {
                text = ending::FallbackReactionText(a_valence);
            }
            RE::DebugNotification(text.c_str());
            spdlog::info(
                "[ending] reaction notification: property={} localized={} "
                "statusSuffixRemoved={} fallback={}",
                a_name ? a_name : "<none>", resolved, sanitized, !resolved);
            return true;
        }

        // Apply the effects carried by SGT's reaction record straight to the
        // player. No inventory item is created, equipped, drunk, or removed.
        // The paired SGT message shown above is the player-facing
        // notification for these effects.
        bool ApplyReactionEffect(RE::BSScript::Object* a_obj,
                                 const char*           a_prop) {
            if (!a_obj || !a_prop) { return false; }
            auto* prop = a_obj->GetProperty(a_prop);
            if (!prop) { return false; }
            auto potObj = prop->GetObject();
            if (!potObj) { return false; }
            auto* potion = static_cast<RE::AlchemyItem*>(potObj->Resolve(
                static_cast<RE::VMTypeID>(RE::FormType::AlchemyItem)));
            if (!potion) { return false; }
            auto* pc = RE::PlayerCharacter::GetSingleton();
            auto* target = pc ? pc->AsMagicTarget() : nullptr;
            if (!target) { return false; }

            bool any = false, all = true;
            for (auto* effect : potion->effects) {
                if (!effect || !effect->baseEffect) { continue; }
                RE::MagicTarget::AddTargetData data{};
                data.caster        = pc;
                data.magicItem     = potion;
                data.effect        = effect;
                data.source        = potion;
                data.magnitude     = effect->GetMagnitude();
                data.unk40         = 1.0f;  // effect power/effectiveness
                data.castingSource = RE::MagicSystem::CastingSource::kInstant;
                const bool added   = target->AddTarget(data);
                any                = true;
                all                = all && added;
            }
            return any && all;
        }

        RE::TESQuest* AudienceQuest() {
            static RE::TESQuest* quest = [] {
                auto* dh = RE::TESDataHandler::GetSingleton();
                return dh ? dh->LookupForm<RE::TESQuest>(kAudienceQuestLocal,
                                                         kSgtPlugin)
                          : nullptr;
            }();
            return quest;
        }

        void LogAudienceStage() {
            if (auto* quest = AudienceQuest()) {
                spdlog::info("[sgt] audience stage={} running={}",
                             quest->GetCurrentStageID(), quest->IsRunning());
            }
        }

        // ---- always gather a crowd --------------------------------------
        //
        // SGT starts its audience by setting the quest to stage 10, and it
        // does that in only FOUR of its five expertise branches
        // (`_Talent_PlayInstrument.psc:174, 181, 195, 211`). The 46-65
        // "Medium Player" band at :183 has no SetStage call at all, so a
        // mid-rank bard performs to nobody - the one band where a player is
        // most likely to be practising.
        //
        // Nudging the same stage ourselves is exactly what SGT's own four
        // branches do, so the audience package, its scene and its teardown
        // (stage 20 -> 200 at :272-295) all run unchanged. Dispatched
        // through Papyrus rather than written to `currentStage` directly so
        // the stage fragments actually fire.
        bool          g_audienceNudged = false;  // game thread; per session
        int           g_audienceWarned = 0;

        void ResetAudienceNudge() { g_audienceNudged = false; }

        void EnsureAudienceGathers() {
            auto* quest = AudienceQuest();
            if (!quest) {
                if (g_audienceWarned++ == 0) {
                    spdlog::warn("[sgt] audience quest unresolved - crowd "
                                 "cannot be started for mid-rank bards");
                }
                return;
            }
            const auto stage = quest->GetCurrentStageID();
            const auto action = audience::Plan(stage);
            if (action == audience::Action::kNone) {
                g_audienceNudged = true;  // healthy live stage 10
                return;
            }
            if (action == audience::Action::kSetStage10 &&
                g_audienceNudged) {
                // The asynchronous SetStage is already queued and the VM has
                // not reflected it yet. Do not enqueue duplicates.
                return;
            }
            if (action == audience::Action::kResetAndSetStage10) {
                // A previous effect's OnEffectFinish can reach stage 20/200
                // after a fast new performance has already started. Leaving
                // that terminal quest in place produced a complete silent
                // song in the 15:39 field run.
                if (quest->IsRunning()) { quest->Stop(); }
                quest->Reset();
                g_audienceNudged = false;
                spdlog::info(
                    "[sgt] audience stale terminal stage {} reset for active "
                    "performance", stage);
            }
            auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
            if (!vm) { return; }
            auto*      policy = vm->GetObjectHandlePolicy();
            const auto handle = policy->GetHandleForObject(
                static_cast<RE::VMTypeID>(RE::FormType::Quest), quest);
            if (handle == policy->EmptyHandle()) { return; }
            RE::BSTSmartPointer<RE::BSScript::Object> obj;
            vm->FindBoundObject(handle, "Quest", obj);
            if (!obj) { return; }
            auto cb = Noop();
            if (vm->DispatchMethodCall1(
                    obj, "SetStage",
                    RE::MakeFunctionArguments(static_cast<std::int32_t>(10)),
                    cb)) {
                g_audienceNudged = true;
                if (action == audience::Action::kResetAndSetStage10) {
                    spdlog::info(
                        "[sgt] audience restarted at stage 10 after stale "
                        "teardown");
                } else {
                    spdlog::info(
                        "[sgt] audience: stage was {} - set to 10 so a crowd "
                        "gathers at this rank too", stage);
                }
            }
        }
    }

    void ResetAudience() { ResetAudienceNudge(); }

    PassResult KeeperPass(RE::FormID a_spellId, bool a_keepIdle,
                          bool a_keepFollower) {
        PassResult r;
        auto*      effect = FindPerformEffect(a_spellId);
        if (!effect) {
            g_stoppedInstance = 0;
            g_timerSuppressed = false;
            g_varWarned       = false;
            g_scriptMisses    = 0;
            g_scriptBoundLogged = false;
            g_idleVarWarned   = false;
            g_duetSeen        = kDuetUnknown;
            g_duetWarned      = false;
            g_duetResolveLogged = false;
            g_duetRestarts    = 0;
            g_duetCapLogged   = false;
            g_duetAnswer[0].store(-1);
            g_duetAnswer[1].store(-1);
            return r;
        }
        r.effectPresent = true;
        auto obj = ScriptObject(effect);
        if (!obj) {
            // Count, do not latch. Papyrus binds the script a few ms AFTER
            // AddSpell returns, so the pass that runs in the same
            // millisecond legitimately sees nothing - the 15:31 field run
            // found it 12ms later. A one-shot warning could not tell that
            // transient apart from a script that NEVER binds, which is the
            // invisible-lute case: a short session gets one pass, it fails,
            // and the idle is never re-upped. The count makes the
            // difference readable straight off the log.
            ++g_scriptMisses;
            spdlog::warn("[sgt] perform effect has no {} script (miss #{})",
                         kScriptClass, g_scriptMisses);
            return r;
        }
        if (g_scriptMisses > 0 && !g_scriptBoundLogged) {
            g_scriptBoundLogged = true;
            spdlog::info(
                "[sgt] perform script bound after {} missed pass(es) - the "
                "keeper is live", g_scriptMisses);
        }
        auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!vm) { return r; }

        // (1) kill the 32.8s RegisterForSingleUpdate exit. Idempotent and
        // re-sent every pass, so a registration that lands AFTER our first
        // pass (OnEffectStart registers ~3.5s+ in, later if a menu held it)
        // still dies long before it can fire.
        {
            auto cb = Noop();
            vm->DispatchMethodCall1(obj, "UnregisterForUpdate",
                                    RE::MakeFunctionArguments(), cb);
            if (!g_timerSuppressed) {
                g_timerSuppressed = true;
                spdlog::info("[sgt] 32.8s exit timer suppressed");
            }
        }

        // (2) stop SGT's own ~33s clip once its instance id exists (the
        // script fills SongToPlay ~2s after effect start; its SNDR category
        // is an SFX child our ducking does NOT cover - esp-verified). The
        // miss branches must NOT be silent: the field run has to be able to
        // tell "variable-offset bug" apart from "script just hasn't filled
        // it yet" (which logs nothing until the id shows up, by design).
        if (auto* var = FindVariable(obj.get(), "SongToPlay"); !var) {
            if (!g_varWarned) {
                g_varWarned = true;
                spdlog::warn("[sgt] SongToPlay variable not found (offset?)");
            }
        } else if (!var->IsInt()) {
            if (!g_varWarned) {
                g_varWarned = true;
                spdlog::warn("[sgt] SongToPlay resolved to non-int (offset?)");
            }
        } else {
            const auto inst = var->GetSInt();
            if (inst != 0 && inst != g_stoppedInstance) {
                auto  cb   = Noop();
                auto* args = RE::MakeFunctionArguments(
                    static_cast<std::int32_t>(inst));
                vm->DispatchStaticCall("Sound", "StopInstance", args, cb);
                g_stoppedInstance = inst;
                spdlog::info("[sgt] performance clip stopped (instance {})",
                             inst);
            }
        }
        r.clipStopped = g_stoppedInstance != 0;

        // (3) keep the play idle alive: SGT plays IdleToPlay exactly once;
        // if the behavior graph dropped it, re-send its anim event (what
        // Debug.SendAnimationEvent does - SGT itself uses that for the bow).
        // Cache SGT's exit idle while its script object is still reachable.
        // Done on every pass but only resolved once: the effect is dispelled
        // before we release the pose, so this is the last chance to read it.
        if (!g_stopIdleEvent.empty()) {
            // already cached
        } else if (auto* stopProp = obj->GetProperty("IdleStop_Loose");
                   stopProp) {
            if (auto stopObj = stopProp->GetObject()) {
                if (auto* stopIdle = static_cast<RE::TESIdleForm*>(
                        stopObj->Resolve(static_cast<RE::VMTypeID>(
                            RE::FormType::Idle)))) {
                    g_stopIdleEvent = stopIdle->animEventName;
                    spdlog::info("[sgt] stop idle cached ({})",
                                 g_stopIdleEvent.c_str());
                }
            }
        }
        if (a_keepIdle) {
            auto* pc     = RE::PlayerCharacter::GetSingleton();
            bool  idling = false;
            if (pc && !pc->GetGraphVariableBool("bIdlePlaying", idling)) {
                if (!g_idleVarWarned) {
                    g_idleVarWarned = true;
                    spdlog::warn("[sgt] bIdlePlaying graph var unreadable - "
                                 "idle keep-alive inert");
                }
            } else if (pc && !idling) {
                if (auto* prop = obj->GetProperty("IdleToPlay"); prop) {
                    if (auto idleObj = prop->GetObject()) {
                        if (auto* idle = static_cast<RE::TESIdleForm*>(
                                idleObj->Resolve(static_cast<RE::VMTypeID>(
                                    RE::FormType::Idle)))) {
                            pc->NotifyAnimationGraph(idle->animEventName);
                            spdlog::info("[sgt] idle re-upped ({})",
                                         idle->animEventName.c_str());
                        }
                    }
                }
            }
        }

        // (4) observe SGT's follower-duet scenes. SGT builds a follower duet
        // as two Scenes (_talent_LetsPlayTogether1/2, both .Stop()ed by its
        // own manual exit); the _Talent_FollowerPlays GLOB is consumed as the
        // clip starts, so the scenes are the only observable. CommonLibSSE-NG
        // BGSScene carries record data only (no runtime playing bit), so
        // playing state comes from vanilla Papyrus Scene.IsPlaying():
        // dispatched here each pass, answer stored by BoolResultCallback,
        // consumed at the top of the NEXT pass (one-pass delay is fine at
        // the 5s cadence). Duet scenes were built for ~35s performances -
        // multi-minute behavior is unknown, hence log-first; the re-start
        // path only runs under a_keepFollower (bFollowerKeepAlive, def off).
        // Confirmed solo (g_duetSeen 0): SGT consumes _Talent_FollowerPlays
        // at clip start, a follower can never join mid-song - stop polling.
        if (g_duetSeen != 0) {
            const int ans[2] = { g_duetAnswer[0].exchange(-1),
                                 g_duetAnswer[1].exchange(-1) };
            auto* s1 = ResolveScene(obj.get(), "_talent_LetsPlayTogether1");
            auto* s2 = ResolveScene(obj.get(), "_talent_LetsPlayTogether2");
            auto  sceneObj1 = SceneObject(s1);
            auto  sceneObj2 = SceneObject(s2);
            if (!g_duetResolveLogged) {
                g_duetResolveLogged = true;
                spdlog::info("[sgt] duet scenes resolved s1={} s2={}",
                             sceneObj1 ? "ok" : (s1 ? "unbound" : "null"),
                             sceneObj2 ? "ok" : (s2 ? "unbound" : "null"));
            }
            if (!sceneObj1 && !sceneObj2) {
                if (!g_duetWarned) {
                    g_duetWarned = true;
                    spdlog::warn("[sgt] duet scene state unreadable "
                                 "(no bound Scene objects)");
                }
            } else {
                // state machine on last pass's answers
                if (g_duetSeen == kDuetUnknown || g_duetSeen == 0) {
                    if (ans[0] == 1 || ans[1] == 1) {
                        g_duetSeen = (ans[0] == 1) ? 1 : 2;
                        spdlog::info("[sgt] duet scene {} playing "
                                     "(follower performing)",
                                     g_duetSeen);
                    } else if (ans[0] == 0 && ans[1] == 0) {
                        g_duetSeen = 0;  // solo performance, no log spam
                    }
                } else if (g_duetSeen > 0 && ans[g_duetSeen - 1] == 0) {
                    const int scene = g_duetSeen;
                    spdlog::info(
                        "[sgt] duet scene {} stopped mid-performance",
                        scene);
                    g_duetSeen = -scene;  // stopped sentinel, log fired
                    if (a_keepFollower) {
                        if (g_duetRestarts < 3) {
                            ++g_duetRestarts;
                            auto& target =
                                (scene == 1) ? sceneObj1 : sceneObj2;
                            if (target) {
                                auto cb = Noop();
                                vm->DispatchMethodCall1(
                                    target, "Start",
                                    RE::MakeFunctionArguments(), cb);
                                spdlog::info("[sgt] duet scene {} re-started "
                                             "(bFollowerKeepAlive)",
                                             scene);
                                // positive again so a later drop re-triggers
                                g_duetSeen = scene;
                            }
                        } else if (!g_duetCapLogged) {
                            g_duetCapLogged = true;
                            spdlog::warn("[sgt] duet re-start cap hit "
                                         "(3 per performance) - giving up");
                        }
                    }
                }
                // query for the next pass
                if (sceneObj1) {
                    RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>
                        cb(new BoolResultCallback(g_duetAnswer[0]));
                    vm->DispatchMethodCall1(sceneObj1, "IsPlaying",
                                            RE::MakeFunctionArguments(), cb);
                }
                if (sceneObj2) {
                    RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>
                        cb(new BoolResultCallback(g_duetAnswer[1]));
                    vm->DispatchMethodCall1(sceneObj2, "IsPlaying",
                                            RE::MakeFunctionArguments(), cb);
                }
            }
        }

        if (Settings::GetSingleton().alwaysGatherCrowd) {
            EnsureAudienceGathers();
        }
        LogAudienceStage();
        return r;
    }

    // Our own end-of-performance reaction, replacing SGT's MessageAndEXP.
    //
    // SGT decides the whole ending from RANK and a dice roll and never looks
    // at the playing: the ovation global is set unconditionally on its first
    // line, the message is a random draw inside an expertise band, and gold
    // is a flat RandomInt(1,10) for anyone at 66+ standing in an inn. Field
    // 2026-07-22: a 0%-accuracy run (2 of 286 notes) still drew cheering,
    // a positive notification and gold.
    //
    // We own the trigger - the payout is held until the results window
    // closes and dispatched by us - so this simply does not call
    // MessageAndEXP, and does the parts worth keeping itself, from the run:
    //
    //   ovation  ending::ShouldOvate    - a let-down room does not applaud
    //   message  ending::MessageFor     - SGT's own text, picked by stars
    //   gold     nothing at all         - the purse is payout::Deserved's
    //   XP       ending::PerformanceXp  - added to the feed at the call site
    //
    // Not calling it also retires the gold clawback: SGT pays nothing if it
    // is never asked to, so `observed` is 0 and TopUp grants the whole
    // purse instead of GoldScale having to take unearned coin back.
    //
    // Returns false only when SGT's script object cannot be reached, which
    // the caller treats exactly as before (the purse still pays).
    bool DispatchEnding(RE::FormID a_spellId, int a_stars, int a_expertise,
                        bool a_finishedGreat) {
        auto* effect = FindPerformEffect(a_spellId);
        if (!effect) { return false; }
        auto obj = ScriptObject(effect);
        if (!obj) { return false; }
        const auto&       se = Settings::GetSingleton();
        ending::Thresholds th{ se.reactionNeutralStars,
                               se.reactionPositiveStars };
        const auto valence =
            ending::ValenceForPerformance(a_stars, a_finishedGreat, th);
        const bool ovate = ending::ShouldOvate(valence);

        SetOvation(ovate);

        const auto tier = ending::TierFor(a_expertise);
        // Seeded from the clock, not std::rand()'s default state: an
        // unseeded rand() replays the same sequence every process start, so
        // the first performance after each launch would always draw the same
        // line out of a nine-entry pool.
        static std::mt19937 rng{ static_cast<std::uint32_t>(
            std::chrono::steady_clock::now().time_since_epoch().count()) };
        const char* name = ending::MessageFor(
            tier, valence, static_cast<int>(rng() & 0x7FFFu));
        const bool shown =
            name && ShowReactionMessage(obj.get(), name, valence);
        // The potion is what the message TEXT is describing. Applied only
        // when the line was actually shown, so the player is never given a
        // silent effect they were told nothing about, nor told about one
        // they never got.
        const char* effectProp = se.reactionEffects
                                     ? ending::EffectPropertyFor(tier, valence)
                                     : nullptr;
        const bool  applied =
            shown && effectProp && ApplyReactionEffect(obj.get(), effectProp);
        spdlog::info(
            "[ending] stars={} finalGloryGreat={} expertise={} tier={} "
            "valence={} ovation={} message={}{} effect={}{}",
            a_stars, a_finishedGreat, a_expertise, static_cast<int>(tier),
            static_cast<int>(valence), ovate, name ? name : "<none>",
            shown ? "" : " (NOT SHOWN - property missing)",
            effectProp ? effectProp : "<none>",
            (effectProp && !applied) ? " (NOT APPLIED)" : "");
        return true;
    }

    void ReleaseAudienceCelebration() {
        auto* quest = AudienceQuest();
        if (!quest) { return; }

        int actors = 0, idleStops = 0, resets = 0;
        for (auto* base : quest->aliases) {
            if (!base ||
                base->GetVMTypeID() != RE::BGSRefAlias::VMTYPEID) {
                continue;
            }
            const char* name = base->aliasName.c_str();
            if (!name) { continue; }
            const bool bystander = std::strncmp(name, "Bystander", 9) == 0;
            const bool follower = std::strcmp(name, "Player Follower") == 0;
            if (!bystander && !follower) { continue; }

            auto* alias = static_cast<RE::BGSRefAlias*>(base);
            auto* actor = alias->GetActorReference();
            if (!actor) { continue; }
            ++actors;
            // SGT itself uses IdleStop before asking an audience NPC to bow
            // (`_Talent_TakeABowNPC.psc`). The default-state event is a belt
            // for other AnimObject celebration idles; both are one-shot and
            // restricted to the quest's current audience aliases.
            if (actor->NotifyAnimationGraph("IdleStop")) { ++idleStops; }
            if (actor->NotifyAnimationGraph("IdleForceDefaultState")) {
                ++resets;
            }
        }
        spdlog::info(
            "[mood] audience celebration release: actors={} IdleStop={} "
            "defaultReset={}",
            actors, idleStops, resets);
    }

    bool DispatchAudienceFailureBark() {
        auto* quest = AudienceQuest();
        constexpr const char* kNegativeTopic = "Negative_Idle1";
        auto* topic =
            RE::TESForm::LookupByEditorID<RE::TESTopic>(kNegativeTopic);
        if (!topic) {
            if (auto* dh = RE::TESDataHandler::GetSingleton()) {
                // Parent DIAL of SGT's negative voice-bank INFO records.
                // Parsing the INFO type-7 group resolves this local ID.
                topic = dh->LookupForm<RE::TESTopic>(
                    0x0094D6, kSgtPlugin);
                if (topic) {
                    const char* edid = topic->GetFormEditorID();
                    if (edid &&
                        std::strcmp(edid, kNegativeTopic) != 0) {
                        spdlog::warn(
                            "[failure] SGT topic 0x0094D6 is \"{}\", not "
                            "\"{}\"; ignoring version-specific lookup",
                            edid, kNegativeTopic);
                        topic = nullptr;
                    }
                }
                // The editor-ID map is incomplete on some runtimes/load
                // orders. DIAL forms remain in the typed data array.
                for (auto* candidate :
                     dh->GetFormArray<RE::TESTopic>()) {
                    if (topic) { break; }
                    if (!candidate) { continue; }
                    const char* edid = candidate->GetFormEditorID();
                    if (edid &&
                        std::strcmp(edid, kNegativeTopic) == 0) {
                        topic = candidate;
                        break;
                    }
                }
            }
        }
        auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!quest || !topic || !vm) {
            spdlog::warn(
                "[failure] audience bark unavailable: quest={} topic={} vm={}",
                quest != nullptr, topic != nullptr, vm != nullptr);
            return false;
        }

        auto* policy = vm->GetObjectHandlePolicy();
        if (!policy) { return false; }

        // Prefer a gathered bystander over the follower. The follower is a
        // useful fallback in a sparse venue, but a stranger heckling the
        // failed performer reads more naturally and avoids making a loyal
        // companion feel arbitrarily hostile.
        for (int pass = 0; pass < 2; ++pass) {
            for (auto* base : quest->aliases) {
                if (!base ||
                    base->GetVMTypeID() != RE::BGSRefAlias::VMTYPEID) {
                    continue;
                }
                const char* name = base->aliasName.c_str();
                if (!name) { continue; }
                const bool bystander =
                    std::strncmp(name, "Bystander", 9) == 0;
                const bool follower =
                    std::strcmp(name, "Player Follower") == 0;
                if ((pass == 0 && !bystander) ||
                    (pass == 1 && !follower)) {
                    continue;
                }

                auto* alias = static_cast<RE::BGSRefAlias*>(base);
                auto* actor = alias->GetActorReference();
                if (!actor) { continue; }
                const auto handle = policy->GetHandleForObject(
                    RE::FormType::ActorCharacter, actor);
                if (handle == policy->EmptyHandle()) { continue; }

                auto cb = Noop();
                const bool queued = vm->DispatchMethodCall2(
                    handle, "ObjectReference", "Say",
                    RE::MakeFunctionArguments(
                        static_cast<RE::TESTopic*>(topic),
                        static_cast<RE::Actor*>(actor), false),
                    cb);
                spdlog::info(
                    "[failure] audience bark queued={} actor={:08X} "
                    "alias=\"{}\" topic={:08X}",
                    queued, actor->GetFormID(), name, topic->GetFormID());
                if (queued) { return true; }
            }
        }

        spdlog::warn(
            "[failure] audience bark not queued: no live bystander/follower "
            "alias");
        return false;
    }

    void StopAudienceWithoutApplause() {
        // Release current graph state first; stage 200 then stops the scene
        // and its packages so they cannot immediately re-issue applause.
        ReleaseAudienceCelebration();
        auto* quest = AudienceQuest();
        if (!quest) { return; }
        const int from = quest->GetCurrentStageID();
        const int to = ending::AudienceTerminalStageForFailure(from);
        if (to == from) {
            spdlog::info(
                "[audience] no-applause terminal stage unchanged: stage={} "
                "running={}",
                from, quest->IsRunning());
            return;
        }
        auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!vm) { return; }
        auto* policy = vm->GetObjectHandlePolicy();
        const auto handle = policy->GetHandleForObject(
            static_cast<RE::VMTypeID>(RE::FormType::Quest), quest);
        if (handle == policy->EmptyHandle()) { return; }
        RE::BSTSmartPointer<RE::BSScript::Object> obj;
        vm->FindBoundObject(handle, "Quest", obj);
        if (!obj) { return; }
        auto cb = Noop();
        const bool queued = vm->DispatchMethodCall1(
            obj, "SetStage",
            RE::MakeFunctionArguments(static_cast<std::int32_t>(to)), cb);
        // Stage 200's own fragment calls Stop(). Do the same synchronously so
        // the current scene/packages release this frame; the queued stage
        // remains the durable state transition before spell cleanup runs.
        if (quest->IsRunning()) { quest->Stop(); }
        spdlog::info(
            "[audience] no-applause hard stop: stage {} -> {} queued={} "
            "runningAfterStop={} (stage 20 applause bypassed)",
            from, to, queued, quest->IsRunning());
    }

    bool DispatchPayout(RE::FormID a_spellId) {
        auto* effect = FindPerformEffect(a_spellId);
        if (!effect) { return false; }
        auto obj = ScriptObject(effect);
        if (!obj) { return false; }
        auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!vm) { return false; }
        // Blanking SGT's reaction Message properties (added field round 5,
        // 961dffe) is OFF by default now - it was the post-session control
        // lock. The 2026-07-21 bisect convicted 961dffe as the first bad
        // commit against a good parent, and this is its end-of-song half:
        // "Show() on None no-ops" was wrong. A call on a None object is a
        // Papyrus error, so MessageAndEXP dies partway through and whatever
        // it does after the reaction - including handing the player back
        // their controls - never happens. Nothing restores the properties
        // either, which is why a save-reload could not clear the lock but a
        // process restart could.
        //
        // The collision this was invented for (a modal box fighting our
        // results window) is already solved a different way: the payout is
        // HELD until the results window closes, so the box has the screen
        // to itself by the time it appears.
        //
        // 1 restores the old behaviour for A/B only. That path is
        // deliberately not symmetric - it never puts the properties back.
        if (!Settings::GetSingleton().blankReactionMessages) {
            spdlog::info("[sgt] payout: reaction messages left INTACT "
                         "(bBlankReactionMessages=0)");
            auto cbIntact = Noop();
            return vm->DispatchMethodCall1(obj, "MessageAndEXP",
                                           RE::MakeFunctionArguments(),
                                           cbIntact);
        }
        static constexpr const char* kMessages[] = {
            "_Message_Standing", "_Talent_Clueless_Broken",
            "_Talent_Clueless_Broken1", "_Talent_Clueless_Broken2",
            "_Talent_Clueless_Broken3", "_Talent_Clueless_OK",
            "_Talent_Clueless_OK1", "_Talent_Clueless_OK2",
            "_Talent_Clueless_OK3", "_Talent_Clueless_OK4",
            "_Talent_Clueless_OK5", "_Talent_Clueless_OK6",
            "_Talent_Clueless_OK7", "_Talent_Clueless_OK8",
            "_Talent_Clueless_Negative", "_Talent_BardLevel_Positive",
            "_Talent_BardLevel_OK", "_Talent_BardLevel_OK1",
            "_Talent_BardLevel_OK2", "_Talent_Beginner_Negative",
            "_Talent_Beginner_OK", "_Talent_Beginner_OK1",
            "_Talent_Decent_OK", "_Talent_Decent_OK2",
            "_Talent_Decent_OK3", "_Talent_Pro_Positive",
            "_Talent_Pro_OK", "_Talent_Pro_OK1", "_Talent_Pro_OK2",
            "_Talent_Pro_OK3",
        };
        int blanked = 0;
        for (const char* name : kMessages) {
            if (auto* prop = obj->GetProperty(name)) {
                *prop = RE::BSScript::Variable{};  // None
                ++blanked;
            }
        }
        spdlog::info(
            "[sgt] payout: {} reaction message properties blanked "
            "(results box is the end-of-song UI)",
            blanked);
        auto cb = Noop();
        return vm->DispatchMethodCall1(obj, "MessageAndEXP",
                                       RE::MakeFunctionArguments(), cb);
    }

    namespace {
        // Browse CANCEL, instant control return (design 2026-07-20).
        //
        // Old behaviour: control came back only when the settle-then-strip
        // finished - SGT's clip had to fill SongToPlay (~2-3s after the
        // unfreeze) plus two linger passes, so the player stood rooted for
        // 3-4s after pressing cancel.
        //
        // The obvious "just dispatch EnablePlayerControls at the cancel
        // edge" LOSES A RACE, because the world-freeze also froze the
        // Papyrus VM: at cancel time SGT's OnEffectStart has usually not
        // run at all yet, so its DisablePlayerControls lands AFTER our
        // enable and the player is rooted anyway. So we act every pass
        // instead of once.
        //
        // ⚠ FIELD 2026-07-20 13:32 + user report: the FIRST version of this
        // gated on `unk11C != 0` and consequently NEVER FIRED - the mask
        // read ZERO on every pass while the player was demonstrably unable
        // to walk for ~3s (they also see an idle-animation stutter in that
        // window). So unk11C is NOT the layer holding them during a browse
        // cancel, whatever it was for the end-of-song lock. The predicate
        // was wrong, not the timing.
        //
        // Rather than guess a third time: dispatch UNCONDITIONALLY while
        // armed (bounded - the window ends at the strip, ~6 passes), and
        // probe every candidate layer each pass so the next log names the
        // real one. If control returns instantly now, ControlMap was the
        // layer and the gate was the whole bug. If it still does not, the
        // lock is the animation graph and the probe will show controls
        // clean throughout - which is the evidence Phase 3 needs.
        void ReleaseCancelControls() {
            auto* pc  = RE::PlayerCharacter::GetSingleton();
            auto* cam = RE::PlayerCamera::GetSingleton();
            bool  idlePlaying = false;
            if (pc) {
                // SGT's own play-idle marker; false does NOT prove no idle
                // (field round 4: it reads false while one visibly plays)
                pc->GetGraphVariableBool("bIdlePlaying", idlePlaying);
            }
            spdlog::info(
                // The control-mask fields that used to lead this line were
                // removed 2026-07-22: on AE they read the input context
                // stack's size, not controls (see MovementGuard.h), so every
                // "controls=/unk11C=/move=/look=" ever logged here was
                // measuring the wrong thing.
                "[sgt] browse cancel probe: sitSleep={} cam={} "
                "bIdlePlaying={} weaponDrawn={}",
                pc ? static_cast<int>(pc->AsActorState()->GetSitSleepState())
                   : -1,
                cam && cam->currentState
                    ? static_cast<int>(cam->currentState->id)
                    : -1,
                idlePlaying,
                pc ? pc->AsActorState()->IsWeaponDrawn() : false);
            if (!g_ssFreeLogged) {
                g_ssFreeLogged = true;
                spdlog::info(
                    "[sgt] browse cancel: releasing controls every pass "
                    "until the strip (unconditional - the unk11C gate was "
                    "provably wrong)");
            }
            EnablePlayerControlsFallback();
        }
    }

    int StandstillPass(RE::FormID a_spellId, bool a_strip) {
        auto* effect = FindPerformEffect(a_spellId);
        if (!effect) { return 0; }
        auto  obj = ScriptObject(effect);
        auto* vm  = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (obj && vm) {
            // every pass, idempotent - a 32.8s registration that lands at
            // ANY point during the settle window dies within 500ms
            auto cb = Noop();
            vm->DispatchMethodCall1(obj, "UnregisterForUpdate",
                                    RE::MakeFunctionArguments(), cb);
        }
        // cancelled browse: give the controls back the moment SGT's start
        // thread takes them, rather than 3-4s later at the strip
        if (g_ssFreeArmed.load()) { ReleaseCancelControls(); }
        if (a_strip) {
            g_ssFreeArmed.store(false);
            auto* pc = RE::PlayerCharacter::GetSingleton();
            auto* sp = RE::TESForm::LookupByID<RE::SpellItem>(a_spellId);
            if (pc && sp) {
                pc->RemoveSpell(sp);
                spdlog::info(
                    "[sgt] standstill: performance stripped (browse) - "
                    "SGT's own cleanup runs");
                // instant control return on cancel too - and insurance
                // against the cleanup's latent death (see the header)
                EnablePlayerControlsFallback();
            }
            return 2;
        }
        if (!obj || !vm) { return 1; }
        // Kill the play ANIMATION before it exists (field round 5: the
        // per-pass IdleForceDefaultState spam visibly jittered a standing
        // player). SGT's PlayMusic reads IdleToPlay ~1s+ into its start
        // thread; blanking the property on THIS script instance means
        // PlayIdle(None) - nothing plays, nothing to cancel. IdleStop_Loose
        // is blanked too so the strip's OnEffectFinish put-away is a no-op
        // on the standing player. CANCEL-ONLY (field 2026-07-20): under the
        // world freeze a pick CONTINUES this very instance, and a blanked
        // instance killed the session's play idle, the keeper re-up and
        // the end-cleanup's IdleStop (invisible lute + graph control lock)
        // - the session thread disarms via g_ssBlankArmed at pick, and the
        // originals are saved for RestoreIdleProperties belt-and-braces.
        if (!g_ssIdleNeutered && g_ssBlankArmed.load()) {
            g_ssIdleNeutered = true;
            int blanked = 0;
            const char* names[2] = { "IdleToPlay", "IdleStop_Loose" };
            for (int i = 0; i < 2; ++i) {
                if (auto* prop = obj->GetProperty(names[i])) {
                    g_ssSavedIdle[i] = *prop;
                    g_ssSaved        = true;
                    *prop = RE::BSScript::Variable{};  // None
                    ++blanked;
                }
            }
            spdlog::info(
                "[sgt] standstill: idle properties blanked ({}/2)",
                blanked);
        }
        if (auto* var = FindVariable(obj.get(), "SongToPlay");
            var && var->IsInt()) {
            const auto inst = var->GetSInt();
            if (inst != 0) {
                if (inst != g_ssStopped) {
                    auto  cb   = Noop();
                    auto* args = RE::MakeFunctionArguments(
                        static_cast<std::int32_t>(inst));
                    vm->DispatchStaticCall("Sound", "StopInstance", args, cb);
                    g_ssStopped = inst;
                    spdlog::info(
                        "[sgt] standstill: SGT clip stopped (instance {})",
                        inst);
                    // one-shot fallback: if the property blank lost the
                    // race (PlayIdle read it first), the idle is playing
                    // now - a single cancel, never a per-pass spam
                    if (!g_ssCancelSent) {
                        g_ssCancelSent = true;
                        if (auto* pc =
                                RE::PlayerCharacter::GetSingleton()) {
                            pc->NotifyAnimationGraph(
                                "IdleForceDefaultState");
                        }
                    }
                }
                return 2;
            }
        }
        return 1;
    }

    void ForceEnableMovementGlobal() {
        if (!g_moveGlob) {
            auto* dh = RE::TESDataHandler::GetSingleton();
            g_moveGlob =
                dh ? dh->LookupForm<RE::TESGlobal>(kEnableMovementLocal,
                                                   kSgtPlugin)
                   : nullptr;
        }
        if (!g_moveGlob) {
            if (!g_moveGlobWarned) {
                g_moveGlobWarned = true;
                spdlog::warn(
                    "[sgt] _Talent_EnableMovement global not found - SGT may "
                    "take movement and hide the instrument");
            }
            return;
        }
        if (g_moveOrig < 0.0f) { g_moveOrig = g_moveGlob->value; }
        if (g_moveGlob->value != 1.0f) {
            g_moveGlob->value = 1.0f;
            spdlog::info(
                "[sgt] _Talent_EnableMovement {} -> 1 for the performance "
                "(SGT then leaves movement alone and adds the instrument "
                "prop); original restored at session end",
                g_moveOrig);
        }
    }

    void RestoreEnableMovementGlobal() {
        if (!g_moveGlob || g_moveOrig < 0.0f) { return; }
        g_moveGlob->value = g_moveOrig;
        spdlog::info("[sgt] _Talent_EnableMovement restored to {}", g_moveOrig);
        g_moveOrig = -1.0f;
    }

    void SetGuitarAnimObjectOverride(bool a_enabled) {
        if (!g_luteAnimObject) {
            g_luteAnimObject =
                RE::TESForm::LookupByID<RE::TESObjectANIO>(
                    kLuteAnimObjectId);
        }
        if (!g_luteAnimObject) {
            if (!g_luteAnimObjectWarned) {
                g_luteAnimObjectWarned = true;
                spdlog::warn(
                    "[guitar] vanilla AnimObjectLute 0x{:08X} not found; "
                    "the performance will retain its lute prop",
                    kLuteAnimObjectId);
            }
            return;
        }

        if (a_enabled) {
            if (g_guitarAnimOverride) { return; }
            if (!g_luteAnimModelSaved) {
                const char* original = g_luteAnimObject->GetModel();
                g_luteAnimOriginalModel =
                    original ? original : "";
                g_luteAnimModelSaved = true;
            }
            g_luteAnimObject->SetModel(
                guitarprop::kAnimationObjectModel.data());
            // BEFORE SGT's PlayIdle fires (same guarantee as the model
            // swap at this seam): arm the OAR player-clip condition
            WriteElectricPerformGlobal(1.0f);
            g_guitarAnimOverride = true;
            spdlog::info(
                "[guitar] AnimObjectLute model redirected to {}",
                guitarprop::kAnimationObjectModel);
            return;
        }

        // Clear even when no override is active: an idempotent 0-write
        // costs nothing and this disable path runs at the start of every
        // NON-guitar performance - exactly where a stale 1.0 would leak
        // the electric clip into a lute set.
        WriteElectricPerformGlobal(0.0f);
        if (!g_guitarAnimOverride) { return; }
        g_luteAnimObject->SetModel(g_luteAnimOriginalModel.c_str());
        g_guitarAnimOverride = false;
        spdlog::info(
            "[guitar] AnimObjectLute model restored to {}",
            g_luteAnimOriginalModel);
    }

    void ReassertAnimObjectBaseline() {
        if (!g_luteAnimObject) {
            g_luteAnimObject =
                RE::TESForm::LookupByID<RE::TESObjectANIO>(
                    kLuteAnimObjectId);
        }
        if (!g_luteAnimObject) { return; }
        // The band never arms the override and never saves the original,
        // so a lute-session band run reaches here with nothing saved; the
        // vanilla path is a known constant, same pattern as BandStage's
        // drink-potion restore.
        const char* model =
            g_guitarAnimOverride ? guitarprop::kAnimationObjectModel.data()
            : g_luteAnimModelSaved ? g_luteAnimOriginalModel.c_str()
                                   : "Meshes\\AnimObjects\\AnimObjectLute.nif";
        g_luteAnimObject->SetModel(model);
        spdlog::info("[band] AnimObjectLute baseline reasserted ({})",
                     model);
    }

    void ClearElectricPerformGlobal() {
        WriteElectricPerformGlobal(0.0f);
    }

    void ReleasePerformPose() {
        // The exit idle still targets the same graph regardless of prop
        // model. Restore the shared ANIO first so no later vanilla/SGT lute
        // can inherit the guitar after this performance ends.
        SetGuitarAnimObjectOverride(false);
        auto* pc = RE::PlayerCharacter::GetSingleton();
        if (!pc) { return; }
        // Field 2026-07-20 (evidence 2026-07-20-moveglobal-fix-failed.log):
        // ControlMap fully restored (probe move=true), player still could
        // not walk but COULD jump. That is the animation graph holding the
        // lute idle: SGT's OnEffectFinish would play IdleStop_Loose to
        // release the pose, but its cleanup dies on the dispelled effect
        // (papyrus-dispel-inflight-threads), so nothing ever ends the idle.
        // Jump fires as a graph event from inside an idle; walking needs
        // the idle to end. Send the engine's own idle-reset event once.
        // (The standstill once spammed this per-pass and visibly jittered
        // the player - proof the event lands; ONE shot is deliberate.)
        // Send SGT's OWN exit event first (psc:280 PlayIdle(IdleStop_Loose)),
        // cached during the session by the keeper. This is the mirror of what
        // the keeper already does for IdleToPlay, and it is the piece that
        // was missing: IdleForceDefaultState is ACCEPTED by the graph every
        // single time and never ends the lute idle - field 2026-07-20, the
        // player kept jumping (a graph event from inside the idle) while
        // unable to walk (which needs the idle to actually end).
        if (!g_stopIdleEvent.empty()) {
            const bool stopped = pc->NotifyAnimationGraph(g_stopIdleEvent);
            spdlog::info("[sgt] perform pose released ({} accepted={})",
                         g_stopIdleEvent.c_str(), stopped);
        } else {
            spdlog::warn(
                "[sgt] stop idle never cached - falling back to "
                "IdleForceDefaultState, which has never actually worked");
        }
        // ...and the engine reset as a belt-and-braces second shot. Harmless
        // once the pose is already out, and still the only lever if SGT's
        // property could not be read.
        const bool ok = pc->NotifyAnimationGraph("IdleForceDefaultState");
        spdlog::info("[sgt] idle reset (IdleForceDefaultState accepted={})",
                     ok);
    }

    void EnablePlayerControlsFallback() {
        // The performance is over however it ended, so hand SGT's movement
        // global back. Idempotent and a no-op when we never changed it (a
        // browse cancel never reaches the pick that sets it), so it is safe
        // on every removal path and on the retry passes.
        RestoreEnableMovementGlobal();

        // NOTHING ELSE HAPPENS HERE, AND NOTHING ELSE MAY BE ADDED.
        //
        // This used to dispatch Game.EnablePlayerControls and then "restore"
        // ControlMap by hand, off a baseline snapshot. It was removed
        // 2026-07-22 along with the setting that gated it: on AE 1.6.1170
        // the members it wrote are not the control masks at all - see
        // MovementGuard.h - so it was corrupting the input context stack in
        // the name of repairing it. The lock it existed to paper over was
        // that same corruption, coming from MovementGuard.
        //
        // If a session ever again ends with the player stuck, do NOT write
        // engine control state from here. Find what took the controls.
    }

    void ResetStandstillLatch() {
        g_ssStopped      = 0;
        g_ssIdleNeutered = false;
        g_ssCancelSent   = false;
        g_ssSaved        = false;
        g_ssFreeLogged   = false;
        g_ssBlankArmed.store(true);
        g_ssFreeArmed.store(false);
    }

    void DisarmStandstillBlank() { g_ssBlankArmed.store(false); }

    void ArmCancelControlRelease() { g_ssFreeArmed.store(true); }

    void RestoreIdleProperties(RE::FormID a_spellId) {
        if (!g_ssIdleNeutered || !g_ssSaved) { return; }
        auto* effect = FindPerformEffect(a_spellId);
        if (!effect) { return; }
        auto obj = ScriptObject(effect);
        if (!obj) { return; }
        int restored = 0;
        const char* names[2] = { "IdleToPlay", "IdleStop_Loose" };
        for (int i = 0; i < 2; ++i) {
            if (auto* prop = obj->GetProperty(names[i])) {
                *prop = g_ssSavedIdle[i];
                ++restored;
            }
        }
        g_ssIdleNeutered = false;
        spdlog::info(
            "[sgt] idle properties restored ({}/2) - session continues "
            "the trigger's instance",
            restored);
    }

    // Game thread (SKSE task), like everything else here: the latches are
    // only ever touched from keeper/payout passes on that thread.
    void ResetPerformanceLatches() {
        g_stoppedInstance = 0;
        g_timerSuppressed = false;
        g_varWarned       = false;
        g_scriptMisses    = 0;
        g_scriptBoundLogged = false;
        g_idleVarWarned   = false;
        g_duetSeen        = kDuetUnknown;
        g_duetWarned      = false;
        g_duetResolveLogged = false;
        g_duetRestarts    = 0;
        g_duetCapLogged   = false;
        g_duetAnswer[0].store(-1);
        g_duetAnswer[1].store(-1);
    }

}
