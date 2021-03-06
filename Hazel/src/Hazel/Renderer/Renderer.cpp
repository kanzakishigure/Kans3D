#include "hzpch.h"
#include "Renderer.h"
#include "Renderer2D.h"
#include "Platform/OpenGL/OpenGLShader.h"
#include "Hazel/Renderer/SceneCamera.h"
namespace Hazel {
	
	Renderer::RendererData* Renderer::s_RendererData = new Renderer::RendererData;

	void Renderer::Init()
	{
		HZ_PROFILE_FUCTION();
		
		Renderer::RendererDataInit();

		RenderCommand::Init();
		Renderer2D::Init();
	}

	void Renderer::RendererDataInit()
	{
		//init renderer asset
		//Texture
		{
			s_RendererData->WhiteTexture = Texture2D::Create(1, 1);
			uint32_t data = 0xffffffff;
			s_RendererData->WhiteTexture->SetData((void*)&data, sizeof(uint32_t));

			s_RendererData->BlackTexture = Texture2D::Create(1, 1);
			data = 0xff000000;
			s_RendererData->BlackTexture->SetData((void*)&data, sizeof(uint32_t));
		}
		//shader
		{
			{
				s_RendererData->m_ShaderLibrary = CreateRef<ShaderLibrary>();
				auto StaticShader = Shader::Create("F:/Kans3D/Hazel/KansEditor/assets/shaders/StaticMeshShader.glsl");
				StaticShader->SetShaderBuffer({
						{ShaderDataType::Float3,MaterialAsset::GetDiffuseLocation()},
						{ShaderDataType::Float3,MaterialAsset::GetSpecularLocation()},
						{ShaderDataType::Float3,MaterialAsset::GetEmissionLocation()},
						{ShaderDataType::Float, MaterialAsset::GetShininessLocation()},
						{ShaderDataType::Float3,"dirLight.LightDir"},
						{ShaderDataType::Float3,"dirLight.Ambient_Intensity"},
						{ShaderDataType::Float3,"dirLight.Diffuse_Intensity"},
						{ShaderDataType::Float3,"dirLight.Specular_Intensity"},
						{ShaderDataType::Float3,"pointLight.Position"},
						{ShaderDataType::Float3,"pointLight.Ambient_Intensity"},
						{ShaderDataType::Float3,"pointLight.Diffuse_Intensity"},
						{ShaderDataType::Float3,"pointLight.Specular_Intensity"},
						{ShaderDataType::Float3,"U_ViewPos"}
					});
				s_RendererData->m_ShaderLibrary->Add(StaticShader);
			}

			{
				auto StencilShader = Shader::Create("F:/Kans3D/Hazel/KansEditor/assets/shaders/StencilShader.glsl");
				StencilShader->SetShaderBuffer({});
				s_RendererData->m_ShaderLibrary->Add(StencilShader);
			}

			{
				auto PostShader = Shader::Create("F:/Kans3D/Hazel/KansEditor/assets/shaders/PostShader.glsl");
				PostShader->SetShaderBuffer({});
				s_RendererData->m_ShaderLibrary->Add(PostShader);
			}
			
			{
				auto OutLineShader = Shader::Create("F:/Kans3D/Hazel/KansEditor/assets/shaders/OutLineShader.glsl");
				OutLineShader->SetShaderBuffer({});
				s_RendererData->m_ShaderLibrary->Add(OutLineShader);
			}

			{
				auto ToneShader = Shader::Create("F:/Kans3D/Hazel/KansEditor/assets/shaders/ToneShader.glsl");
				ToneShader->SetShaderBuffer({
						{ShaderDataType::Float3,MaterialAsset::GetDiffuseLocation()},
						{ShaderDataType::Float3,MaterialAsset::GetSpecularLocation()},
						{ShaderDataType::Float3,MaterialAsset::GetEmissionLocation()},
						{ShaderDataType::Float, MaterialAsset::GetShininessLocation()}
					});
				s_RendererData->m_ShaderLibrary->Add(ToneShader);
			}

			{
				auto DebugShader = Shader::Create("F:/Kans3D/Hazel/KansEditor/assets/shaders/DebugShader.glsl");
				DebugShader->SetShaderBuffer({
						{ShaderDataType::Float,MaterialAsset::GetShininessLocation()},
						{ShaderDataType::Float,"U_Debug"},
						{ShaderDataType::Color3,"U_DarkColor"},
						{ShaderDataType::Color4,"U_DarkColor4"},

					});
				s_RendererData->m_ShaderLibrary->Add(DebugShader);
			}
			

			{
				auto DebugnormalShader = Shader::Create("F:/Kans3D/Hazel/KansEditor/assets/shaders/DebugNormalShader.glsl");
				DebugnormalShader->SetShaderBuffer({
					});
				s_RendererData->m_ShaderLibrary->Add(DebugnormalShader);
			}
			
			{
				auto ToneCharactorShader = Shader::Create("F:/Kans3D/Hazel/KansEditor/assets/shaders/GenShin_Shader/ToneCharactorShader.glsl");
				ToneCharactorShader->SetShaderBuffer({
					{ShaderDataType::Color4,"U_ShadowMultColor"},
					{ShaderDataType::Color4,"U_DarkShadowMultColor"},
					{ShaderDataType::Float,"U_ShadowArea"},
					{ShaderDataType::Float,"U_FixDarkShadow"},
					{ShaderDataType::Float,"U_ShadowSmooth"},
					{ShaderDataType::Float,"U_DarkShadowSmooth"},
					{ShaderDataType::Float,"U_DarkShadowArea"},
					{ShaderDataType::Float,"U_RampShadowRange"},
					{ShaderDataType::Bool,"U_UseRamp"},
					{ShaderDataType::Float2,"U_RampSkin"},
					{ShaderDataType::Float2,"U_RampTights"},
					{ShaderDataType::Float2,"U_RampMetal"},
					{ShaderDataType::Float2,"U_RampSoft"},
					{ShaderDataType::Float2,"U_RampSilk"}
					});
				s_RendererData->m_ShaderLibrary->Add(ToneCharactorShader);
			}
		}

	}

	void Renderer::BeginScene(const Camera& camera, const glm::mat4& transform)
	{
		
		s_RendererData->ViewProjectionMatix = camera.GetProjectMatrix()*glm::inverse(transform);
		
	}
	void Renderer::EndScene()
	{

	}

	void Renderer::Submit(const Ref<Shader>& shader,const Ref<VertexArray>& vertexArray,const glm::mat4& transform)
	{
		shader->Bind();
		std::dynamic_pointer_cast<OpenGLShader>(shader)->UploadUniformMat4("U_ViewProjection", s_RendererData->ViewProjectionMatix);
		std::dynamic_pointer_cast<OpenGLShader>(shader)->UploadUniformMat4("U_Transform", transform);
		vertexArray->Bind();
		RenderCommand::DrawIndexed(vertexArray);
	}

	void Renderer::OnWindowResize(uint32_t width, uint32_t height)
	{
		RenderCommand::SetViewPort(0, 0, width, height);
	}

	Ref<Texture2D> Renderer::GetWhiteTexture()
	{
		return s_RendererData->WhiteTexture;
	}

	Ref<Texture2D> Renderer::GetBlackTexture()
	{
		return s_RendererData->BlackTexture;
	}


	Ref<ShaderLibrary> Renderer::GetShaderLibrary()
	{
		return s_RendererData->m_ShaderLibrary;
	}


}