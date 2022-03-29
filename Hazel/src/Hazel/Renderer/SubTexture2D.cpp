#include "hzpch.h"
#include "SubTexture2D.h"

namespace Hazel
{

	SubTexture2D::SubTexture2D(Ref<Texture2D> texture, glm::vec2 min, glm::vec2 max)
		:m_Texture(texture)
	{
		m_SubTexCroods[0] = { min.x,min.y };
		m_SubTexCroods[1] = { max.x,min.y };
		m_SubTexCroods[2] = { max.x,max.y };
		m_SubTexCroods[3] = { min.x,max.y };
	}



	Ref<SubTexture2D> SubTexture2D::CreateFromCroods(Ref<Texture2D> texture, glm::vec2 croods, glm::vec2 sprtesize)
	{
		glm::vec2 min = {(croods.x)*sprtesize.x/texture->GetWidth(),(croods.y) * sprtesize.y / texture->GetHeight() };
		glm::vec2 max = { (croods.x + 1) * sprtesize.x / texture->GetWidth(),(croods.y + 1) * sprtesize.y / texture->GetHeight() };

		return CreateRef<SubTexture2D>(texture, min, max);

	}

}