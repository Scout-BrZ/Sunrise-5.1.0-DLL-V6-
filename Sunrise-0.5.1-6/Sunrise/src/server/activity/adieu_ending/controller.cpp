#include "controller.h"
#include "../mission/mission_script_runtime_internal.h"
#include "../activity_sdk_mission_runtime.h"
#include "../../../state/activity/membership/activity_membership_query.h"

namespace sunrise::server::activity::mission {
namespace {
namespace scenes = activity_sdk_mission;
namespace ending = ::sunrise::server::activity::adieu_ending;
void ending_log(RuntimeInstance& instance, std::string_view result) noexcept {
    log_line(core::log::Level::info, &instance, "adieu_ending", result);
}
// Resolve os slots pelo objeto e estado autorais, sem offsets nativos inventados.
bool resolve_ending_clips(RuntimeInstance& instance, std::array<ending::Clip,2>& clips) noexcept {
    const auto* scenario=sdk::bound_scenario(instance.view);
    if(instance.publicTarget || scenario==nullptr || scenario->tag!=0x80B5E01FU) return false;
    const auto states=instance.view.catalog->states();
    const auto objects=instance.view.catalog->objects();
    const auto slots=instance.view.catalog->slots();
    const auto occurrences=instance.view.catalog->occurrences();
    for(unsigned movie=0;movie<2;++movie) {
        const auto region=26U+movie;
        const auto objectTag=movie==0 ? 0x80B5E364U : 0x80B5E368U;
        const auto registry=movie==0 ? 0x251FFE4EU : 0xDC3FB621U;
        bool found=false;
        for(std::size_t row=0;row<occurrences.size();++row) {
            const auto& occurrence=occurrences[row];
            if(occurrence.scenarioIndex!=instance.view.scenarioRow || occurrence.stateIndex>=states.size()
                || occurrence.objectIndex>=objects.size()) continue;
            const auto& state=states[occurrence.stateIndex];
            const auto& object=objects[occurrence.objectIndex];
            if(state.stateOrdinal!=movie+2 || state.sliceSetIndex!=24 || object.objectTag!=objectTag
                || object.slots.first>slots.size() || object.slots.count>slots.size()-object.slots.first) continue;
            for(std::uint32_t offset=0;offset<object.slots.count;++offset) {
                const auto slotRow=object.slots.first+offset;
                const auto& slot=slots[slotRow];
                if(slot.slotIndex!=0 || slot.slotType!=sdk::format::kCinematicSlotType
                    || slot.componentClass!=sdk::format::kCinematicComponentClass
                    || slot.authSchema!=sdk::format::kCinematicAuthSchema
                    || (slot.flags & sdk::format::kSlotSchemaJoinExact)==0) continue;
                if(found) return false;
                clips[movie]={region,registry,objectTag,static_cast<std::uint32_t>(row),slotRow};found=true;
            }
        }
        if(!found) return false;
    }
    return true;
}
}
bool begin_adieu_ending(RuntimeInstance& instance, std::uint64_t now) noexcept {
    auto& ending=instance.adieuEnding;
    if(ending.phase!=adieu_ending::Phase::idle) {
        return ending.attempt==instance.attempt.generation && ending.generation==instance.view.activityClientGeneration
            && ending.phase!=adieu_ending::Phase::failed;
    }
    std::array<adieu_ending::Clip,2> clips{};
    if(instance.attempt.complete || !resolve_ending_clips(instance,clips)) return false;
    ending.clips=clips;
    ending.begin(instance.attempt.generation,instance.view.activityClientGeneration,now);
    ::sunrise::state::activity::membership::set_program_spawn_hold(instance.view.binding.sessionId,true);
    ending_log(instance,"begin");return true;
}
void service_adieu_ending(RuntimeInstance& instance,std::uint64_t now) noexcept {
    auto& ending=instance.adieuEnding;
    using Phase=adieu_ending::Phase;
    if(ending.phase==Phase::idle) return;
    if(ending.attempt!=instance.attempt.generation || ending.generation!=instance.view.activityClientGeneration) {
        ending={};return;
    }
    if(ending.phase==Phase::failed || ending.completionReported) return;
    if(ending.deadline && now>=ending.deadline) {ending.fail();ending_log(instance,"timeout");return;}
    lua_vm::Intent queued{};
    if(lua_vm::pending_intent(instance.vm,queued)) return; // Respeita a fila existente da missao.
    const auto& clip=ending.clips[ending.movie];
    if(ending.phase==Phase::selecting) {
        scenes::Snapshot selected{};
        const auto status=scenes::select_state(instance.view,static_cast<std::int32_t>(clip.region),{},selected);
        if(status==scenes::Status::outputBusy) return;
        if(status!=scenes::Status::ready || !selected.configured || selected.plan.effectiveRegion!=clip.region) {
            ending.fail();ending_log(instance,"state_refused");return;
        }
        arm_state_region_teleport(instance,selected.plan,0x811C9DC5U);
        ending.arrivalDiagnosticAt=now+15000;
        ending.phase=Phase::arriving;ending_log(instance,"transition_requested");return;
    }
    if(ending.phase==Phase::arriving) {
        const auto placement=::sunrise::state::activity::membership::reported_placement(instance.view.binding.sessionId);
        // Registra uma vez qual confirmacao ainda falta, sem reenviar comandos.
        const bool teleportArmed=::sunrise::state::activity::membership::host_teleport_armed(instance.view.binding.sessionId);
        if(ending.arrivalDiagnosticAt!=0 && now>=ending.arrivalDiagnosticAt) {
            ending.arrivalDiagnosticAt=0;
            if(placement.currentRegion!=static_cast<std::int32_t>(clip.region)) {
                ending_log(instance,"waiting_current_region");
            } else if(teleportArmed) {
                ending_log(instance,"waiting_teleport_confirmation");
            } else if(scenes::cinematic_availability(instance.view,clip.occurrence,clip.slot)!=scenes::SceneStatus::ready) {
                ending_log(instance,"waiting_cinematic_availability");
            }
        }
        if(placement.currentRegion!=static_cast<std::int32_t>(clip.region)) return;
        // A regiao atual pode chegar antes da resposta de teletransporte.
        // Aguarda a validacao de token/destino/hash antes de solicitar o video.
        if(teleportArmed) return;
        if(scenes::cinematic_availability(instance.view,clip.occurrence,clip.slot)!=scenes::SceneStatus::ready) return;
        ending.phase=Phase::offering;ending_log(instance,"arrival_confirmed");
    }
    if(ending.phase==Phase::offering) {
        const auto status=scenes::set_cinematic_active(instance.view,clip.occurrence,clip.slot,true);
        if(status==scenes::SceneStatus::queued) {
            ending.phase=Phase::offered;ending.deadline=now+60000;ending_log(instance,"play_requested");
        } else if(status!=scenes::SceneStatus::outputBusy && status!=scenes::SceneStatus::missionSeedPending) {
            ending.fail();ending_log(instance,"play_refused");
        }
        return;
    }
    if(ending.phase==Phase::stopping) {
        if(!ending.stopQueued) {
            const auto status=scenes::set_cinematic_active(instance.view,clip.occurrence,clip.slot,false);
            if(status==scenes::SceneStatus::queued) ending.stopQueued=true;
            else if(status!=scenes::SceneStatus::outputBusy && status!=scenes::SceneStatus::missionSeedPending) {
                ending.fail();ending_log(instance,"stop_refused");
            }
            return;
        }
        host::PendingScriptableOverride pending{};
        if(!ending.terminated || host::pending_scriptable_override_for_activity_client(
            instance.view.binding,instance.view.activityClientGeneration,pending)) return;
        if(ending.movie==0) {
            ending.movie=1;ending.phase=Phase::selecting;ending.deadline=now+90000;
            ending.stopQueued=false;ending.terminated=false;ending_log(instance,"next_movie");return;
        }
        ending.phase=Phase::complete;ending.deadline=0;ending.completionReported=true;
        // Reutiliza o evento existente para a conclusao duravel do Lua, somente apos os dois videos.
        host::Event done{};
        done.kind=host::EventKind::cinematicTerminated;done.binding=instance.view.binding;
        done.sourceGeneration=instance.view.activityClientGeneration;done.tick=now;
        done.sequence=instance.missionStateRevision;done.missionSequence=instance.lastMissionSequence;
        done.firstRegistryKey=clip.registry;done.slotObjectTag=clip.object;
        done.firstSlotIndex=0;done.firstSlotType=6;
        push_script_event(instance,done);ending_log(instance,"complete");
    }
}
}
