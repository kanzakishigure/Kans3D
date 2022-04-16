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
			m_Framebuffer = FrameBuffer::Create(spec);

		}
	
		// scene init
		{
			//Create Scene
			m_ActiveScene = CreateRef<Scene>();
			//create Entity
#if 1
			{
				auto SunEntity = m_ActiveScene->CreateEntity("Entity A");
				auto& spritCMP = SunEntity.AddComponent<SpriteRendererComponent>(glm::vec4{ 244.0f/256.0f, 185.0f/256.0f, 116.0f/256.0f,1.0f });
				//spritCMP.Texture = Hazel::Texture2D::Create("assets/textures/hutao.png");
			}
			{
				auto RedEntity = m_ActiveScene->CreateEntity("Entity B");
				auto& spritCMP  = RedEntity.AddComponent<SpriteRendererComponent>(glm::vec4{ 151.0f / 256.0f, 52.0f / 256.0f, 68.0f / 256.0f,1.0f });
				spritCMP.Texture = Hazel::Texture2D::Create("F:/Kans3D/Hazel/KansEditor/assets/textures/Checkerboard.png");
			}
#endif
			
			//Create Scene camera
			{
				m_CameraEntity = m_ActiveScene->CreateEntity("mainCamera");
				auto& transfromCMP =  m_CameraEntity.GetComponent<TransformComponent>();
				transfromCMP.Position = { 0.0f,0.0f,5.0f };
				auto& cmp = m_CameraEntity.AddComponent<CameraComponent>();
				cmp.SceneCamera.SetViewportSize(1920, 1080);
			}
			{
				m_SecondCameraEntity = m_ActiveScene->CreateEntity("SecondCamera");
				auto& cmp = m_SecondCameraEntity.AddComponent<CameraComponent>();		
				cmp.SceneCamera.SetViewportSize(1000, 1080);
				cmp.FixedAspectRatio = true;
				cmp.Primary = false;
			}

			// createMesh test
#if 1
			{
				auto hutaoEntity = m_ActiveScene->CreateEntity("HuTao");
				auto& meshCMP =  hutaoEntity.AddComponent<MeshComponent>();
				auto MeshSrouce = CreateRef<MeshSource>("F:/Kans3D/Hazel/KansEditor/assets/model/ht/ht.fbx");
				auto Meshshader = Shader::Create("F:/Kans3D/Hazel/KansEditor/assets/shaders/StaticMeshShader.glsl");
				Meshshader->SetShaderBuffer({
					{ShaderDataType::Mat4,"U_ViewProjection"},
					{ShaderDataType::Mat4,"U_Transform"}
					});
				Ref<Material> StaticMeshShader = Material::Create(Meshshader, "StaticMesh_MTL");
				MeshSrouce->SetMaterial(StaticMeshShader);
				meshCMP.MeshSource = MeshSrouce;
				auto& TransformCMP = hutaoEntity.GetComponent<TransformComponent>();
				TransformCMP.Rotation = { glm::radians(-20.0f),0.0f,0.0f };
			}
#endif			
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
		RenderCommand::SetClearColor({ 0.1f, 0.1f, 0.1f, 1.0f });
		RenderCommand::Clear();
		
	

		//rendering
		{
			HZ_PROFILE_SCOPE("rendering")

			//updateScene
			m_ActiveScene->OnUpdate(ts);

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
		ImGuiIO& io = ImGui::GetIO();
		if (io.ConfigFlags & ImGuiConfigFlags_DockingEnable)
		{
			ImGuiID dockspace_id = ImGui::GetID("MyDockSpace");
			ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), dockspace_flags);
		}


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
		if(0)
		{
			ImGui::Begin("ViewPort2");
			ImVec2 viewportsize = ImGui::GetContentRegionAvail();

			if (m_ViewportSize != *(glm::vec2*) & viewportsize)
			{
				//HZ_INFO("ViewportSize:  {0}   {1}", viewportsize.x, viewportsize.y);
				m_ViewportSize = { viewportsize.x,viewportsize.y };
				//m_Framebuffer->Resize(viewportsize.x, viewportsize.y);
			}


			uint64_t depthframebufferID =(uint64_t)m_Framebuffer->GetDepthAttachmentRendererID();
			ImGui::Image((void*)depthframebufferID, viewportsize, ImVec2(0, 1), ImVec2(1, 0));

			ImGui::End();
		}

	ImGui::PopStyleVar();
		


	}

}


