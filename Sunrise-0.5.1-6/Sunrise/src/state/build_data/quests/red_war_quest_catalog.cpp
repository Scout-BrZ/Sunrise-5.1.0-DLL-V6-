#include "red_war_quest_catalog.h"
#include <Windows.h>
#include <utility>
#include "../../unlocks/definition.h"

namespace sunrise::state::build_data::quests {
namespace {
SRWLOCK g_lock = SRWLOCK_INIT;
std::vector<Step> g_steps;
std::vector<CompletionObjective> g_objectives;
// A ordem da etapa e ordinal; o valor nativo pode ser um hash negativo.
const Step* held_step(const CharacterState& c) noexcept {
    if (c.inventory.count > c.inventory.values.size()) { return nullptr; }
    const Step* selected = nullptr;
    for (const auto& step : g_steps) {
        for (std::size_t i = 0; i < c.inventory.count; ++i) {
            const auto& item = c.inventory.values[i];
            if (item.quantity > 0 && item.definitionHash == step.itemHash
                && (selected == nullptr || step.ordinal > selected->ordinal)) {
                selected = &step;
            }
        }
    }
    return selected;
}
void project(const Step& step, std::span<std::int32_t> values) noexcept {
    // Preserva a conclusao terminal ja persistida; nao reabre a jornada.
    // O inventario apenas preenche um acompanhamento ausente em saves antigos.
    // Nunca substitui um valor ja persistido, mesmo se houver um item antigo no inventario.
    if (step.row < values.size() && values[step.row] == 0) {
        values[step.row] = step.value;
    }
}
} // namespace
// Consulta apenas o catalogo; esta funcao nao altera inventario nem banco de dados.
bool transition(std::uint32_t hash, Step& current, Step& next, bool& terminal) noexcept {
    current = {}; next = {}; terminal = false;
    // Protege a leitura contra uma publicacao simultanea de um novo catalogo.
    AcquireSRWLockShared(&g_lock);
    bool found = false, successor = false, later = false;
    for (const auto& step : g_steps) if (step.itemHash == hash) { current = step; found = true; }
    // A ordem e dada por ordinal. Os valores nativos nao precisam ser consecutivos.
    if (found) for (const auto& step : g_steps) {
        later = later || step.ordinal > current.ordinal;
        if (step.ordinal == current.ordinal + 1) { next = step; successor = true; }
    }
    // Uma lacuna entre ordinais nao significa conclusao: so e terminal se nao houver etapa posterior.
    terminal = found && !later;
    ReleaseSRWLockShared(&g_lock);
    return found && (successor || terminal);
}
bool step_for_value(std::int32_t value, Step& output) noexcept {
    output = {};
    AcquireSRWLockShared(&g_lock);
    std::size_t matches{};
    for (const auto& step : g_steps) if (step.value == value) { output = step; ++matches; }
    ReleaseSRWLockShared(&g_lock);
    return matches == 1; // Recusa valores ausentes ou ambiguos, em vez de escolher uma etapa.
}
bool ready() noexcept {
    AcquireSRWLockShared(&g_lock);
    const bool result = !g_steps.empty();
    ReleaseSRWLockShared(&g_lock);
    return result;
}
bool publish(std::vector<Step> steps, std::vector<CompletionObjective> objectives) noexcept {
    if (steps.empty() || steps.size() > 256) { return false; }
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const auto& s = steps[i];
        if (s.itemHash == 0 || s.itemHash == 0xFFFFFFFFU || s.value == 0 || s.value == -1
            || s.bank > 1 || s.row >= (s.bank == 0 ? unlocks::kObjectiveValueCapacity
                                                  : unlocks::kCharacterObjectValueCapacity)
            || s.bank != steps[0].bank || s.row != steps[0].row) { return false; }
        for (std::size_t j = 0; j < i; ++j) {
            if (steps[j].itemHash == s.itemHash || steps[j].ordinal == s.ordinal) { return false; }
        }
    }
    // Os quatro primeiros membros tem conclusao automatica: Homecoming,
    // Adieu e a cinematica de chegada a Chacara, que leva a Tyra Karn.
    if (objectives.size() != 4) { return false; }
    for (std::size_t i = 0; i < objectives.size(); ++i) {
        const auto& o = objectives[i];
        if (o.ordinal != i || o.threshold <= 0 || o.bank > 2
            || (o.bank < 2 && o.row >= (o.bank == 0 ? unlocks::kObjectiveValueCapacity
                                                                 : unlocks::kCharacterObjectValueCapacity))
            || (o.bank == 2 && (o.slot >= 15500 || o.row != UINT16_MAX))) { return false; }
        bool member{};
        for (const auto& step : steps) {
            member = member || (step.ordinal == o.ordinal && step.itemHash == o.itemHash);
            // Um contador de objetivo nao pode sobrescrever o acompanhamento da jornada.
            if (step.bank == o.bank && step.row == o.row) { return false; }
        }
        if (!member || (i != 0 && ((o.bank < 2 && o.bank == objectives[0].bank && o.row == objectives[0].row)
                              || o.slot == objectives[0].slot))) {
            return false;
        }
    }
    AcquireSRWLockExclusive(&g_lock);
    g_steps = std::move(steps);
    g_objectives = std::move(objectives);
    ReleaseSRWLockExclusive(&g_lock);
    return true;
}
bool completion_objective(std::string_view package, CompletionObjective& output) noexcept {
    output = {};
    // Os textos nativos confirmam: 0 e 1 sao Homecoming; 2 e Adieu;
    // 3 e a etapa da Chacara, concluida pela cinematica cine_farm_325.
    // O objetivo da missao Homecoming pertence ao membro 1, nao ao membro 0.
    const int ordinal = package == "mission_towerfall" ? 1
        : package == "mission_journey" ? 2
        : package == "cine_farm_325" ? 3 : -1;
    if (ordinal < 0) { return false; }
    AcquireSRWLockShared(&g_lock);
    const bool found = g_objectives.size() == 4;
    if (found) { output = g_objectives[static_cast<std::size_t>(ordinal)]; }
    ReleaseSRWLockShared(&g_lock);
    return found;
}
bool project_character(const CharacterState& c, std::span<std::int32_t> values) noexcept {
    AcquireSRWLockShared(&g_lock);
    const Step* step = held_step(c);
    const bool active = step != nullptr;
    if (step != nullptr && step->bank == 1) { project(*step, values); }
    ReleaseSRWLockShared(&g_lock);
    return active;
}
void project_account(const AccountState& a, std::span<std::int32_t> values) noexcept {
    if (a.characterCount > a.characters.size()) { return; }
    AcquireSRWLockShared(&g_lock);
    const Step* selected = nullptr;
    for (std::size_t i = 0; i < a.characterCount; ++i) {
        const Step* step = held_step(a.characters[i]);
        if (step != nullptr && step->bank == 0
            && (selected == nullptr || step->ordinal > selected->ordinal)) { selected = step; }
    }
    if (selected != nullptr) { project(*selected, values); }
    ReleaseSRWLockShared(&g_lock);
}
} // namespace sunrise::state::build_data::quests
