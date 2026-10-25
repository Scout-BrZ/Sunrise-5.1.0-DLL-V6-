#pragma once
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>
#include "../../account/account_state.h"

namespace sunrise::state::build_data::quests {
// Item real usado pela primeira substituicao de campanha no Dawn.
// Os membros, valores e indices dos bancos sao lidos dos pacotes instalados.
inline constexpr std::uint32_t kRedWarAcquisitionHash = 0x37DD26F0U;
struct Step {
    std::uint32_t itemHash{};
    std::int32_t value{};
    std::uint16_t ordinal{}, row{};
    std::uint8_t bank{}; // 0: conta; 1: personagem.
};
// itemHash identifica o item possuido; current e next recebem copias dos dados nativos.
// Retorna false se o item nao pertence ao catalogo ou se existe uma lacuna antes da sucessora.
// Copia a etapa atual e sua sucessora; terminal somente no ultimo membro do catalogo.
[[nodiscard]] bool transition(std::uint32_t itemHash, Step& current, Step& next, bool& terminal) noexcept;
// Resolve o valor salvo por igualdade; valores de etapa nao sao numeros sequenciais.
[[nodiscard]] bool step_for_value(std::int32_t value, Step& output) noexcept;
[[nodiscard]] bool ready() noexcept;
struct CompletionObjective {
    std::uint32_t itemHash{};
    std::int32_t threshold{};
    std::uint16_t ordinal{}, slot{}, row{}, objectiveIndex{};
    std::uint8_t bank{}; // 2: slot avulso, sem linha nos bancos fixos.
};
[[nodiscard]] bool publish(std::vector<Step> steps,
    std::vector<CompletionObjective> objectives) noexcept;
[[nodiscard]] bool completion_objective(std::string_view package,
    CompletionObjective& output) noexcept;
// Retorna true somente quando uma etapa desta jornada estiver realmente no inventario.
// Nao cria itens, nao avanca a jornada e nao grava progresso no banco de dados.
[[nodiscard]] bool project_character(const CharacterState& character,
    std::span<std::int32_t> values) noexcept;
void project_account(const AccountState& account, std::span<std::int32_t> values) noexcept;
} // namespace sunrise::state::build_data::quests
