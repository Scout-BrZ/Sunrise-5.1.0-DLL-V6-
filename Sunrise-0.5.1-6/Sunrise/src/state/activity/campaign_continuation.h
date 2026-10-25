#pragma once
#include <Windows.h>
#include <cstdint>
#include <array>
#include <string_view>
#include "definition.h"

namespace sunrise::state::activity::campaign_continuation {
// Cada rota preserva a sessao de origem e resolve o destino pelo hash do activity.
enum class Route : std::uint8_t { homecomingToAdieu, adieuToFarmCinematic, farmCinematicToFarm };
struct Destination {
    std::string_view source, target;
    std::uint32_t targetHash{};
};
inline constexpr std::array<Destination, 3> destinations{{
    {"mission_towerfall", "mission_journey", 0xB913ED3FU},
    {"mission_journey", "cine_farm_325", 0x5F3C8457U},
    // A cinematica da Chacara terminou; agora carrega o espaco social onde Tyra esta.
    {"cine_farm_325", "campaign_social_space_d2", 0x4E0FA012U},
}};
inline const Destination* destination(Route route) noexcept {
    const auto index = static_cast<std::size_t>(route);
    return index < destinations.size() ? &destinations[index] : nullptr;
}
// Pedido process-local: nao grava variaveis Lua nem altera o cache da missao.
struct Ticket {
    SessionBinding source{};
    std::uint64_t attempt{}, createdAt{};
    Route route{Route::homecomingToAdieu};
};
inline SRWLOCK ticketLock = SRWLOCK_INIT;
inline Ticket pending{};
inline bool offer(const Ticket& ticket) noexcept {
    if (destination(ticket.route) == nullptr || ticket.source.sessionId == 0 || ticket.attempt == 0) { return false; }
    AcquireSRWLockExclusive(&ticketLock);
    const bool same = same_binding(pending.source, ticket.source) && pending.attempt == ticket.attempt && pending.route == ticket.route;
    const bool free = pending.source.sessionId == 0;
    if (free) { pending = ticket; }
    ReleaseSRWLockExclusive(&ticketLock);
    return free || same;
}
inline Ticket peek() noexcept {
    AcquireSRWLockShared(&ticketLock);
    const Ticket copy = pending;
    ReleaseSRWLockShared(&ticketLock);
    return copy;
}
inline void retire(const Ticket& ticket) noexcept {
    AcquireSRWLockExclusive(&ticketLock);
    if (same_binding(pending.source, ticket.source) && pending.attempt == ticket.attempt && pending.route == ticket.route) { pending = {}; }
    ReleaseSRWLockExclusive(&ticketLock);
}
}
