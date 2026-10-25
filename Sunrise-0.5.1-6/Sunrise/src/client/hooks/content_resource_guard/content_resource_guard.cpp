#include "content_resource_guard.h"

#include <Windows.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../patterns/image_scan.h"
#include "../../memory/current_process_memory.h"
#include "../../process/freeze/client_process_freeze.h"

namespace sunrise::client::hooks::content_resource_guard {
namespace {
using patterns::signature;
using patterns::signature_length;

// Assinatura da imagem capturada: recusa outras versoes em vez de usar um RVA fixo.
constexpr std::string_view kSignatureText =
    "0F AF 51 30 48 63 41 34 8B FA 48 8D 53 10 48 03 79 08 48 23 47 08 "
    "48 2B F8 80 3A FF 75 05 40 B6 06 EB 4E 45 33 C0 C6 45 BD 00 "
    "48 8D 4C 24 50 E8 23 99 00 00 84 C0 0F 84 A1 00 00 00";
constexpr auto kSignature = signature<signature_length(kSignatureText)>(kSignatureText);
constexpr std::array<unsigned char, 6> kOriginal{0x48, 0x2B, 0xF8, 0x80, 0x3A, 0xFF};
constexpr std::array<unsigned char, 20> kSkipBytes{
    0x44, 0x8B, 0x44, 0x24, 0x30, 0x4C, 0x8B, 0x54, 0x24, 0x38,
    0x8B, 0x54, 0x24, 0x20, 0xFF, 0xC2, 0x89, 0x54, 0x24, 0x20};
constexpr std::size_t kSiteOffset = 0x16, kStubSize = 20, kAttempts = 8;
std::byte* g_site{};
std::byte* g_stub{};
std::array<unsigned char, 6> g_patch{};
bool g_attached{};
bool g_siteHealthy{true};
DWORD g_originalProtection{};

void report(bool success, const char* text) noexcept {
    core::log::write(core::log::Channel::client,
        success ? core::log::Level::info : core::log::Level::warn, text);
}

// Todos os saltos sao relativos de 32 bits; nao se presume que a DLL esteja perto do EXE.
bool displacement(std::uintptr_t next, std::uintptr_t target, std::int32_t& result) noexcept {
    const auto delta = static_cast<std::int64_t>(target) - static_cast<std::int64_t>(next);
    if (delta < (std::numeric_limits<std::int32_t>::min)()
        || delta > (std::numeric_limits<std::int32_t>::max)()) { return false; }
    result = static_cast<std::int32_t>(delta);
    return true;
}

std::byte* allocate_near(std::byte* site) noexcept {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const auto granularity = static_cast<std::uintptr_t>(info.dwAllocationGranularity);
    const auto center = reinterpret_cast<std::uintptr_t>(site) & ~(granularity - 1U);
    const auto low = reinterpret_cast<std::uintptr_t>(info.lpMinimumApplicationAddress);
    const auto high = reinterpret_cast<std::uintptr_t>(info.lpMaximumApplicationAddress);
    for (std::uintptr_t distance = granularity; distance < 0x7FFF0000ULL;
         distance += granularity) {
        const std::array<std::uintptr_t, 2> choices{
            center >= distance ? center - distance : 0,
            center <= high - distance ? center + distance : 0};
        for (const auto address : choices) {
            if (address < low || address > high - info.dwPageSize) { continue; }
            MEMORY_BASIC_INFORMATION region{};
            if (VirtualQuery(reinterpret_cast<void*>(address), &region, sizeof region) == 0
                || region.State != MEM_FREE) { continue; }
            auto* memory = static_cast<std::byte*>(VirtualAlloc(reinterpret_cast<void*>(address),
                info.dwPageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
            if (memory != nullptr) { return memory; }
        }
    }
    return nullptr;
}

bool prepare_stub(std::byte* skip) noexcept {
    // SUB RDI,RAX; TEST RDI,RDI; JZ skip; CMP [RDX],FF; JMP continuacao.
    // Nao usa pilha, nao chama funcoes e nao altera outros registradores.
    std::array<unsigned char, kStubSize> bytes{
        0x48, 0x2B, 0xF8, 0x48, 0x85, 0xFF, 0x0F, 0x84,
        0, 0, 0, 0, 0x80, 0x3A, 0xFF, 0xE9, 0, 0, 0, 0};
    const auto stub = reinterpret_cast<std::uintptr_t>(g_stub);
    const auto site = reinterpret_cast<std::uintptr_t>(g_site);
    std::int32_t toSkip{}, toResume{}, toStub{};
    if (!displacement(stub + 12, reinterpret_cast<std::uintptr_t>(skip), toSkip)
        || !displacement(stub + bytes.size(), site + kOriginal.size(), toResume)
        || !displacement(site + 5, stub, toStub)) { return false; }
    std::memcpy(bytes.data() + 8, &toSkip, sizeof toSkip);
    std::memcpy(bytes.data() + 16, &toResume, sizeof toResume);
    g_patch = {0xE9, 0, 0, 0, 0, 0x90};
    std::memcpy(g_patch.data() + 1, &toStub, sizeof toStub);
    std::memcpy(g_stub, bytes.data(), bytes.size());
    DWORD previous{};
    return VirtualProtect(g_stub, bytes.size(), PAGE_EXECUTE_READ, &previous) != FALSE
        && FlushInstructionCache(GetCurrentProcess(), g_stub, bytes.size()) != FALSE;
}

// O stub nao tem CALL: basta recusar qualquer RIP dentro dele ou da janela substituida.
bool code_idle(const process::freeze::Held& held) noexcept {
    const auto site = reinterpret_cast<std::uintptr_t>(g_site);
    const auto stub = reinterpret_cast<std::uintptr_t>(g_stub);
    for (std::size_t i = 0; i < held.count; ++i) {
        CONTEXT context{};
        context.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(held.handles[i], &context) == FALSE) { return false; }
        if ((context.Rip >= site && context.Rip < site + kOriginal.size())
            || (context.Rip >= stub && context.Rip < stub + kStubSize)) { return false; }
    }
    return true;
}

// Mantem a posse do stub mesmo se a restauracao de protecao/cache falhar apos a escrita.
// Assim nenhuma falha parcial permite descarregar memoria ainda referenciada pelo jogo.
bool update_site(bool attaching) noexcept {
    for (std::size_t attempt = 0; attempt < kAttempts; ++attempt) {
        process::freeze::Held held{};
        if (!process::freeze::hold(held)) { return false; }
        if (!code_idle(held)) {
            process::freeze::release(held);
            Sleep(1);
            continue;
        }
        const auto& expected = g_attached ? g_patch : kOriginal;
        if (std::memcmp(g_site, expected.data(), expected.size()) != 0) {
            process::freeze::release(held);
            return false;
        }
        DWORD previous{};
        if (VirtualProtect(g_site, kOriginal.size(), PAGE_EXECUTE_READWRITE, &previous) == FALSE) {
            process::freeze::release(held);
            return false;
        }
        if (g_siteHealthy) { g_originalProtection = previous; }
        const auto& replacement = attaching ? g_patch : kOriginal;
        std::memcpy(g_site, replacement.data(), replacement.size());
        g_attached = attaching;
        const bool flushed = FlushInstructionCache(GetCurrentProcess(), g_site,
            replacement.size()) != FALSE;
        DWORD ignored{};
        const bool protectedAgain = VirtualProtect(g_site, replacement.size(),
            g_originalProtection, &ignored) != FALSE;
        g_siteHealthy = flushed && protectedAgain;
        // Libera somente enquanto todas as threads estao paradas e o original foi restaurado.
        bool freed = true;
        if (!attaching && g_siteHealthy) {
            freed = VirtualFree(g_stub, 0, MEM_RELEASE) != FALSE;
            if (freed) { g_stub = nullptr; }
        }
        process::freeze::release(held);
        return g_siteHealthy && freed;
    }
    return false;
}
} // namespace

bool install() noexcept {
    if (g_attached) { return g_siteHealthy; }
    if (g_stub != nullptr) { return false; }
    auto* match = patterns::scan_main_image_unique(kSignature, "content_resource_null_guard");
    if (match == nullptr) {
        report(false, "ev=content_resource_guard stage=signature result=fail");
        return false;
    }
    g_site = match + kSiteOffset;
    // O JE original no fim da assinatura deve apontar para a continuacao conhecida.
    auto* branch = match + 54;
    std::int32_t delta{};
    std::memcpy(&delta, branch + 2, sizeof delta);
    auto* skip = branch + 6 + delta;
    std::array<unsigned char, kSkipBytes.size()> observedSkip{};
    if (skip != g_site + 0xC7
        || !memory::read_current_process(nullptr, reinterpret_cast<std::uintptr_t>(skip),
            std::as_writable_bytes(std::span(observedSkip)))
        || observedSkip != kSkipBytes) {
        report(false, "ev=content_resource_guard stage=continuation result=fail");
        return false;
    }
    g_stub = allocate_near(g_site);
    if (g_stub == nullptr || !prepare_stub(skip)) {
        if (g_stub != nullptr && VirtualFree(g_stub, 0, MEM_RELEASE) != FALSE) { g_stub = nullptr; }
        report(false, "ev=content_resource_guard stage=stub result=fail");
        return false;
    }
    const bool success = update_site(true);
    if (!success && !g_attached && g_siteHealthy) {
        if (VirtualFree(g_stub, 0, MEM_RELEASE) != FALSE) { g_stub = nullptr; }
    }
    report(success, success ? "ev=content_resource_guard stage=install result=ok"
                            : "ev=content_resource_guard stage=install result=fail");
    return success;
}

bool uninstall() noexcept {
    if (g_stub == nullptr) { return !g_attached; }
    const bool success = update_site(false);
    report(success, success ? "ev=content_resource_guard stage=uninstall result=ok"
                            : "ev=content_resource_guard stage=uninstall result=fail");
    if (success) { g_site = nullptr; g_patch = {}; }
    return success;
}
} // namespace sunrise::client::hooks::content_resource_guard
