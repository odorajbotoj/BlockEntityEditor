#include "bee/BlockEntityEditor.h"

#include <memory>
#include <optional>
#include <vector>

#include "ll/api/command/runtime/ParamKind.h"
#include "ll/api/command/runtime/RuntimeCommand.h"
#include "ll/api/mod/RegisterHelper.h"

#include "ll/api/command/CommandHandle.h"
#include "ll/api/command/CommandRegistrar.h"
#include "ll/api/command/runtime/RuntimeOverload.h"
#include "ll/api/memory/Memory.h"

#include "mc/dataloadhelper/DefaultDataLoadHelper.h"
#include "mc/nbt/CompoundTag.h"
#include "mc/nbt/CompoundTagVariant.h"
#include "mc/server/commands/CommandBlockName.h"
#include "mc/server/commands/CommandBlockNameResult.h"
#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/server/commands/CommandPermissionLevel.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/actor/BlockActor.h"

namespace {

DefaultDataLoadHelper* helper;

struct PathNode {
    std::string id;
    int         index;
    bool        useIndex;
};

bool parsePath(std::string path, std::vector<PathNode>& vec) {
    for (auto key : ll::string_utils::splitByPattern(path, ".")) {
        if (key.ends_with(']')) try {
                vec.emplace_back(PathNode{
                    std::string{key.substr(0, key.find('['))},
                    std::stoi(std::string{key.substr(key.find('[') + 1, key.length() - key.find('[') - 2)}),
                    true
                });
            } catch (...) {
                return false;
            }
        else vec.emplace_back(PathNode{std::string{key}, 0, false});
    }
    return true;
}

std::optional<CompoundTag> editNbtFromTag(std::unique_ptr<CompoundTag> tag, std::string path, std::string value) {
    std::vector<PathNode>           nodes;
    std::vector<CompoundTagVariant> tags;
    if (!parsePath(path, nodes)) return std::nullopt;
    try {
        tags.emplace_back((*tag)[nodes[0].id]);
        if (nodes[0].useIndex) {
            if (tags.back().is_array() && tags.back().get<ListTag>().getCompound(nodes[0].index))
                tags.emplace_back(*(tags.back().get<ListTag>().getCompound(nodes[0].index)));
            else return std::nullopt;
        }
        for (unsigned long long i = 1; i < nodes.size(); ++i) {
            tags.emplace_back(tags.back()[nodes[i].id]);
            if (nodes[i].useIndex) {
                if (tags.back().is_array() && tags.back().get<ListTag>().getCompound(nodes[i].index))
                    tags.emplace_back(*(tags.back().get<ListTag>().getCompound(nodes[i].index)));
                else return std::nullopt;
            }
        }
        tags.pop_back();
        tags.emplace_back(CompoundTagVariant::parse(value).value());
        long long tagIndex  = tags.size() - 2;
        long long nodeIndex = nodes.size() - 1;
        while (tagIndex >= 0) {
            if (nodes[nodeIndex].useIndex) {
                tags[tagIndex][nodes[nodeIndex].index] = tags[tagIndex + 1].toUnique();
                --tagIndex;
            }
            if (tagIndex < 0) break;
            tags[tagIndex][nodes[nodeIndex].id] = tags[tagIndex + 1];
            --nodeIndex;
            --tagIndex;
        }
        tag->remove(nodes[0].id);
        (*tag)[nodes[0].id] = tags[0];
        return *tag;
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace

namespace bee {

static std::unique_ptr<BlockEntityEditor> instance;

BlockEntityEditor& BlockEntityEditor::getInstance() { return *instance; }

bool BlockEntityEditor::load() {
    getSelf().getLogger().debug("Loading...");
    helper = static_cast<DefaultDataLoadHelper*>(ll::memory::resolveSymbol("??_7DefaultDataLoadHelper@@6B@"));
    if (!helper) {
        getSelf().getLogger().error("Cannot get DefaultDataLoadHelper");
        return false;
    }
    return true;
}

bool BlockEntityEditor::enable() {
    getSelf().getLogger().debug("Enabling...");
    ll::command::CommandRegistrar::getInstance()
        .getOrCreateCommand("bee", "BlockEntity Editor", CommandPermissionLevel::GameDirectors)
        .runtimeOverload()
        .required("pos", ll::command::ParamKind::BlockPos)
        .required("path", ll::command::ParamKind::String)
        .required("value", ll::command::ParamKind::RawText)
        .execute([](CommandOrigin const& origin, CommandOutput& output, ll::command::RuntimeCommand const& self) {
            auto* entity = origin.getEntity();
            if (entity == nullptr || !entity->isType(ActorType::Player))
                return output.error("Only players can run this command");
            auto* player = static_cast<Player*>(entity);
            auto& bs     = player->getDimensionBlockSource();
            auto* be     = bs.getBlockEntity(
                self["pos"].get<ll::command::ParamKind::BlockPos>().getBlockPos(player->getFeetBlockPos())
            );
            if (!be) return output.error("No BlockEntity");
            auto tag = std::make_unique<CompoundTag>();
            be->save(*tag);
            auto newtag = editNbtFromTag(
                std::move(tag),
                self["path"].get<ll::command::ParamKind::String>(),
                self["value"].get<ll::command::ParamKind::RawText>().getText()
            );
            if (newtag.has_value()) {
                // output.success(newtag->toSnbt(SnbtFormat::PrettyChatPrint));
                be->load(player->getLevel(), newtag.value(), *helper);
                return output.success("Success.");
            } else return output.error("Error.");
        });
    ll::command::CommandRegistrar::getInstance()
        .getOrCreateCommand("bec", "BlockEntity Creator", CommandPermissionLevel::GameDirectors)
        .runtimeOverload()
        .required("pos", ll::command::ParamKind::BlockPos)
        .required("block", ll::command::ParamKind::BlockName)
        .required("state", ll::command::ParamKind::BlockState)
        .required("value", ll::command::ParamKind::RawText)
        .execute([](CommandOrigin const& origin, CommandOutput& output, ll::command::RuntimeCommand const& self) {
            auto* entity = origin.getEntity();
            if (entity == nullptr || !entity->isType(ActorType::Player))
                return output.error("Only players can run this command");
            auto* player = static_cast<Player*>(entity);
            auto& bs     = player->getDimensionBlockSource();
            auto  be     = BlockActor::create(
                CompoundTag::fromSnbt(self["value"].get<ll::command::ParamKind::RawText>().getText()).value(),
                self["pos"].get<ll::command::ParamKind::BlockPos>().getBlockPos(player->getFeetBlockPos())
            );
            if (!be) return output.error("Bad BlockEntity");
            auto* b = self["block"]
                          .get<ll::command::ParamKind::BlockName>()
                          .resolveBlock(self["state"].get<ll::command::ParamKind::BlockState>(), output)
                          .getBlock();
            if (!b) return output.error("Bad Block");
            bs.setBlock(
                self["pos"].get<ll::command::ParamKind::BlockPos>().getBlockPos(player->getFeetBlockPos()),
                *b,
                3,
                be,
                nullptr,
                nullptr
            );
        });
    return true;
}

bool BlockEntityEditor::disable() {
    getSelf().getLogger().debug("Disabling...");
    // Code for disabling the mod goes here.
    return true;
}

} // namespace bee

LL_REGISTER_MOD(bee::BlockEntityEditor, bee::instance);
