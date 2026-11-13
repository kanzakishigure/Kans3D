#pragma once

namespace Kans::ShaderText
{
#define KANS_SHADER_TEXT(id, name, text) extern const char* const id;
#include "EngineShaderTexts.inc"
#undef KANS_SHADER_TEXT
} // namespace Kans::ShaderText
