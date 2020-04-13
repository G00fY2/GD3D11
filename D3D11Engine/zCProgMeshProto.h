#pragma once
#include "pch.h"
#include "HookedFunctions.h"
#include "zCPolygon.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "zSTRING.h"
#include "zCArrayAdapt.h"
#include "zCVisual.h"

struct zTPMTriangle {
	VERTEX_INDEX wedge[3];
};		

struct zTPMWedge {
	DirectX::SimpleMath::Vector3 normal;
	DirectX::SimpleMath::Vector2 texUV;
	VERTEX_INDEX position;
};

struct zTPMTriangleEdges 
{
	VERTEX_INDEX edge[3];
};													

struct zTPMEdge 
{
	VERTEX_INDEX wedge[2];
};		

struct zTPMVertexUpdate 
{
	VERTEX_INDEX numNewTri;
	VERTEX_INDEX numNewWedge;
};

class zCSubMesh {
public:
	zCMaterial*						Material;
	zCArrayAdapt<zTPMTriangle>		TriList;
	zCArrayAdapt<zTPMWedge>			WedgeList;
	zCArrayAdapt<float>				ColorList;				
	zCArrayAdapt<VERTEX_INDEX>	TriPlaneIndexList;		
	zCArrayAdapt<zTPlane>			TriPlaneList;			
	zCArrayAdapt<zTPMTriangleEdges>	TriEdgeList;
	zCArrayAdapt<zTPMEdge>			EdgeList;
	zCArrayAdapt<float>				EdgeScoreList;

	zCArrayAdapt<VERTEX_INDEX>	WedgeMap;				
	zCArrayAdapt<zTPMVertexUpdate>	VertexUpdates;

	int vbStartIndex;

	static const unsigned int CLASS_SIZE = 0x58;
};

class zCProgMeshProto : public zCVisual
{
public:

	zCArrayAdapt<DirectX::SimpleMath::Vector3>* GetPositionList()
	{
		return (zCArrayAdapt<DirectX::SimpleMath::Vector3> *)THISPTR_OFFSET(GothicMemoryLocations::zCProgMeshProto::Offset_PositionList);
	}

	zCArrayAdapt<DirectX::SimpleMath::Vector3>* GetNormalsList()
	{
		return (zCArrayAdapt<DirectX::SimpleMath::Vector3> *)THISPTR_OFFSET(GothicMemoryLocations::zCProgMeshProto::Offset_NormalsList);
	}

	zCSubMesh* GetSubmesh(int n)
	{
		return (zCSubMesh *)(((char *)GetSubmeshes()) + zCSubMesh::CLASS_SIZE * n);
	}

	zCSubMesh* GetSubmeshes()
	{
		 return *(zCSubMesh **)(((char *)this) + (GothicMemoryLocations::zCProgMeshProto::Offset_Submeshes));
	}

	int GetNumSubmeshes()
	{
		return *(int *)(((char *)this) + (GothicMemoryLocations::zCProgMeshProto::Offset_NumSubmeshes));
	}

	/** Constructs a readable mesh from the data given in the progmesh */
	void ConstructVertexBuffer(std::vector<ExVertexStruct>* vertices)
	{
		zCArrayAdapt<DirectX::SimpleMath::Vector3>* pl = GetPositionList();

		for(int i=0;i<pl->NumInArray;i++)
		{
			ExVertexStruct vx;
			vx.Position = pl->Get(i);
			vx.Normal = DirectX::SimpleMath::Vector3(0, 0, 0);
			vx.TexCoord = DirectX::SimpleMath::Vector2(0, 0);
			vx.Color = 0xFFFFFFFF;
			vertices->push_back(vx);
		}
	}
};