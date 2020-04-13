#include "pch.h"
#include "GSpriteCloud.h"


GSpriteCloud::GSpriteCloud()
{
}


GSpriteCloud::~GSpriteCloud()
{
}

struct CloudBB
{
	void MakeRandom(const DirectX::SimpleMath::Vector3& center, const DirectX::SimpleMath::Vector3& minSize, const DirectX::SimpleMath::Vector3& maxSize)
	{
		DirectX::SimpleMath::Vector3 d = maxSize - minSize;
		
		// Random Box size
		DirectX::SimpleMath::Vector3 sr = DirectX::SimpleMath::Vector3(minSize.x + Toolbox::frand() * d.x,
			minSize.y + Toolbox::frand() * d.y,
			minSize.z + Toolbox::frand() * d.z);
		sr /= 2.0f;
		Size = sr;

		Center = center;
	}

	DirectX::SimpleMath::Vector3 GetRandomPointInBox()
	{
		DirectX::SimpleMath::Vector3 r = DirectX::SimpleMath::Vector3((Toolbox::frand() * Size.x * 2) - Size.x,
			(Toolbox::frand() * Size.y * 2) - Size.y,
			(Toolbox::frand() * Size.z * 2) - Size.z);

		return r;
	}

	DirectX::SimpleMath::Vector3 Center;
	DirectX::SimpleMath::Vector3 Size;
};

/** Initializes this cloud */
void GSpriteCloud::CreateCloud(const DirectX::SimpleMath::Vector3& size, int numSprites)
{
	CloudBB c;
	c.MakeRandom(DirectX::SimpleMath::Vector3(0, 0, 0), size / 2.0f, size);

	// Fill the bb with sprites
	for(int i=0;i<numSprites;i++)
	{
		DirectX::SimpleMath::Vector3 rnd = c.GetRandomPointInBox();
		Sprites.push_back(rnd);

		DirectX::SimpleMath::Matrix m = DirectX::XMMatrixTranslation(rnd.x, rnd.y, rnd.z);

		SpriteWorldMatrices.push_back(m);
	}
}