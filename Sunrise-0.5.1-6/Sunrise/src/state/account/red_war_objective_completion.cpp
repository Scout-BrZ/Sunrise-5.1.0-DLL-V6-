#include "red_war_objective_completion.h"
#include <memory>
#include <atomic>
#include <limits>
#include "../../middleware/datagen/family4/loadout/loadout_resolver.h"
#include <cstring>
#include <array>
#include <mutex>
#include <new>
#include "account_context.h"
#include "../build_data/quests/red_war_quest_catalog.h"
#include "../investment/store_internal.h"
#include "../build_data/runtime.h"

namespace sunrise::state::account::red_war {
// Sinal compartilhado entre a conclusao da missao e o processamento da rede.
// Varias conclusoes pendentes podem ser atendidas por uma unica atualizacao do inventario.
namespace { std::atomic_bool g_publicationPending{}; }
bool consume_publication_request() noexcept {
    // Le e limpa o pedido de forma atomica: este pedido e consumido uma unica vez.
    return g_publicationPending.exchange(false, std::memory_order_acq_rel);
}

bool acquisition_matches(const CharacterState& character, std::uint32_t hash) noexcept {
    namespace catalog = build_data::quests;
    catalog::Step step{}, next{};
    bool terminal{};
    if (!catalog::transition(hash, step, next, terminal)) {
        // O item inicial conhecido nao pode escapar das verificacoes se o catalogo falhou.
        return hash != catalog::kRedWarAcquisitionHash;
    }
    if (!local_account_access() || character.inventory.count > character.inventory.values.size()) { return false; }
    // Nenhuma etapa desta mesma jornada pode estar possuida durante uma nova aquisicao.
    for (std::size_t i = 0; i < character.inventory.count; ++i) {
        catalog::Step held{}, successor{}; bool last{};
        const auto& item = character.inventory.values[i];
        if (item.quantity > 0 && catalog::transition(item.definitionHash, held, successor, last)) { return false; }
    }
    std::int32_t saved{};
    const auto bank = step.bank == 0 ? investment::store::Bank::objectiveValues
                                     : investment::store::Bank::characterObjectValues;
    if (!investment::store::read_unlock(bank, step.row, saved)) { return false; }
    // Zero permite somente a primeira aquisicao; -1 e valores desconhecidos sao recusados.
    return saved == step.value || (saved == 0 && step.ordinal == 0);
}

bool resolve_successor_item(std::uint16_t expectedCurrentOrdinal,
                            std::uint16_t& itemDefinitionIndex) noexcept {
    itemDefinitionIndex = UINT16_MAX;
    namespace catalog = build_data::quests;
    namespace store = investment::store;
    if (!local_account_access() || !catalog::ready()) { return false; }
    const std::lock_guard guard(store::g_mutex);
    auto snapshot = std::unique_ptr<AccountState>(new (std::nothrow) AccountState{});
    if (!snapshot || !store::read_account(*snapshot)
        || snapshot->characterCount > snapshot->characters.size()) { return false; }

    const CharacterState* selected = nullptr;
    for (std::size_t index = 0; index < snapshot->characterCount; ++index) {
        const auto& character = snapshot->characters[index];
        if (!character.selected) { continue; }
        if (selected != nullptr) { return false; }
        selected = &character;
    }
    if (selected == nullptr || selected->inventory.count > selected->inventory.values.size()) {
        return false;
    }

    catalog::Step successor{};
    std::size_t matches{};
    for (std::size_t index = 0; index < selected->inventory.count; ++index) {
        const auto& held = selected->inventory.values[index];
        if (held.quantity != 1) { continue; }
        catalog::Step candidate{}, next{};
        bool terminal{};
        if (!catalog::transition(held.definitionHash, candidate, next, terminal)
            || candidate.ordinal != expectedCurrentOrdinal || terminal
            || next.ordinal != static_cast<std::uint16_t>(expectedCurrentOrdinal + 1U)) {
            continue;
        }
        if (++matches != 1) { return false; }
        const auto bank = candidate.bank == 0 ? store::Bank::objectiveValues
                                               : store::Bank::characterObjectValues;
        std::int32_t tracked{};
        if (!store::read_unlock(bank, candidate.row, tracked) || tracked != candidate.value) {
            return false;
        }
        successor = next;
    }
    if (matches != 1) { return false; }
    for (std::size_t index = 0; index < selected->inventory.count; ++index) {
        const auto& held = selected->inventory.values[index];
        if (held.quantity > 0 && held.definitionHash == successor.itemHash) { return false; }
    }
    build_data::items::Definition definition{};
    if (!build_data::find_item_definition_hash(successor.itemHash, definition)) { return false; }
    itemDefinitionIndex = definition.definitionIndex;
    return true;
}

bool recover_acquisition(std::uint16_t requestedIndex, std::uint16_t& grantedIndex) noexcept {
    grantedIndex = requestedIndex;
    build_data::items::Definition requested{};
    if (!build_data::find_item_definition_index(requestedIndex, requested)) { return false; }
    namespace catalog = build_data::quests;
    if (requested.definitionHash != catalog::kRedWarAcquisitionHash) { return true; }
    if (!local_account_access()) { return false; }
    namespace store = investment::store;
    const std::lock_guard guard(store::g_mutex);
    catalog::Step first{}, next{}, savedStep{}; bool terminal{};
    if (!catalog::transition(requested.definitionHash, first, next, terminal)) { return false; }
    std::int32_t saved{};
    const auto bank = first.bank == 0 ? store::Bank::objectiveValues : store::Bank::characterObjectValues;
    if (!store::read_unlock(bank, first.row, saved) || saved == -1) { return false; }
    savedStep = first;
    if (saved != 0 && !catalog::step_for_value(saved, savedStep)) { return false; }
    build_data::items::Definition restored{};
    if (!build_data::find_item_definition_hash(savedStep.itemHash, restored)) { return false; }
    auto snapshot = std::unique_ptr<AccountState>(new (std::nothrow) AccountState{});
    if (!snapshot || !store::read_account(*snapshot) || snapshot->characterCount > snapshot->characters.size()) { return false; }
    const CharacterState* selected = nullptr;
    for (std::size_t i = 0; i < snapshot->characterCount; ++i) if (snapshot->characters[i].selected) {
        if (selected != nullptr) { return false; }
        selected = &snapshot->characters[i];
    }
    if (selected == nullptr || !acquisition_matches(*selected, savedStep.itemHash)) { return false; }
    // A concessao continua no caminho original: preview, persistencia e resposta ao cliente.
    // Nao grava progresso, nao cobra um item diferente e nao cria um inventario paralelo.
    grantedIndex = restored.definitionIndex;
    return true;
}

Result record_completed_objective(std::string_view package, std::uint64_t characterSoid) noexcept {
    // A cinematica da Chacara tambem conclui a etapa atual e libera Tyra Karn.
    if (package != "mission_towerfall" && package != "mission_journey"
        && package != "cine_farm_325") {
        return Result::notApplicable;
    }
    build_data::quests::CompletionObjective objective{};
    if (!build_data::quests::completion_objective(package, objective)) { return Result::unavailable; }
    if (characterSoid == 0) { return Result::unavailable; }
    // O personagem local pode nao estar publicado no cache de perfis publicos.
    // Usa o resolvedor existente de assinaturas: ele valida a posse no SQLite privado
    // e preserva as restricoes do escopo remoto/publico. Nao assume a conta local.
    const auto handle = account_for_subscription_root(characterSoid);
    if (handle == kInvalidAccount) { return Result::unavailable; }
    ScopedAccount scope(handle);
    if (!local_account_access()) { return Result::notApplicable; }
    namespace store = investment::store;
    // Mantem as operacoes abaixo na mesma transacao. Qualquer retorno antes do commit
    // desfaz as gravacoes, inclusive as feitas por write_account em uma transacao interna.
    store::Transaction transaction;
    auto snapshot = std::unique_ptr<AccountState>(new (std::nothrow) AccountState{});
    if (!transaction.ready() || !snapshot || !store::read_account(*snapshot)) { return Result::failed; }
    // Nao credita um personagem diferente que tenha sido selecionado durante a transicao.
    CharacterState* character = nullptr;
    std::size_t characterIndex{};
    if (snapshot->characterCount > snapshot->characters.size()) { return Result::failed; }
    for (std::size_t i = 0; i < snapshot->characterCount; ++i) {
        if (snapshot->characters[i].selected && snapshot->characters[i].soid == characterSoid) {
            if (character != nullptr) { return Result::failed; }
            character = &snapshot->characters[i];
            characterIndex = i;
        }
    }
    if (character == nullptr) { return Result::unavailable; }
    if (character->inventory.count > character->inventory.values.size()) { return Result::failed; }
    // Exige exatamente uma unidade da etapa correspondente a esta missao.
    // Se ela foi abandonada ou substituida anteriormente, nao avanca outra etapa.
    bool ownsStep{};
    std::size_t itemIndex{};
    std::uint32_t heldHash{};
    for (std::size_t i = 0; i < character->inventory.count; ++i) {
        const auto& item = character->inventory.values[i];
        // Amanda entrega o membro 0 (Homecoming, 100); alguns saves ja possuem
        // o membro 1 (Homecoming, 200). Concluir Homecoming atende ambos os casos.
        const bool openingHomecoming = package == "mission_towerfall"
            && item.definitionHash == build_data::quests::kRedWarAcquisitionHash;
        if (item.quantity > 0 && (item.definitionHash == objective.itemHash || openingHomecoming)) {
            if (ownsStep || item.quantity != 1) { return Result::failed; }
            ownsStep = true; itemIndex = i; heldHash = item.definitionHash;
        }
    }
    if (!ownsStep) { return Result::notOwned; }
    std::int32_t before{};
    // Os bancos 0 e 1 possuem linhas nativas; o banco 2 representa um slot avulso.
    // Slots avulsos sao persistidos por personagem, sem usa-los como indices de arrays.
    if (objective.bank < 2) {
        const auto bank = objective.bank == 0 ? store::Bank::objectiveValues
                                             : store::Bank::characterObjectValues;
        if (!store::read_unlock(bank, objective.row, before)) { return Result::failed; }
        if (!store::write_unlock(bank, objective.row, objective.threshold)) { return Result::failed; }
    } else {
        // Tabela auxiliar: nao altera os bancos nativos nem o esquema das aquisicoes.
        if (!store::execute("CREATE TABLE IF NOT EXISTS red_war_objectives("
            "character_soid INTEGER NOT NULL,objective_slot INTEGER NOT NULL,"
            "value INTEGER NOT NULL,PRIMARY KEY(character_soid,objective_slot))")) { return Result::failed; }
        store::Statement row("SELECT value FROM red_war_objectives WHERE character_soid=? AND objective_slot=?");
        if (!row.parameters(characterSoid, objective.slot)) { return Result::failed; }
        const int status = row.step();
        if (status != SQLITE_DONE && (status != SQLITE_ROW || !row.column(0, before))) { return Result::failed; }
        // MAX evita diminuir um contador ja salvo ao receber novamente a conclusao.
        store::Statement write("INSERT INTO red_war_objectives VALUES(?,?,?) "
            "ON CONFLICT(character_soid,objective_slot) DO UPDATE SET value=MAX(value,excluded.value)");
        if (!write.write(characterSoid, objective.slot, objective.threshold)) { return Result::failed; }
    }
    // O item mantem sua identidade: o cliente recebe uma nova definicao na mesma instancia.
    build_data::quests::Step current{}, next{};
    bool terminal{};
    // A sucessora vem da ordem do catalogo lido dos pacotes, nao de somar hashes ou valores.
    if (!build_data::quests::transition(heldHash, current, next, terminal)) { return Result::failed; }
    if (heldHash != objective.itemHash) {
        // A conclusao da missao nao pode parar no outro membro chamado Homecoming.
        // Usa a sucessora do objetivo de Homecoming: Adieu (250), lida do catalogo.
        build_data::quests::Step missionStep{};
        if (package != "mission_towerfall" || current.ordinal != 0 || objective.ordinal != 1
            || !build_data::quests::transition(objective.itemHash, missionStep, next, terminal)
            || missionStep.bank != current.bank || missionStep.row != current.row) { return Result::failed; }
    }
    const auto trackingBank = current.bank == 0 ? store::Bank::objectiveValues : store::Bank::characterObjectValues;
    std::int32_t tracking{};
    // O valor -1 significa jornada concluida; nao permite reabri-la por esta funcao.
    if (!store::read_unlock(trackingBank, current.row, tracking)) { return Result::failed; }
    // Exige a etapa exata. Nao compara com > ou <, pois os valores podem ser hashes negativos.
    if (tracking != current.value) { return Result::stateConflict; }
    // Evita gerar um numero de alteracao nulo ou fora do inteiro aceito pelo cliente.
    if (character->nextInventorySerial == 0 || character->nextInventorySerial >= static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)())) { return Result::failed; }
    // Caminho generico para o ultimo membro: remove o item e depois grava -1.
    // Homecoming e Adieu nao sao o ultimo membro e seguem pelo bloco else.
    if (terminal) {
        for (std::size_t i = itemIndex + 1; i < character->inventory.count; ++i) {
            character->inventory.values[i - 1] = character->inventory.values[i];
        }
        character->inventory.values[--character->inventory.count] = {};
    } else {
        // Recusa inventarios que ja tenham a sucessora, evitando criar uma duplicata.
        for (std::size_t i = 0; i < character->inventory.count; ++i) {
            if (character->inventory.values[i].quantity > 0 && character->inventory.values[i].definitionHash == next.itemHash) { return Result::failed; }
        }
        // Mantem instanceSoid. Troca a definicao, restaura sockets padrao e marca o item como novo.
        // O novo mutationSerial permite ao cliente reconhecer a alteracao da mesma instancia.
        auto& item = character->inventory.values[itemIndex];
        item.definitionHash = next.itemHash;
        item.sockets = {};
        item.seen = false;
        item.mutationSerial = static_cast<std::int32_t>(character->nextInventorySerial++);
    }
    // Valida a conta e a representacao do inventario antes de salvar.
    // Atualiza tambem a linha nativa que informa ao Diretor qual etapa esta ativa.
    middleware::datagen::family4::loadout::ResolvedLoadout resolved{};
    if (!account::valid(*snapshot)
        || !middleware::datagen::family4::loadout::resolve(*snapshot, characterIndex, resolved)
        || !store::write_account(*snapshot)
        || !store::write_unlock(trackingBank, current.row, terminal ? -1 : next.value)) { return Result::failed; }
    // Inventario, acompanhamento e contador confirmam juntos; falhas revertem todos.

    if (!transaction.commit()) { return Result::failed; }
    // So solicita a publicacao depois que todas as gravacoes foram confirmadas.
    // A rede sera atendida pelo BAP, sem adquirir seu bloqueio dentro desta transacao.
    g_publicationPending.store(true, std::memory_order_release);
    return Result::recorded;
}
bool project_objectives(const CharacterState& character, std::span<std::byte> tail) noexcept {
    // Offsets do mesmo layout confirmado no Dawn. O chamador valida offsetof do bloco.
    constexpr std::size_t kCount = 254, kRows = 258, kStride = 8, kCapacity = 20;
    if (tail.size() < kRows + kStride * kCapacity
        || character.inventory.count > character.inventory.values.size()) { return false; }
    std::array<build_data::quests::CompletionObjective, 3> plans{};
    if (!build_data::quests::completion_objective("mission_towerfall", plans[0])
        || !build_data::quests::completion_objective("mission_journey", plans[1])
        || !build_data::quests::completion_objective("cine_farm_325", plans[2])) { return true; }
    bool ownsAny{};
    for (const auto& plan : plans) for (std::size_t i = 0; i < character.inventory.count; ++i) {
        const auto& item = character.inventory.values[i];
        ownsAny = ownsAny || (plan.bank == 2 && item.quantity > 0 && item.definitionHash == plan.itemHash);
    }
    if (!ownsAny) { return true; }
    // O encoder ja esta no escopo privado do destinatario. Nao eleva o acesso de outro perfil.
    if (!local_account_access()) { return true; }
    namespace store = investment::store;
    const std::lock_guard guard(store::g_mutex);
    store::Statement exists("SELECT 1 FROM sqlite_master WHERE type='table' AND name='red_war_objectives'");
    const int table = exists.step();
    if (table != SQLITE_ROW && table != SQLITE_DONE) { return false; }
    std::uint32_t count{};
    std::memcpy(&count, tail.data() + kCount, sizeof count);
    if (count > kCapacity) { return false; }
    for (const auto& plan : plans) {
        if (plan.bank != 2) { continue; }
        bool owns{};
        for (std::size_t i = 0; i < character.inventory.count; ++i) {
            const auto& item = character.inventory.values[i];
            owns = owns || (item.quantity > 0 && item.definitionHash == plan.itemHash);
        }
        if (!owns) { continue; }
        std::int32_t value{};
        if (table == SQLITE_ROW) {
            store::Statement row("SELECT value FROM red_war_objectives WHERE character_soid=? AND objective_slot=?");
            if (!row.parameters(character.soid, plan.slot)) { return false; }
            const int status = row.step();
            if (status != SQLITE_DONE && (status != SQLITE_ROW || !row.column(0, value))) { return false; }
        }
        std::size_t position = 0;
        for (; position < count; ++position) {
            std::int16_t slot{};
            std::memcpy(&slot, tail.data() + kRows + position * kStride, sizeof slot);
            if (slot == static_cast<std::int16_t>(plan.slot)) { break; }
        }
        if (position >= kCapacity) { return false; }
        if (position == count) { ++count; }
        const auto slot = static_cast<std::int16_t>(plan.slot);
        const std::uint16_t reserved = 0;
        auto* row = tail.data() + kRows + position * kStride;
        std::memcpy(row, &slot, sizeof slot);
        std::memcpy(row + 2, &reserved, sizeof reserved);
        std::memcpy(row + 4, &value, sizeof value);
    }
    std::memcpy(tail.data() + kCount, &count, sizeof count);
    return true;
}
const char* result_name(Result result) noexcept {
    switch (result) {
    case Result::recorded: return "recorded";
    case Result::alreadyRecorded: return "already_recorded";
    case Result::notOwned: return "step_not_owned";
    case Result::notApplicable: return "not_applicable";
    case Result::unavailable: return "unavailable";
    case Result::failed: return "failed";
    case Result::stateConflict: return "step_state_conflict";
    }
    return "unknown";
}
}
