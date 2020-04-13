#pragma once

#include "Types.h"

typedef unsigned short VERTEX_INDEX;

/** We pack most of Gothics FVF-formats into this vertex-struct */
struct ExVertexStruct {
	DirectX::SimpleMath::Vector3 Position;
	DirectX::SimpleMath::Vector3 Normal;
	DirectX::SimpleMath::Vector2 TexCoord;
	DirectX::SimpleMath::Vector2 TexCoord2;
	DWORD Color;
};

struct SimpleObjectVertexStruct {
	DirectX::SimpleMath::Vector3 Position;
	DirectX::SimpleMath::Vector2 TexCoord;
};

struct ObjVertexStruct {
	DirectX::SimpleMath::Vector3 Position;
	DirectX::SimpleMath::Vector3 Normal;
	DirectX::SimpleMath::Vector2 TexCoord;
};

struct BasicVertexStruct {
	DirectX::SimpleMath::Vector3 Position;
};

struct ExSkelVertexStruct {
	DirectX::SimpleMath::Vector3 Position[4];
	DirectX::SimpleMath::Vector3 Normal;
	DirectX::SimpleMath::Vector2 TexCoord;
	DWORD Color;
	unsigned char boneIndices[4];
	float weights[4];
};

struct Gothic_XYZ_DIF_T1_Vertex {
	DirectX::SimpleMath::Vector3 xyz;
	DWORD color;
	DirectX::SimpleMath::Vector2 texCoord;
};

struct Gothic_XYZRHW_DIF_T1_Vertex {
	DirectX::SimpleMath::Vector3 xyz;
	float rhw;
	DWORD color;
	DirectX::SimpleMath::Vector2 texCoord;
};

struct Gothic_XYZRHW_DIF_SPEC_T1_Vertex {
	DirectX::SimpleMath::Vector3 xyz;
	float rhw;
	DWORD color;
	DWORD spec;
	DirectX::SimpleMath::Vector2 texCoord;
};

struct Gothic_XYZ_NRM_T1_Vertex {
	DirectX::SimpleMath::Vector3 xyz;
	DirectX::SimpleMath::Vector3 nrm;
	DirectX::SimpleMath::Vector2 texCoord;
};
