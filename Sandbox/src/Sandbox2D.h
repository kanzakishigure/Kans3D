#pragma once
#include <Hazel.h>


class Sandbox2DLayer :public Hazel::Layer
{

public:
	Sandbox2DLayer();
	virtual ~Sandbox2DLayer() = default;

	virtual void OnAttach() override;
	virtual void OnDetach() override;

	void OnUpdate(Hazel::TimeStep ts) override;
	void OnEvent(Hazel::Event& e) override;
	virtual void OnImGuiRender() override;
private:
	Hazel::ShaderLibrary shaderLibrary;
	Hazel::OrthographicCameraController m_CameraController;
	Hazel::Ref<Hazel::Texture2D> ChessTexture;
	Hazel::Ref<Hazel::Texture2D> LightPop;
	Hazel::Ref<Hazel::Texture2D>TextureSheet;
	Hazel::Ref<Hazel::Texture2D> HuTao;

	Hazel::Ref<Hazel::SubTexture2D> B_Rockstairs[2];
	Hazel::Ref<Hazel::SubTexture2D> W_Rockstairs[2];
	Hazel::Ref<Hazel::SubTexture2D> grassland[3];
	Hazel::Ref<Hazel::SubTexture2D> Y_roof[6];
	Hazel::Ref<Hazel::SubTexture2D> B_roof[6];
	//Hazel::Ref<Hazel::FrameBuffer> m_Framebuffer;
	
	//临时成员，用来进行2drenderer的进一步抽象
	glm::vec4 FlatColor = { 1.0,1.0,1.0,1.0 };
	Hazel::ParticleSystem m_ParticleSys;
	Hazel::ParticleProps m_Particle;

	Hazel::Ref<Hazel::FrameBuffer> m_Framebuffer;
};