#pragma once
#include <set>
#include <stdexcept>
#include <yaml-cpp/yaml.h>
#include "Kans3D/Asset/Runtime/ModelData.h"

namespace Kans
{
    // Shared by the human-authored source and KMAT codec. A missing mapping is legacy opaque.
    inline MaterialRenderState ReadMaterialRenderState(const YAML::Node& node)
    {
        MaterialRenderState state;
        if (!node)
            return state;
        if (!node.IsMap())
            throw std::runtime_error("material render state must be a mapping");
        std::set<std::string> fields;
        for (const auto& entry : node)
        {
            const auto name = entry.first.as<std::string>();
            if (!fields.insert(name).second ||
                !std::set<std::string> {"alpha_mode", "alpha_cutoff", "double_sided", "receive_shadows", "temporal"}
                     .count(name))
                throw std::runtime_error("unknown or duplicate render field '" + name + "'");
        }
        if (node["alpha_mode"])
        {
            const auto mode = node["alpha_mode"].as<std::string>();
            if (mode == "mask")
                state.AlphaMode = MaterialAlphaMode::Mask;
            else if (mode == "blend")
                state.AlphaMode = MaterialAlphaMode::Blend;
            else if (mode == "dithered")
                state.AlphaMode = MaterialAlphaMode::Dithered;
            else if (mode == "additive")
                state.AlphaMode = MaterialAlphaMode::Additive;
            else if (mode == "premultiplied")
                state.AlphaMode = MaterialAlphaMode::Premultiplied;
            else if (mode != "opaque")
                throw std::runtime_error("invalid material alpha_mode '" + mode + "'");
        }
        if (node["temporal"])
        {
            const auto response = node["temporal"].as<std::string>();
            if (response == "reactive")
                state.TemporalResponse = MaterialTemporalResponse::Reactive;
            else if (response == "composition")
                state.TemporalResponse = MaterialTemporalResponse::Composition;
            else if (response != "default")
                throw std::runtime_error("invalid material temporal response '" + response + "'");
        }
        if (node["alpha_cutoff"])
            state.AlphaCutoff = node["alpha_cutoff"].as<float>();
        if (node["double_sided"])
            state.DoubleSided = node["double_sided"].as<bool>();
        if (node["receive_shadows"])
            state.ReceiveShadows = node["receive_shadows"].as<bool>();
        if (!IsValidMaterialRenderState(state))
            throw std::runtime_error("invalid material render state (cutoff must be finite and non-negative)");
        return state;
    }

    inline void EmitMaterialRenderState(YAML::Emitter& out, const MaterialRenderState& state)
    {
        if (!IsValidMaterialRenderState(state))
            throw std::runtime_error("invalid material render state");
        const char* mode = state.AlphaMode == MaterialAlphaMode::Mask          ? "mask" :
                           state.AlphaMode == MaterialAlphaMode::Blend         ? "blend" :
                           state.AlphaMode == MaterialAlphaMode::Dithered      ? "dithered" :
                           state.AlphaMode == MaterialAlphaMode::Additive      ? "additive" :
                           state.AlphaMode == MaterialAlphaMode::Premultiplied ? "premultiplied" :
                                                                                 "opaque";
        out << YAML::Key << "render" << YAML::Value << YAML::Flow << YAML::BeginMap << YAML::Key << "alpha_mode"
            << YAML::Value << mode << YAML::Key << "alpha_cutoff" << YAML::Value << state.AlphaCutoff << YAML::Key
            << "double_sided" << YAML::Value << state.DoubleSided;
        if (!state.ReceiveShadows)
            out << YAML::Key << "receive_shadows" << YAML::Value << false;
        if (state.TemporalResponse != MaterialTemporalResponse::Default)
            out << YAML::Key << "temporal" << YAML::Value
                << (state.TemporalResponse == MaterialTemporalResponse::Reactive ? "reactive" : "composition");
        out << YAML::EndMap;
    }
} // namespace Kans
