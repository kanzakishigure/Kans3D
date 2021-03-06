#pragma once
#include <glm/glm.hpp>
#include "Hazel/Renderer/Mesh.h"
#include "Hazel/Renderer/SceneCamera.h"
#include "Hazel/Scene/Scene.h"
#include "Hazel/Renderer/FrameBuffer.h"
namespace Hazel
{
	struct SceneRendererCamera
	{
		SceneCamera camera;
		glm::mat4 viewMatrix;
		glm::vec3 Position;
	};
	struct SceneInfo
	{
		DirLight dirLight;
		PointLight pointLight;
		SceneRendererCamera sceneCamera;
	};
	struct SceneRendererSpecification
	{
		bool SwapChainTarget = false;
	};
	class Scene;
	class SceneRenderer 
	{
		public:
			SceneRenderer(Ref<Scene> scene, SceneRendererSpecification spec = SceneRendererSpecification());
			~SceneRenderer(){};

			void Init();
			void BeginScene(SceneInfo info);
			void EndScene();
			void SetScene(Ref<Scene> scene);
			void SubmitStaticMesh(Ref<StaticMesh> mesh,Ref<MaterialTable> material,glm::mat4 transform);
			void SubmitStaticMeshStencil(Ref<StaticMesh> mesh, glm::mat4 transform);
			void SubmitStaticMeshPostEffect(Ref<StaticMesh> mesh,Ref<Texture2D> attachment, glm::mat4 transform);
			void SubmitStaticMeshOutLine(Ref<StaticMesh> mesh, glm::mat4 transform);
			void SubmitStaticMeshToneshading(Ref<StaticMesh> mesh, glm::mat4 transform);



			//temp method
			void SetFrameBuffer(Ref<FrameBuffer> framebuffer) { m_FrameBuffer = framebuffer; }
			const Ref<FrameBuffer>& GetFrameBuffer()const { return m_FrameBuffer; }
		public:
			void SubmitStaticMeshDebugNormal(Ref<StaticMesh> mesh, glm::mat4 transform);
			void SubmitStaticMeshDebug(Ref<StaticMesh> mesh, glm::mat4 transform);
			void SubmitToneCharactorShader(Ref<StaticMesh> mesh, glm::mat4 transform);
		private:
			//SceneRenderer did't handle the life of Scnene;
			Ref<Scene> m_Scene;
			SceneInfo m_SceneInfo;
			SceneRendererSpecification m_Specification;
			Ref<FrameBuffer> m_FrameBuffer;
			bool m_Active = false;
	};
}