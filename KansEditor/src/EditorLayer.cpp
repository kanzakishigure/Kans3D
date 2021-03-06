#include "EditorLayer.h"
#include <imgui.h>
#include "Platform/OpenGL/OpenGLShader.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
static const int S_mapwidth = 24;
static const char* S_mapTiles= "";
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

namespace Hazel
{


	EditorLayer::EditorLayer()
		:m_CameraController(1920.0f / 1080.0f), Layer("EditorLayer")
	{

	}


	void EditorLayer::OnAttach()
	{
		HZ_PROFILE_FUCTION();
		

		//FrameBuffer init
		{
			FrameBufferSpecification spec;
			spec.Width = Application::Get().GetWindow().GetWidth();
			spec.Height = Application::Get().GetWindow().GetHeight();
											//Color                           normal													Depth
			spec.AttachmentSpecification = { FrameBufferTextureFormat::RGBA8 ,FrameBufferTextureFormat::RGBA8,FrameBufferTextureFormat::Depth };
			m_Framebuffer = FrameBuffer::Create(spec);

		}
	
		// scene init
		{
			//Create Scene
			{
				m_ActiveScene = CreateRef<Scene>();
				auto pointlight = m_ActiveScene->CreateEntity("PointLight");

				auto& plightCMP =pointlight.AddComponent<PointLightComponent>();
				plightCMP.Diffuse_Intensity = glm::vec3(1.0);
				plightCMP.Specular_Intensity = glm::vec3(1.0);
				plightCMP.Ambient_Intensity = glm::vec3(1.0);
				auto& CMP = pointlight.GetComponent<TransformComponent>();
				CMP.Position = { 10.0,20.0,3.3 };
				
				auto DirLight = m_ActiveScene->CreateEntity("DirLight");
				auto& dirCMP = DirLight.AddComponent<DirLightComponent>();
				dirCMP.Diffuse_Intensity = glm::vec3(1.0);
				dirCMP.Specular_Intensity = glm::vec3(1.0);
				dirCMP.Ambient_Intensity = glm::vec3(1.0);
			}
			{
				auto RefEntity = m_ActiveScene->CreateEntity("RefEntity");
				auto& spritCMP = RefEntity.AddComponent<SpriteRendererComponent>();
				spritCMP.Texture = Hazel::Texture2D::Create("F:/Kans3D/Hazel/KansEditor/assets/textures/GY.png");
				auto& transformCMP = RefEntity.GetComponent<TransformComponent>();
				transformCMP.Scale = {19.2f,10.8f,1.0f};
				transformCMP.Position = { 2.3f,0.0f,-12.0f};
			}
			//create Entity
#if 0
			{
				auto SunEntity = m_ActiveScene->CreateEntity("Entity A");
				auto& spritCMP = SunEntity.AddComponent<SpriteRendererComponent>(glm::vec4{ 244.0f/256.0f, 185.0f/256.0f, 116.0f/256.0f,1.0f });
				//spritCMP.Texture = Hazel::Texture2D::Create("F:/Kans3D/Hazel/KansEditor/assets/textures/Checkerboard.png");
			}
			{
				auto RedEntity = m_ActiveScene->CreateEntity("Entity B");
				auto& spritCMP  = RedEntity.AddComponent<SpriteRendererComponent>(glm::vec4{ 151.0f / 256.0f, 52.0f / 256.0f, 68.0f / 256.0f,1.0f });
				spritCMP.Texture = Hazel::Texture2D::Create("F:/Kans3D/Hazel/KansEditor/assets/textures/Checkerboard.png");
			}
#endif
			//Scene Renderer Init
			{
				m_StaticMeshRenderer = CreateRef<SceneRenderer>(m_ActiveScene);
				m_StaticMeshRenderer->SetFrameBuffer(m_Framebuffer);
			}
			//Create Scene camera
			{
				m_CameraEntity = m_ActiveScene->CreateEntity("mainCamera");
				auto& transfromCMP =  m_CameraEntity.GetComponent<TransformComponent>();
				transfromCMP.Position = { 0.0f,0.7f,1.5f };
				auto& cmp = m_CameraEntity.AddComponent<CameraComponent>();
				cmp.SceneCamera.SetViewportSize(1920, 1080);
			}

			// createMesh test
#if 1
			{
				if(0)
				{
					auto DBYEntity = m_ActiveScene->CreateEntity("DBY");
					auto& meshCMP = DBYEntity.AddComponent<StaticMeshComponent>();
					DBYEntity.AddComponent<MaterialComponent>();
					auto meshSrouce = CreateRef<MeshSource>("F:/Kans3D/Hazel/KansEditor/assets/model/dby/dby.fbx");
					meshCMP.StaticMesh = CreateRef<StaticMesh>(meshSrouce);
					meshCMP.MaterialTable = meshCMP.StaticMesh->GetMaterials();
					auto& TransformCMP = DBYEntity.GetComponent<TransformComponent>();
					TransformCMP.Position = { -0.6f,-1.6f,-2.5f };
					TransformCMP.Rotation = { glm::radians(-20.0f),0.0f,0.0f };
					TransformCMP.Scale = { glm::vec3(0.15f) };
				}
				if (0)
				{
					auto GYEntity = m_ActiveScene->CreateEntity("GY");
					auto& meshCMP = GYEntity.AddComponent<StaticMeshComponent>();
					auto& materialCMP = GYEntity.AddComponent<MaterialComponent>();
					auto meshSrouce = CreateRef<MeshSource>("F:/Kans3D/Hazel/KansEditor/assets/model/GY/GY.fbx");
					meshCMP.StaticMesh = CreateRef<StaticMesh>(meshSrouce);
					meshCMP.MaterialTable = meshCMP.StaticMesh->GetMaterials();
					materialCMP.MaterialTable = meshCMP.MaterialTable;
					auto& TransformCMP = GYEntity.GetComponent<TransformComponent>();
					TransformCMP.Position = { 0.0f,-1.2f,0.2f };
					TransformCMP.Rotation = { glm::radians(-20.0f),0.0f,glm::radians(0.0f)};
					TransformCMP.Scale = { glm::vec3(0.15f) };

					//Init Material
					{
						int matCount = materialCMP.MaterialTable->GetMaterialCount();
						for (uint32_t i = 0; i < matCount; i++)
						{
							auto& mat = materialCMP.MaterialTable->GetMaterialAsset(i)->GetMaterial();
							mat->Set("U_ShadowMultColor", glm::vec4(1.0));
							mat->Set("U_DarkShadowMultColor", glm::vec4(1.0));						
							mat->Set("U_ShadowArea", 0.5f);
							mat->Set("U_DarkShadowArea", 0.5f);
							mat->Set("U_FixDarkShadow", 0.5f);
							mat->Set("U_ShadowSmooth", 0.5f);
							mat->Set("U_DarkShadowSmooth", 0.5f);
							mat->Set("U_RampShadowRange", 0.5f);
						
						}
					}
				}
				if (1)
				{
					auto GY_LightEntity = m_ActiveScene->CreateEntity("GY_Light");
					auto& meshCMP = GY_LightEntity.AddComponent<StaticMeshComponent>();
					auto& materialCMP = GY_LightEntity.AddComponent<MaterialComponent>();
					auto meshSrouce = CreateRef<MeshSource>("F:/Kans3D/Hazel/KansEditor/assets/model/GY_Light/GY_Light.fbx");
					meshCMP.StaticMesh = CreateRef<StaticMesh>(meshSrouce);
					meshCMP.MaterialTable = meshCMP.StaticMesh->GetMaterials();
					materialCMP.MaterialTable = meshCMP.MaterialTable;
					auto& TransformCMP = GY_LightEntity.GetComponent<TransformComponent>();
					TransformCMP.Position = { 0.0f,-1.2f,-3.0f };
					TransformCMP.Rotation = { glm::radians(-20.0f),0.0f,glm::radians(0.0f) };
					TransformCMP.Scale = { glm::vec3(0.15f) };

					//Init Material
					MaterialUtil::InitMaterial(materialCMP.MaterialTable);
				}
				if (0)
				{
					auto YLEntity = m_ActiveScene->CreateEntity("YL");
					auto& meshCMP = YLEntity.AddComponent<StaticMeshComponent>();
					auto& materialCMP = YLEntity.AddComponent<MaterialComponent>();
					auto meshSrouce = CreateRef<MeshSource>("F:/Kans3D/Hazel/KansEditor/assets/model/YL/YL.fbx");
					meshCMP.StaticMesh = CreateRef<StaticMesh>(meshSrouce);
					meshCMP.MaterialTable = meshCMP.StaticMesh->GetMaterials();
					materialCMP.MaterialTable = meshCMP.MaterialTable;
					auto& TransformCMP = YLEntity.GetComponent<TransformComponent>();
					TransformCMP.Position = { 0.0f,-1.2f,0.2f };
					TransformCMP.Rotation = { glm::radians(-20.0f),0.0f,glm::radians(0.0f) };
					TransformCMP.Scale = { glm::vec3(0.15f) };

					//Init Material
					{
						int matCount = materialCMP.MaterialTable->GetMaterialCount();
						for (uint32_t i = 0; i < matCount; i++)
						{
							auto& mat = materialCMP.MaterialTable->GetMaterialAsset(i)->GetMaterial();
							mat->Set("U_ShadowMultColor", glm::vec4(1.0));
							mat->Set("U_DarkShadowMultColor", glm::vec4(1.0));
							mat->Set("U_ShadowArea", 0.5f);
							mat->Set("U_DarkShadowArea", 0.5f);
							mat->Set("U_FixDarkShadow", 0.5f);
							mat->Set("U_ShadowSmooth", 0.5f);
							mat->Set("U_DarkShadowSmooth", 0.5f);

						}
					}
				}
				if (0)
				{
					auto BOXEntity = m_ActiveScene->CreateEntity("BOX");
					auto& meshCMP = BOXEntity.AddComponent<StaticMeshComponent>();
					BOXEntity.AddComponent<MaterialComponent>();
					auto meshSrouce = CreateRef<MeshSource>("F:/Kans3D/Hazel/KansEditor/assets/model/BOX/BOX.fbx");
					meshCMP.StaticMesh = CreateRef<StaticMesh>(meshSrouce);
					meshCMP.MaterialTable = meshCMP.StaticMesh->GetMaterials();
					auto& TransformCMP = BOXEntity.GetComponent<TransformComponent>();
					TransformCMP.Position = { 0.0f,0.0f,-20.0f };
					TransformCMP.Rotation = { 0.0f,0.0f,0.0f };
					TransformCMP.Scale = { glm::vec3(0.15f) };
				}
				if (0)
				{
					auto YLEntity = m_ActiveScene->CreateEntity("YL");
					auto& meshCMP = YLEntity.AddComponent<StaticMeshComponent>();
					YLEntity.AddComponent<MaterialComponent>();
					auto meshSrouce = CreateRef<MeshSource>("F:/Kans3D/Hazel/KansEditor/assets/model/YL/YL.fbx");
					meshCMP.StaticMesh = CreateRef<StaticMesh>(meshSrouce);
					meshCMP.MaterialTable = meshCMP.StaticMesh->GetMaterials();
					auto& TransformCMP = YLEntity.GetComponent<TransformComponent>();
					TransformCMP.Position = { 0.0f,0.0f,-20.0f };
					TransformCMP.Rotation = { 0.0f,0.0f,0.0f };
					TransformCMP.Scale = { glm::vec3(0.15f) };
				}
#endif
#if 0
				{
					auto nanosuitEntity = m_ActiveScene->CreateEntity("nanosuit");
					auto& meshCMP = nanosuitEntity.AddComponent<StaticMeshComponent>();
					auto meshSrouce = CreateRef<MeshSource>("F:/Kans3D/Hazel/KansEditor/assets/model/nanosuit/nanosuit.obj");
					meshCMP.StaticMesh = CreateRef<StaticMesh>(meshSrouce);
					meshCMP.MaterialTable = meshCMP.StaticMesh->GetMaterials();
					auto& TransformCMP = nanosuitEntity.GetComponent<TransformComponent>();
					TransformCMP.Position = { 0.0f,-1.6f,-2.5f };
					TransformCMP.Rotation = { glm::radians(-20.0f),0.0f,0.0f };
					TransformCMP.Scale = { glm::vec3(0.15f) };
				}
				
#endif // DEBUG
			}
		
			//Native Script
			{
				class CameracontorlScript : public ScriptableEntity
				{
				public:
					void OnCreate() override
					{

					}

					void OnUpdate(TimeStep ts)override
					{

						auto& transformCMP = GetComponent<TransformComponent>();
						if (Input::IsKeyPressed(Key::W))
						{
							transformCMP.Position.y += ts * 5.0f;
						}
						if (Input::IsKeyPressed(Key::S))
						{
							transformCMP.Position.y -= ts * 5.0f;
						}
						if (Input::IsKeyPressed(Key::A))
						{
							transformCMP.Position.x -= ts * 5.0f;

						}
						if (Input::IsKeyPressed(Key::D))
						{
							transformCMP.Position.x += ts * 5.0f;
						}

					}

					void OnDestory() override
					{
					}
				};


				m_CameraEntity.AddComponent<NativeScriptComponent>().Bind<CameracontorlScript>();

				//HierachyPanel
				{
					m_SceneHierachyPanel = { m_ActiveScene };
				}
			}

		}

		
	}

	void EditorLayer::OnDetach()
	{
		HZ_PROFILE_FUCTION();
		HZ_CORE_INFO("call detach");
		Renderer2D::Shutdown();
		
	}

	void EditorLayer::OnUpdate(TimeStep ts)
	{
		HZ_PROFILE_FUCTION();

		//resize
		{
			if (FrameBufferSpecification spec = m_Framebuffer->GetSpecification();
				m_ViewportSize.x > 0.0f && m_ViewportSize.y > 0.0f &&
				(spec.Width != m_ViewportSize.x || spec.Height != m_ViewportSize.y))
			{
				m_Framebuffer->Resize((uint32_t)m_ViewportSize.x, (uint32_t)m_ViewportSize.y);
				m_CameraController.OnResize((uint32_t)m_ViewportSize.x, (uint32_t)m_ViewportSize.y);
				m_ActiveScene->OnViewportResize((uint32_t)m_ViewportSize.x, (uint32_t)m_ViewportSize.y);
			}
		}
	
		//update
		{
			if(!m_ViewportFocused)
			m_CameraController.OnUpdate(ts);

		}
		//renderer
		Renderer2D::ResetStats();
		m_Framebuffer->Bind();
		RenderCommand::SetClearColor({ 0.02f, 0.02f, 0.02f, 1.0f });
		RenderCommand::Clear();
		
		//updateScene
		{
			m_ActiveScene->OnUpdate(ts);
		}

		//rendering
		{
			HZ_PROFILE_SCOPE("rendering")


			m_ActiveScene->OnRenderer(m_StaticMeshRenderer,ts);
			m_Framebuffer->Unbind();

		}
	}

	void EditorLayer::OnEvent(Event& e)
	{
		HZ_PROFILE_FUCTION();
		m_CameraController.OnEvent(e);
	}

	void EditorLayer::OnImGuiRender()
	{
		HZ_PROFILE_FUCTION();
		
//Imgui docking code
#if 1
		static bool p_open = true;
		static bool opt_fullscreen = true;
		static bool opt_padding = false;
		static ImGuiDockNodeFlags dockspace_flags = ImGuiDockNodeFlags_None;

		// We are using the ImGuiWindowFlags_NoDocking flag to make the parent window not dockable into,
		// because it would be confusing to have two docking targets within each others.
		ImGuiWindowFlags window_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;
		if (opt_fullscreen)
		{
			ImGuiViewport* viewport = ImGui::GetMainViewport();
			ImGui::SetNextWindowPos(viewport->GetWorkPos());
			ImGui::SetNextWindowSize(viewport->GetWorkSize());
			ImGui::SetNextWindowViewport(viewport->ID);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
			window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
			window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
		}
		else
		{
			dockspace_flags &= ~ImGuiDockNodeFlags_PassthruCentralNode;
		}

		// When using ImGuiDockNodeFlags_PassthruCentralNode, DockSpace() will render our background
		// and handle the pass-thru hole, so we ask Begin() to not render a background.
		if (dockspace_flags & ImGuiDockNodeFlags_PassthruCentralNode)
			window_flags |= ImGuiWindowFlags_NoBackground;

		// Important: note that we proceed even if Begin() returns false (aka window is collapsed).
		// This is because we want to keep our DockSpace() active. If a DockSpace() is inactive,
		// all active windows docked into it will lose their parent and become undocked.
		// We cannot preserve the docking relationship between an active window and an inactive docking, otherwise
		// any change of dockspace/settings would lead to windows being stuck in limbo and never being visible.
		if (!opt_padding)
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		ImGui::Begin("DockSpace Demo", &p_open, window_flags);
		if (!opt_padding)
			ImGui::PopStyleVar();

		if (opt_fullscreen)
			ImGui::PopStyleVar(2);

		// DockSpace
		//set the min docking windowsSize tO 340
		auto& style = ImGui::GetStyle();
		float minsize = style.WindowMinSize.x;
		style.WindowMinSize.x = 340;
		ImGuiIO& io = ImGui::GetIO();
		if (io.ConfigFlags & ImGuiConfigFlags_DockingEnable)
		{
			ImGuiID dockspace_id = ImGui::GetID("MyDockSpace");
			ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), dockspace_flags);
		}
		style.WindowMinSize.x = minsize;

		if (ImGui::BeginMenuBar())
		{
			if (ImGui::BeginMenu("Options"))
			{
				// Disabling fullscreen would allow the window to be moved to the front of other windows,
				// which we can't undo at the moment without finer window depth/z control.
				ImGui::MenuItem("Fullscreen", NULL, &opt_fullscreen);
				ImGui::MenuItem("Padding", NULL, &opt_padding);
				ImGui::Separator();

				if (ImGui::MenuItem("Flag: NoSplit", "", (dockspace_flags & ImGuiDockNodeFlags_NoSplit) != 0)) { dockspace_flags ^= ImGuiDockNodeFlags_NoSplit; }
				if (ImGui::MenuItem("Flag: NoResize", "", (dockspace_flags & ImGuiDockNodeFlags_NoResize) != 0)) { dockspace_flags ^= ImGuiDockNodeFlags_NoResize; }
				if (ImGui::MenuItem("Flag: NoDockingInCentralNode", "", (dockspace_flags & ImGuiDockNodeFlags_NoDockingInCentralNode) != 0)) { dockspace_flags ^= ImGuiDockNodeFlags_NoDockingInCentralNode; }
				if (ImGui::MenuItem("Flag: AutoHideTabBar", "", (dockspace_flags & ImGuiDockNodeFlags_AutoHideTabBar) != 0)) { dockspace_flags ^= ImGuiDockNodeFlags_AutoHideTabBar; }
				if (ImGui::MenuItem("Flag: PassthruCentralNode", "", (dockspace_flags & ImGuiDockNodeFlags_PassthruCentralNode) != 0, opt_fullscreen)) { dockspace_flags ^= ImGuiDockNodeFlags_PassthruCentralNode; }
				ImGui::Separator();

				if (ImGui::MenuItem("Close", NULL, false, &p_open != NULL))
				{
					p_open = false;
					Application::Get().Close();
				}
				ImGui::EndMenu();
			}


			ImGui::EndMenuBar();
		}
		ImGui::End();
#endif


	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
	
	{
		ImGui::Begin("ProjectSpecication");

		//BlockEvent
		m_ViewportFocused = ImGui::IsWindowFocused();
		m_viewprotHovered = ImGui::IsWindowHovered();
		Application::Get().GetImGuiLayer()->BlockEvents(m_ViewportFocused||m_viewprotHovered);
		//renderStats
		ImGui::Separator();
		ImGui::Text("Render2DStats");
		ImGui::Text("DrawCalls: %d", Renderer2D::GetStats().DrawCalls);
		ImGui::Text("QuadCount: %d", Renderer2D::GetStats().QuadCount);
		ImGui::Text("TotalVertexCount: %d", Renderer2D::GetStats().GetTotalVertexCount());
		ImGui::Separator();
		ImGui::End();

	}
	//HierachyPanel
	m_SceneHierachyPanel.OnImguiRender();
	//Viewport
		//Color FrameBuffer
		if (1)
		{
			ImGui::Begin("ViewPort1");
			ImVec2 viewportsize = ImGui::GetContentRegionAvail();
			if (m_ViewportSize != *(glm::vec2*) & viewportsize)
			{
				
				m_ViewportSize = { viewportsize.x,viewportsize.y };
				//m_Framebuffer->Resize((uint32_t)viewportsize.x, (uint32_t)viewportsize.y);
				//m_CameraController.OnResize((uint32_t)viewportsize.x, (uint32_t)viewportsize.y);
			}
			uint64_t colorframebufferID = (uint64_t)m_Framebuffer->GetColorAttachmentRendererID();
			ImGui::Image((void*)colorframebufferID, viewportsize, ImVec2(0, 1), ImVec2(1, 0));

			ImGui::End();
		}

		//Depth FrameBuffer
		if(1)
		{
			ImGui::Begin("ViewPort2");
			ImVec2 viewportsize = ImGui::GetContentRegionAvail();

			if (m_ViewportSize != *(glm::vec2*) & viewportsize)
			{
				//HZ_INFO("ViewportSize:  {0}   {1}", viewportsize.x, viewportsize.y);
				m_ViewportSize = { viewportsize.x,viewportsize.y };
				//m_Framebuffer->Resize(viewportsize.x, viewportsize.y);
			}


			uint64_t depthframebufferID =(uint64_t)m_Framebuffer->GetColorAttachmentRendererID(1);
			ImGui::Image((void*)depthframebufferID, viewportsize, ImVec2(0, 1), ImVec2(1, 0));

			ImGui::End();
		}

	ImGui::PopStyleVar(1);
		


	}

}


