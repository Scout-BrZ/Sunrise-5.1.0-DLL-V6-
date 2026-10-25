// Incluir no namespace anonimo de mission_launch.cpp, imediatamente antes de submit().
// Reutiliza os tipos nativos e os validadores que ja existem no lancador da V6.
namespace continuation = state::activity::campaign_continuation;
struct CampaignTransfer {
    continuation::Ticket ticket{};
    std::uintptr_t nativeSession{};
    std::uint64_t nonce{};
    std::uint16_t target{};
    bool active{}, submitted{}, departed{}, overrideChanged{};
    forced::ForcedDestination previousOverride{};
};
CampaignTransfer g_campaignTransfer{};
std::atomic_flag g_launchPolling = ATOMIC_FLAG_INIT;

template<class T> bool continuation_read(std::uintptr_t address, T& value) noexcept {
    SIZE_T copied{};
    return address != 0 && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
        &value, sizeof value, &copied) != FALSE && copied == sizeof value;
}
// Mesmos offsets usados no Dawn; se o descritor nao estiver pronto, nao solicita a saida.
bool continuation_nonce(std::uintptr_t session, std::uint64_t& nonce) noexcept {
    std::uint8_t flags{};
    return continuation_read(session + 0x182C0 + 0x140, flags) && (flags & 1U)
        && continuation_read(session + 0x182C0 + 0x148 + 0x18, nonce)
        && nonce != 0 && nonce != UINT64_MAX;
}
using ContinuationLeave = void(__fastcall*)(std::int32_t, std::int32_t);
ContinuationLeave continuation_leave() noexcept {
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    std::array<unsigned char, 8> actual{};
    constexpr std::array<unsigned char, 8> expected{0x48,0x89,0x5C,0x24,0x18,0x55,0x56,0x57};
    // RVA e assinatura retirados do Dawn. Recusa outra versao do executavel.
    return continuation_read(base + 0xE2DEB0, actual) && actual == expected
        ? reinterpret_cast<ContinuationLeave>(base + 0xE2DEB0) : nullptr;
}
void finish_campaign_transfer(Status status) noexcept {
    auto& transfer = g_campaignTransfer;
    const auto* route = continuation::destination(transfer.ticket.route);
    core::log::writef(core::log::Channel::client, core::log::Level::info,
        "ev=campaign_continuation stage=finish status=%u target=%.*s attempt=%llu",
        static_cast<unsigned>(status), route ? static_cast<int>(route->target.size()) : 0,
        route ? route->target.data() : "", static_cast<unsigned long long>(transfer.ticket.attempt));
    // So restaura a selecao anterior se ainda nao iniciou a saida do mundo.
    if (status != Status::arrived && transfer.overrideChanged && !transfer.departed) {
        (void)forced::publish(transfer.previousOverride);
    }
    continuation::retire(transfer.ticket);
    transfer = {};
    finish(status);
}
// Procura a chegada ao destino solicitado, sem escolher uma sessao auxiliar mais nova.
bool campaign_arrived(const CampaignTransfer& transfer, std::string_view package) noexcept {
    std::array<state::activity::SessionRosterRow, state::activity::kSessionCapacity> roster{};
    std::size_t count{};
    if (!transfer.departed || !state::activity::snapshot_session_roster(roster, count)) { return false; }
    for (const auto& row : std::span(roster).first(count)) {
        const auto& binding = row.binding;
        const auto& destination = binding.destination;
        if (!row.joined || binding.createdRevision <= transfer.ticket.source.createdRevision
            || state::activity::same_binding(binding, transfer.ticket.source)
            || destination.activityIndex != static_cast<std::int16_t>(transfer.target)
            || destination.packageNameLength != package.size()
            || destination.packageNameLength > destination.packageName.size()) { continue; }
        if (std::string_view(reinterpret_cast<const char*>(destination.packageName.data()),
            destination.packageNameLength) == package) { return true; }
    }
    return false;
}
bool poll_campaign_transfer(std::int32_t step) noexcept {
    auto& transfer = g_campaignTransfer;
    const auto now = GetTickCount64();
    if (!transfer.active) {
        const auto ticket = continuation::peek();
        if (ticket.source.sessionId == 0) { return false; }
        if (now - ticket.createdAt > kArrivalTimeoutMs) { continuation::retire(ticket); return false; }
        AcquireSRWLockExclusive(&g_lock);
        // A verificacao e a reserva sao atomicas em relacao aos pedidos da interface.
        if (g_state.busy) { ReleaseSRWLockExclusive(&g_lock); return false; }
        transfer = {}; transfer.active = true; transfer.ticket = ticket;
        g_state = {Status::requested, 0, true};
        g_requestedAt = now;
        ReleaseSRWLockExclusive(&g_lock);
    }
    const auto* route = continuation::destination(transfer.ticket.route);
    const auto& source = transfer.ticket.source.destination;
    if (route == nullptr || source.packageNameLength > source.packageName.size()
        || std::string_view(reinterpret_cast<const char*>(source.packageName.data()), source.packageNameLength) != route->source) {
        finish_campaign_transfer(Status::descriptorRejected); return true;
    }
    if (now - transfer.ticket.createdAt > kArrivalTimeoutMs) {
        finish_campaign_transfer(Status::timedOut); return true;
    }
    if (!g_resolved.load(std::memory_order_acquire) || continuation_leave() == nullptr) {
        finish_campaign_transfer(Status::nativeUnavailable); return true;
    }
    const auto rows = state::build_data::activities::entries();
    if (rows.empty()) { return true; }
    if (transfer.submitted && step == kInWorldStep && campaign_arrived(transfer, route->target)) {
        finish_campaign_transfer(Status::arrived); return true;
    }
    // Depois da saida, a sessao antiga pode desaparecer: espera somente a nova chegada.
    if (transfer.departed || step != kInWorldStep) { return true; }
    state::activity::mission::InputSequenceSnapshot input{};
    // Confere a sessao exata autorizada pelo servidor. Sessoes auxiliares mais
    // recentes nao representam uma troca da missao que o jogador esta concluindo.
    const char* refusal = nullptr;
    if (!state::activity::binding_matches(transfer.ticket.source)) { refusal = "source_binding_changed"; }
    else if (!state::activity::is_joined(transfer.ticket.source.sessionId)) { refusal = "source_not_joined"; }
    else if (!state::activity::mission::input_sequence_snapshot(transfer.ticket.source, input)) { refusal = "mission_state_missing"; }
    else if (input.faulted) { refusal = "mission_faulted"; }
    else if (input.attemptGeneration != transfer.ticket.attempt) { refusal = "attempt_changed"; }
    if (refusal != nullptr) {
        core::log::writef(core::log::Channel::client, core::log::Level::warn,
            "ev=campaign_continuation stage=refused reason=%s session=%llu attempt=%llu current_attempt=%llu",
            refusal, static_cast<unsigned long long>(transfer.ticket.source.sessionId),
            static_cast<unsigned long long>(transfer.ticket.attempt),
            static_cast<unsigned long long>(input.attemptGeneration));
        finish_campaign_transfer(Status::notReady); return true;
    }
    const auto natives = g_natives;
    SessionManager* manager = natives.manager();
    if (manager == nullptr || manager->primary < 0 || manager->primary >= static_cast<std::int32_t>(kSessionSlots)) { return true; }
    const auto* session = &manager->sessions[static_cast<std::size_t>(manager->primary)];
    const auto address = reinterpret_cast<std::uintptr_t>(session);
    if (transfer.submitted) {
        std::uint64_t nonce{}; std::int16_t selected{-1}; std::uint8_t kind{0xFF};
        if (address != transfer.nativeSession || !continuation_nonce(address, nonce)
            || nonce != transfer.nonce
            || !continuation_read(address + 0x182C0 + 0x148 + 4, selected)
            || selected != static_cast<std::int16_t>(transfer.target)
            || !continuation_read(address + 0x182C0 + 0x378, kind) || kind == 0xFF) { return true; }
        // O destino foi confirmado no descritor nativo. A saida e solicitada uma unica vez.
        transfer.departed = true;
        core::log::writef(core::log::Channel::client, core::log::Level::info,
            "ev=campaign_continuation stage=departure target=%.*s attempt=%llu",
            static_cast<int>(route->target.size()), route->target.data(),
            static_cast<unsigned long long>(transfer.ticket.attempt));
        continuation_leave()(28, 309);
        return true;
    }
    std::uint16_t target{}; std::size_t matches{};
    for (const auto& row : rows) if (row.hash == route->targetHash && row.name() == route->target) {
        target = row.index; ++matches;
    }
    if (matches != 1 || target > forced::kMaximumActivityIndex || target >= rows.size() || rows[target].hash != route->targetHash) {
        finish_campaign_transfer(Status::entryUnavailable); return true;
    }
    state::build_data::scenarios::Definition layout{};
    if (!state::build_data::find_scenario_layout(route->target, layout)) {
        finish_campaign_transfer(Status::entryUnavailable); return true;
    }
    if (session->state < kSessionStateFirstReady || session->state > kSessionStateLastReady
        || !natives.sessionReady(session) || !natives.memberReady(session)
        || session->localMember < 0 || session->localMember >= kMemberSlots) { return true; }
    const MemberRecord* record = natives.memberRecord(static_cast<std::uint32_t>(session->localMember));
    if (record == nullptr || record->launchState >= kLaunchStatePending) { return true; }
    Selection selection{};
    if (natives.construct(&selection, kSelectionReason, static_cast<std::int16_t>(target)) != &selection
        || selection.kind != kSelectionKindActivity || selection.source != static_cast<std::int16_t>(target)
        || selection.destination != static_cast<std::int16_t>(target) || !natives.valid(&selection)) {
        finish_campaign_transfer(Status::descriptorRejected); return true;
    }
    const char* package = natives.name(static_cast<std::int16_t>(target));
    if (package == nullptr || std::string_view(package) != route->target
        || !continuation_nonce(address, transfer.nonce)) { return true; }
    // Retem o nonce da sessao de origem, como o caminho de continuacao do Dawn.
    std::memcpy(reinterpret_cast<std::byte*>(&selection) + 0x10, &transfer.nonce, sizeof transfer.nonce);
    if (!natives.valid(&selection)) { finish_campaign_transfer(Status::descriptorRejected); return true; }
    transfer.nativeSession = address; transfer.target = target;
    g_previousSession = transfer.ticket.source; g_leftOrbit = true;
    // Remove somente um override de destino anterior. Nao limpa cache, wipes ou progresso.
    forced::snapshot(transfer.previousOverride); forced::clear(); transfer.overrideChanged = true;
    AcquireSRWLockExclusive(&g_lock);
    g_state.index = target;
    ReleaseSRWLockExclusive(&g_lock);
    natives.clear(); natives.select(kSelectionSlot, &selection); natives.commit(kCommitLaunch);
    transfer.submitted = true;
    core::log::writef(core::log::Channel::client, core::log::Level::info,
        "ev=campaign_continuation stage=selected target=%.*s activity=%u",
        static_cast<int>(route->target.size()), route->target.data(), static_cast<unsigned>(target));
    finish(Status::queued);
    return true;
}
