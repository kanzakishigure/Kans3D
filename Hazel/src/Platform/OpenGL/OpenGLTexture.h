#pragma once
#include "Hazel/Renderer/Texture.h"
#include <glad/glad.h>
namespace Hazel {


	class OpenGLTexture2D :public Texture2D 
	{
	public:
		OpenGLTexture2D(const std::string& path);
		OpenGLTexture2D(uint32_t width,uint32_t height);
		virtual ~OpenGLTexture2D() override;

		virtual void Bind(uint32_t slot = 0) const override;

		virtual void SetData(void* data, uint32_t size) override;
		
		virtual uint32_t GetRenererID() const override { return m_RendererID; }
		virtual uint32_t GetHeight() const override { return m_Height; };
		virtual uint32_t GetWidth() const override { return m_Width; };
	public:
		virtual bool operator==(const Texture& other) const override
		{
			//�ڽ�����ǿ������ת����ͨ�����÷��ʵ���˽�б���
			return m_RendererID == ((OpenGLTexture2D&)other).m_RendererID;
		}
	private:
		uint32_t m_Height;
		uint32_t m_Width;
		uint32_t m_RendererID;
		std::string path;
		GLenum m_InternalFormat, m_DataFormat;
	};
}