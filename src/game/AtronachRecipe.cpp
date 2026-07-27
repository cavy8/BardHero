// src/game/AtronachRecipe.cpp
#include "PCH.h"
#include "game/AtronachRecipe.h"

namespace SH::AtronachRecipe {
    namespace {
        constexpr auto kSkyrimEsm = "Skyrim.esm";
        constexpr auto kAddonEsp  = "Bard Hero - Doom Lute.esp";

        // ---- HOW THE ATRONACH FORGE ACTUALLY WORKS ---------------------
        //
        // NOT ConstructibleObject. The forge is Papyrus-driven off two
        // PARALLEL FormLists: entry i of the recipe list is a FormList of
        // ingredients, and entry i of the result list is what those
        // ingredients produce. Read out of Skyrim.esm rather than guessed -
        // `AtrFrg15VoidSalts` sits at index 15 of both, which is what
        // confirms the pairing.
        //
        // ---- WHY THIS IS DONE AT RUNTIME AND NOT IN THE ESP ------------
        //
        // Because overriding those two lists in a plugin would be a silent
        // conflict. Three plugins in this very load order already override
        // both - `FormList-Patch-Collection_ITMs.esp`,
        // `mihailaegisofthesigil.esp` and `mihailpossesseddaedricarmors.esp`
        // - so an override built from the VANILLA contents would drop every
        // recipe those mods add, and one of them exists precisely to
        // reconcile FormList conflicts. Appending to whatever copy actually
        // won the conflict cannot lose anyone's entries, needs no new
        // masters, and works regardless of load order.
        //
        // Runtime appends do not persist into a save; this simply runs again
        // on the next game start.
        constexpr RE::FormID kRecipeListForm = 0x000CDE01;  // AtrFrg...RecipeList
        constexpr RE::FormID kResultListForm = 0x000CDE02;  // AtrFrg...ResultList
        constexpr RE::FormID kOurRecipeLocal = 0x00080D;    // our ingredients
        constexpr RE::FormID kDoomLuteLocal  = 0x000800;    // the result
    }

    void Install() {
        auto* data = RE::TESDataHandler::GetSingleton();
        if (!data) { return; }

        // The Electric addon is OPTIONAL. No addon, no Doom Lute, no recipe.
        auto* result = data->LookupForm<RE::TESObjectMISC>(kDoomLuteLocal,
                                                          kAddonEsp);
        auto* recipe = data->LookupForm<RE::BGSListForm>(kOurRecipeLocal,
                                                        kAddonEsp);
        if (!result || !recipe) {
            spdlog::info(
                "[atronach] Electric addon not present; Doom Lute recipe "
                "not registered");
            return;
        }

        auto* recipes = data->LookupForm<RE::BGSListForm>(kRecipeListForm,
                                                         kSkyrimEsm);
        auto* results = data->LookupForm<RE::BGSListForm>(kResultListForm,
                                                         kSkyrimEsm);
        if (!recipes || !results) {
            spdlog::warn("[atronach] forge lists missing; recipe skipped");
            return;
        }

        // Idempotent: kDataLoaded fires once per process, but a re-entrant
        // call must not queue the recipe twice and desynchronise the pair.
        if (recipes->HasForm(recipe)) {
            spdlog::info("[atronach] Doom Lute recipe already registered");
            return;
        }

        // ⚠ THE PAIRING IS BY INDEX, so the two lists MUST be the same
        // length before we touch them. If some other mod has appended to one
        // without the other, they are already misaligned and adding ours
        // would pair our ingredients with someone else's result - better to
        // do nothing and say so loudly than to hand the player a Daedric
        // Warhammer for five instruments.
        const auto before = recipes->forms.size();
        if (before != results->forms.size()) {
            spdlog::error(
                "[atronach] forge lists are MISALIGNED ({} recipes vs {} "
                "results) - refusing to add the Doom Lute recipe, because "
                "index pairing is how the forge matches them",
                before, results->forms.size());
            return;
        }

        recipes->AddForm(recipe);
        results->AddForm(result);
        spdlog::info(
            "[atronach] Doom Lute recipe registered at index {} "
            "({} ingredients; lists now {}/{})",
            before, recipe->forms.size(), recipes->forms.size(),
            results->forms.size());
    }

    namespace {
        // One lookup path for both the availability check and the cheat, so
        // they can never disagree about whether the addon is present.
        RE::TESObjectMISC* FindDoomLute() {
            auto* data = RE::TESDataHandler::GetSingleton();
            if (!data) { return nullptr; }
            return data->LookupForm<RE::TESObjectMISC>(kDoomLuteLocal,
                                                      kAddonEsp);
        }
    }

    bool DoomLuteAvailable() { return FindDoomLute() != nullptr; }

    void GiveDoomLuteToPlayer() {
        auto* lute = FindDoomLute();
        if (!lute) {
            spdlog::info(
                "[atronach] give-lute ignored: the addon is not installed");
            return;
        }

        // The settings panel draws on the render thread. Handing the player's
        // inventory a new item from there is not ours to do on that thread,
        // so it goes through the task queue exactly as GoldScale's payouts
        // do. The captured pointer is a TESDataHandler form: it outlives the
        // task for the life of the process.
        SKSE::GetTaskInterface()->AddTask([lute] {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) { return; }
            player->AddObjectToContainer(lute, nullptr, 1, nullptr);
            spdlog::info("[atronach] Doom Lute added to the player's "
                         "inventory (cheat)");
        });
    }
}
