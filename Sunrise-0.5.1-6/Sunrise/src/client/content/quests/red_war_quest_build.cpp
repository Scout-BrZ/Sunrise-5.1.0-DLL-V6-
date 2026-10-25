#include "red_war_quest_build.h"
#include <Windows.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <span>
#include <utility>
#include <vector>
#include "../../../core/logging/log.h"
#include "../../../state/build_data/quests/red_war_quest_catalog.h"
#include "../../../middleware/content/packages/tables/definition_index_table.h"
#include "../../../middleware/content/packages/tables/quest_initialization_reader.h"

namespace sunrise::client::content::quests {
namespace {
namespace reader = middleware::content::packages::reader;
namespace tables = middleware::content::packages::tables;
namespace domain = state::build_data::quests;

// Leitura limitada: nenhum ponteiro relativo do pacote e desreferenciado diretamente.
template<class T> bool read(std::span<const std::byte> bytes, std::size_t offset, T& out) noexcept {
    if (offset > bytes.size() || sizeof out > bytes.size() - offset) { return false; }
    std::memcpy(&out, bytes.data() + offset, sizeof out);
    return true;
}
bool block(std::span<const std::byte> bytes, std::size_t field,
           std::uint32_t expectedClass, std::size_t& offset) noexcept {
    std::int64_t relative{};
    if (!read(bytes, field, relative) || relative == 0
        || relative < 4 - static_cast<std::int64_t>(field)
        || relative > static_cast<std::int64_t>(bytes.size()) - static_cast<std::int64_t>(field) - 32) {
        return false;
    }
    offset = static_cast<std::size_t>(static_cast<std::int64_t>(field) + relative);
    std::uint32_t cls{};
    return read(bytes, offset - 4, cls) && cls == expectedClass;
}
bool checked_array(std::span<const std::byte> bytes, std::size_t at, std::size_t stride,
                   std::uint32_t expectedClass, tables::Array& rows) noexcept {
    return tables::find_array_at(bytes, at, rows) && rows.elementClass == expectedClass
        && rows.dataOffset <= bytes.size() && rows.count <= 16384
        && rows.count <= (bytes.size() - rows.dataOffset) / stride;
}
bool extract(const reader::Source& source, reader::Scratch& scratch,
             std::vector<domain::Step>& steps, std::vector<domain::CompletionObjective>& completed,
             std::uint32_t& ownerHash,
             std::uint16_t& trackingSlot) {
    std::vector<std::byte> index, item, owner, values;
    std::uint32_t cls{};
    tables::Array items{};
    if (!reader::read_tag(source, scratch, 0x81327CCBU, index, cls)
        || cls != 0x80807BE4U
        || !checked_array(index, 8, 24, tables::kItemIndexTableClass, items)) { return false; }
    // Nao se usam indices de atividade ou item copiados de outra versao.
    std::uint16_t acquisition = UINT16_MAX;
    tables::IndexRow acquisitionRow{};
    for (std::size_t i = 0; i < items.count; ++i) {
        tables::IndexRow row{};
        if (!tables::index_row(index, items, i, row)) { return false; }
        if (row.definitionHash == domain::kRedWarAcquisitionHash) {
            if (acquisition != UINT16_MAX) { return false; }
            acquisition = static_cast<std::uint16_t>(i);
            acquisitionRow = row;
        }
    }
    if (acquisition == UINT16_MAX
        || !reader::read_tag(source, scratch, acquisitionRow.targetTag, item, cls)
        || cls != tables::kItemDefinitionClass) { return false; }
    std::size_t set{};
    ownerHash = acquisitionRow.definitionHash;
    if (block(item, 96, 0x808077C8U, set)) {
        owner = std::move(item);
    } else {
        const auto parent = tables::items::quest_parent(item);
        tables::IndexRow parentRow{};
        if (parent >= items.count || !tables::index_row(index, items, parent, parentRow)
            || !reader::read_tag(source, scratch, parentRow.targetTag, owner, cls)
            || cls != tables::kItemDefinitionClass
            || !block(owner, 96, 0x808077C8U, set)) { return false; }
        ownerHash = parentRow.definitionHash;
    }
    tables::Array members{};
    if (!read(owner, set + 16, trackingSlot)
        || !checked_array(owner, set, 8, 0x808077CAU, members)
        || members.count == 0 || members.count > 256
        || !reader::read_tag(source, scratch, 0x81319320U, values, cls)
        || cls != 0x80807C80U) { return false; }
    bool found{};
    std::uint8_t selectedBank{};
    std::uint16_t selectedRow{};
    for (unsigned bank = 0; bank < 4; ++bank) {
        const std::size_t field = 8 + bank * 16;
        std::uint64_t count{};
        if (!read(values, field, count)) { return false; }
        if (count == 0) { continue; }
        tables::Array rows{};
        if (!checked_array(values, field, 8, 0x80807C8DU, rows)) { return false; }
        for (std::size_t row = 0; row < rows.count; ++row) {
            std::uint16_t slot{};
            if (!read(values, rows.dataOffset + row * 8 + 4, slot)) { return false; }
            if (slot != trackingSlot) { continue; }
            if (found || bank > 1) { return false; }
            found = true;
            selectedBank = static_cast<std::uint8_t>(bank);
            selectedRow = static_cast<std::uint16_t>(row);
        }
    }
    if (!found) { return false; }
    bool acquisitionMember{};
    for (std::size_t i = 0; i < members.count; ++i) {
        const auto at = members.dataOffset + i * 8;
        std::int32_t value{};
        std::uint16_t itemIndex{}, reserved{};
        if (!read(owner, at, value) || !read(owner, at + 4, itemIndex)
            || !read(owner, at + 6, reserved) || reserved != 0) { return false; }
        if (itemIndex == UINT16_MAX) { continue; }
        tables::IndexRow row{};
        if (itemIndex >= items.count || !tables::index_row(index, items, itemIndex, row)) { return false; }
        acquisitionMember = acquisitionMember || itemIndex == acquisition;
        steps.push_back({row.definitionHash, value, static_cast<std::uint16_t>(i), selectedRow, selectedBank});
    }
    if (!acquisitionMember || steps.size() < 3) { return false; }
    std::vector<std::byte> objectiveTable;
    tables::Array objectiveRows{};
    if (!reader::read_tag(source, scratch, 0x81319344U, objectiveTable, cls)
        || cls != 0x8080775BU
        || !checked_array(objectiveTable, 8, 160, 0x8080775FU, objectiveRows)) { return false; }
    // Homecoming, Adieu e a cinematica da Chacara concluem respectivamente
    // os membros 1, 2 e 3 da jornada. O membro 3 libera Tyra Karn.
    for (std::size_t ordinal = 0; ordinal < 4; ++ordinal) {
        std::uint16_t itemIndex{};
        if (!read(owner, members.dataOffset + ordinal * 8 + 4, itemIndex)
            || itemIndex >= items.count) { return false; }
        tables::IndexRow row{};
        if (!tables::index_row(index, items, itemIndex, row)
            || !reader::read_tag(source, scratch, row.targetTag, item, cls)
            || cls != tables::kItemDefinitionClass) { return false; }
        std::size_t objectiveBlock{};
        tables::Array refs{};
        std::uint16_t objectiveIndex{};
        if (!block(item, 48, 0x808077EBU, objectiveBlock)
            || !checked_array(item, objectiveBlock, 2, 0x808087B1U, refs)
            || refs.count != 1 || !read(item, refs.dataOffset, objectiveIndex)
            || objectiveIndex >= objectiveRows.count) { return false; }
        const auto at = objectiveRows.dataOffset + objectiveIndex * 160;
        tables::Array expression{};
        std::uint32_t opcode{}, objectiveSlot{};
        std::int32_t threshold{};
        if (!read(objectiveTable, at + 48, threshold) || threshold <= 0
            || !checked_array(objectiveTable, at + 8, 8, 0x80807D31U, expression)
            || expression.count != 1 || !read(objectiveTable, expression.dataOffset, opcode)
            || !read(objectiveTable, expression.dataOffset + 4, objectiveSlot)
            || opcode != 10 || objectiveSlot >= 15500) { return false; }
        bool bound{};
        domain::CompletionObjective objective{};
        objective.itemHash = row.definitionHash;
        objective.threshold = threshold;
        objective.ordinal = static_cast<std::uint16_t>(ordinal);
        objective.slot = static_cast<std::uint16_t>(objectiveSlot);
        objective.objectiveIndex = objectiveIndex;
        for (unsigned bank = 0; bank < 4; ++bank) {
            const auto field = 8 + bank * 16;
            std::uint64_t count{};
            if (!read(values, field, count)) { return false; }
            if (count == 0) { continue; }
            tables::Array rows{};
            if (!checked_array(values, field, 8, 0x80807C8DU, rows)) { return false; }
            for (std::size_t i = 0; i < rows.count; ++i) {
                std::uint16_t slot{};
                if (!read(values, rows.dataOffset + i * 8 + 4, slot)) { return false; }
                if (slot != objective.slot) { continue; }
                if (bound || bank > 1) { return false; }
                objective.bank = static_cast<std::uint8_t>(bank);
                objective.row = static_cast<std::uint16_t>(i);
                bound = true;
            }
        }
        if (!bound) {
            // Como no project_active_progress do Dawn: slot sem binding usa a lista avulsa.
            objective.bank = 2;
            objective.row = UINT16_MAX;
        }
        completed.push_back(objective);
    }
    return true;
}
} // namespace
bool build(const reader::Source& source, reader::Scratch& scratch) noexcept {
    if (domain::ready()) { return true; }
    static ULONGLONG retryAt{};
    const auto now = GetTickCount64();
    if (now < retryAt) { return false; }
    retryAt = now + 5000;
    std::vector<domain::Step> steps;
    std::vector<domain::CompletionObjective> completed;
    std::uint32_t ownerHash{};
    std::uint16_t slot{};
    bool ok{};
    try {
        ok = extract(source, scratch, steps, completed, ownerHash, slot);
        const auto count = steps.size();
        if (ok) { ok = domain::publish(std::move(steps), std::move(completed)); }
        if (ok) {
            core::log::writef(core::log::Channel::client, core::log::Level::info,
                "ev=red_war_quest stage=catalog result=ready owner=0x%08X slot=%u steps=%zu",
                ownerHash, static_cast<unsigned>(slot), count);
            return true;
        }
    } catch (const std::bad_alloc&) {
        // A leitura opcional nao impede carregar os outros dominios do Sunrise.
    }
    core::log::write(core::log::Channel::client, core::log::Level::warn,
        "ev=red_war_quest stage=catalog result=unavailable");
    return false;
}
} // namespace sunrise::client::content::quests
