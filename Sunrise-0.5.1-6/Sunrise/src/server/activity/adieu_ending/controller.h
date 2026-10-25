#pragma once
#include <array>
#include <cstdint>

namespace sunrise::server::activity::adieu_ending {
// Estado exclusivo da sessao/instancia. Nenhum temporizador conclui a missao.
enum class Phase : std::uint8_t { idle, selecting, arriving, offering, offered, playing, stopping, complete, failed };
struct Clip final {
    std::uint32_t region{}, registry{}, object{}, occurrence{}, slot{};
};
struct Controller final {
    std::array<Clip, 2> clips{};
    std::uint64_t attempt{}, generation{}, deadline{};
    // Diagnostico unico por transicao; nao altera o prazo nem conclui a missao.
    std::uint64_t arrivalDiagnosticAt{};
    unsigned movie{};
    Phase phase{Phase::idle};
    bool stopQueued{}, terminated{}, completionReported{};
    bool running() const noexcept { return phase != Phase::idle && phase != Phase::failed && phase != Phase::complete; }
    void fail() noexcept { phase=Phase::failed;deadline=0; }
    void begin(std::uint64_t owner, std::uint64_t client, std::uint64_t now) noexcept {
        attempt=owner;generation=client;movie=0;phase=Phase::selecting;deadline=now+90000;
        stopQueued=false;terminated=false;completionReported=false;
        arrivalDiagnosticAt=0;
    }
    bool matches(std::uint32_t registry, std::int32_t type, std::int32_t slot) const noexcept {
        return phase!=Phase::idle && type==6 && slot==0 && registry==clips[movie].registry;
    }
    // 0=iniciou, 1=pediu salto, 2=terminou. Um termino sem inicio e falha.
    void incident(unsigned signal, std::uint64_t now) noexcept {
        if(signal==0 && phase==Phase::offered) {phase=Phase::playing;deadline=now+650000;}
        else if(signal==1 && phase==Phase::playing) {phase=Phase::stopping;deadline=now+15000;}
        else if(signal==2 && (phase==Phase::playing || phase==Phase::stopping)) {
            phase=Phase::stopping;terminated=true;deadline=now+15000;
        } else if(signal==2 && phase==Phase::offered) {fail();}
    }
};
}
