#pragma once
//------core-----------------
#include "Kans3D/Core/Application.h"
#include "Kans3D/Core/Layer.h"
#include "Kans3D/Core/Log/Log.h"
#include "Kans3D/Core/Math/Math.h"
#include "Kans3D/Core/TimeStep.h"

#include "Kans3D/ImGui/ImGuiLayer.h"
//------Input---------------
#include "Kans3D/Input/KeyCodes.h"

#include "Kans3D/Input/Input.h"
//------Entity/Scene---------------
#include "Kans3D/Scene/Components.h"
#include "Kans3D/Scene/Entity.h"
#include "Kans3D/Scene/Scene.h"
#include "Kans3D/Scene/SceneSerializer.h"
//------UI---------------
#include "Kans3D/ImGui/KansUI.h"
//------Renderer---------------

#include "Kans3D/Renderer/Renderer.h"
#include "Kans3D/Renderer/SpriteBatch.h"

//-------Render Resource-----
#include "Kans3D/Renderer/Resource/OrthographicCamera.h"
#include "Kans3D/Renderer/Resource/OrthographicCameraController.h"
#include "Kans3D/Renderer/Resource/Texture.h"

//-----script---------------
#include "Kans3D/Scene/ScriptableEntity.h"

//------渲染效果---------------
#include "Kans3D/VFX/VfxWorld.h"
//------辅助工具---------------
#include "Kans3D/Debug/Instrumentor.h"
//------程序入口点--------------

// #include"Kans3D/Core/EntryPoint.h"
//------------------------------

#include "Kans3D/Geometry/MeshOperations.h"
