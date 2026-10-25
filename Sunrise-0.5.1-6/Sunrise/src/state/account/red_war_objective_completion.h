#pragma once
#include <cstdint>
#include <string_view>
#include <cstddef>
#include <span>
#include "account_state.h"
namespace sunrise::state::account::red_war {
// Consumido pelo BAP sob seu proprio lock, depois do commit; nao chama a rede sob o lock da missao.
[[nodiscard]] bool consume_publication_request() noexcept;
// recorded: salvou e avancou; notOwned: etapa ausente; notApplicable: missao/perfil nao atendido.
// unavailable: contexto/catalogo ainda indisponivel; failed: validacao ou gravacao recusada.
// alreadyRecorded permanece no contrato existente, mas a troca de item impede repetir o mesmo avanco.
// stateConflict: item e acompanhamento divergem; nao repete nem retrocede o progresso.
enum class Result { recorded, alreadyRecorded, notOwned, notApplicable, unavailable, failed, stateConflict };
// Chamado no fluxo existente de Amanda apos resolver o item real da jornada.
// Recupera a etapa salva, recusa jornada concluida ou ja possuida e preserva outras compras.
[[nodiscard]] bool recover_acquisition(std::uint16_t requestedIndex, std::uint16_t& grantedIndex) noexcept;
// Revalida tambem no preparo, preview e commit para impedir uma compra obsoleta.
[[nodiscard]] bool acquisition_matches(const CharacterState& character, std::uint32_t hash) noexcept;
// Resolve the installed definition for the successor of the selected character's held step.
// The vendor may call this only for the exact authored claim trigger and expected ordinal.
[[nodiscard]] bool resolve_successor_item(std::uint16_t expectedCurrentOrdinal,
                                          std::uint16_t& itemDefinitionIndex) noexcept;
// Chamar somente depois da conclusao duravel da tentativa, com o jogador dono da sessao.
// Grava o objetivo e avanca o item na mesma transacao persistente.
[[nodiscard]] Result record_completed_objective(std::string_view package,
    std::uint64_t characterSoid) noexcept;
// Publica apenas os objetivos das etapas realmente possuidas, na lista avulsa do Family 4.
[[nodiscard]] bool project_objectives(const CharacterState& character,
    std::span<std::byte> activityTail) noexcept;
[[nodiscard]] const char* result_name(Result result) noexcept;
}
