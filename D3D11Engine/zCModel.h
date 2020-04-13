#pragma once
#include "pch.h"
#include "HookedFunctions.h"
#include "zCPolygon.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "zSTRING.h"
#include "zCArray.h"
#include "zTypes.h"
#include "zCWorld.h"
#include "zCCamera.h"
#include "zCModelTexAniState.h"
#include "zCVisual.h"
#include "zCMeshSoftSkin.h"
#include "zCObject.h"
#include "AlignedAllocator.h"

class zCVisual;
class zCMeshSoftSkin;

struct zCModelNode;
struct zCModelNodeInst
{
	zCModelNodeInst* ParentNode;
	zCModelNode* ProtoNode;
	zCVisual* NodeVisual;
	DirectX::XMFLOAT4X4 Trafo;
	DirectX::XMFLOAT4X4 TrafoObjToCam;	
	zTBBox3D BBox3D;

	zCModelTexAniState TexAniState;
};



struct zCModelNode
{
	zCModelNode* ParentNode;
	zSTRING	NodeName;
	zCVisual* Visual;
	DirectX::SimpleMath::Matrix Transform;

	DirectX::SimpleMath::Vector3 NodeRotAxis;
	float NodeRotAngle;
	DirectX::SimpleMath::Vector3	Translation;
	DirectX::SimpleMath::Matrix TransformObjToWorld;
	DirectX::SimpleMath::Matrix* NodeTransformList;
	zCModelNodeInst* LastInstNode;
};

struct zTMdl_NodeVobAttachment
{
	zCVob * Vob;
	zCModelNodeInst* NodeInst;
};

class zCModelMeshLib;
struct zTMeshLibEntry
{				
	zCModelTexAniState TexAniState;
	zCModelMeshLib* MeshLibPtr;
};

class zCModelAni
{
public:
	bool IsIdleAni()
	{
#ifdef BUILD_GOTHIC_1_08k
		return false;
#else
		DWORD value = *(DWORD *)THISPTR_OFFSET(GothicMemoryLocations::zCModelAni::Offset_Flags);
		return (value & GothicMemoryLocations::zCModelAni::Mask_FlagIdle) != 0;
#endif
	}

	int GetNumAniFrames()
	{
#ifdef BUILD_GOTHIC_1_08k
		return 0;
#else
		return *(uint16_t *)THISPTR_OFFSET(GothicMemoryLocations::zCModelAni::Offset_NumFrames);
#endif
	}
};

class zCModelAniActive
{
public:
	zCModelAni* protoAni;
	zCModelAni* nextAni;
};

class zCModelMeshLib : public zCObject
{
public:
	struct zTNodeMesh
	{			
		zCVisual* Visual;
		int NodeIndex;	
	};

	/** This returns the list of nodes which hold information about the bones and attachments later */
	zCArray<zTNodeMesh>* GetNodeList()
	{
		return &NodeList;
	}

	/** Returns the list of meshes which store the vertex-positions and weights */
	zCArray<zCMeshSoftSkin *>* GetMeshSoftSkinList()
	{
#ifndef BUILD_GOTHIC_1_08k
		return &SoftSkinList;
#else
		return nullptr;
#endif
	}

	const char* GetVisualName()
	{
		if (GetMeshSoftSkinList()->NumInArray > 0)
			return GetMeshSoftSkinList()->Array[0]->GetObjectName();

		return "";
		//return __GetVisualName().ToChar();
	}

private:
	zCArray<zTNodeMesh>			NodeList;
	zCArray<zCMeshSoftSkin*>	SoftSkinList;
};

class zCModelPrototype
{
public:
	/** Hooks the functions of this Class */
	static void Hook()
	{
		//XHook(HookedFunctions::OriginalFunctions.original_zCModelPrototypeLoadModelASC, GothicMemoryLocations::zCModelPrototype::LoadModelASC, zCModelPrototype::Hooked_LoadModelASC);
		//XHook(HookedFunctions::OriginalFunctions.original_zCModelPrototypeReadMeshAndTreeMSB, GothicMemoryLocations::zCModelPrototype::ReadMeshAndTreeMSB, zCModelPrototype::Hooked_ReadMeshAndTreeMSB);

	}

	/** This is called on load time for models */
	static int __fastcall Hooked_LoadModelASC(void * thisptr, void * unknwn, const zSTRING& file)
	{
		LogInfo() << "Loading Model: " << file.ToChar();
		int r = HookedFunctions::OriginalFunctions.original_zCModelPrototypeLoadModelASC(thisptr, file);

		// Pre-Load this model for us, too
		if (r)
		{
			
		}

		return r;
	}

	/** This is called on load time for models */
	static int __fastcall Hooked_ReadMeshAndTreeMSB(void * thisptr, void * unknwn, int& i, class zCFileBIN& f)
	{
		LogInfo() << "Loading Model!";
		int r = HookedFunctions::OriginalFunctions.original_zCModelPrototypeReadMeshAndTreeMSB(thisptr, i, f);

		// Pre-Load this model for us, too
		if (r)
		{
		}

		return r;
	}

	


	/** This returns the list of nodes which hold information about the bones and attachments later */
	zCArray<zCModelNode *>* GetNodeList()
	{
		return (zCArray<zCModelNode *> *)THISPTR_OFFSET(GothicMemoryLocations::zCModelPrototype::Offset_NodeList);
	}

	/** Returns the list of meshes which store the vertex-positions and weights */
	zCArray<zCMeshSoftSkin *>* GetMeshSoftSkinList()
	{
#ifndef BUILD_GOTHIC_1_08k
		return (zCArray<zCMeshSoftSkin *> *)THISPTR_OFFSET(GothicMemoryLocations::zCModelPrototype::Offset_MeshSoftSkinList);
#else
		return nullptr;
#endif
	}

	/** Returns the name of the first Mesh inside this */
	const char* GetVisualName()
	{
		if (GetMeshSoftSkinList()->NumInArray > 0)
			return GetMeshSoftSkinList()->Array[0]->GetObjectName();

		return "";
	}
};



class zCModel : public zCVisual
{
public:
	/** Hooks the functions of this Class */
	static void Hook()
	{
/*#ifndef BUILD_GOTHIC_1_08k
		DWORD dwProtect;
		VirtualProtect((void *)GothicMemoryLocations::zCModel::AdvanceAnis, GothicMemoryLocations::zCModel::SIZE_AdvanceAnis, PAGE_EXECUTE_READWRITE, &dwProtect);

		byte unsmoothAnisFix[] = {0x75, 0x00, 0xC7, 0x44, 0x24, 0x78, 0x01, 0x00, 0x00, 0x00}; // Replaces a jnz in AdvanceAnis - Thanks to killer-m!
		memcpy((void *)GothicMemoryLocations::zCModel::RPL_AniQuality, unsmoothAnisFix, sizeof(unsmoothAnisFix));
#endif*/
	}


	/** Creates an array of matrices for the bone transforms */
	void __fastcall RenderNodeList(zTRenderContext& renderContext, zCArray<DirectX::SimpleMath::Matrix*> & boneTransforms, zCRenderLightContainer& lightContainer, int lightingMode = 0)
	{
		XCALL(GothicMemoryLocations::zCModel::RenderNodeList);
	}

	/** Returns the current amount of active animations */
	int GetNumActiveAnimations()
	{
		return *(int *)THISPTR_OFFSET(GothicMemoryLocations::zCModel::Offset_NumActiveAnis);
	}

	/** Returns true if only an idle-animation is running */
	bool IdleAnimationRunning()
	{
#ifdef BUILD_GOTHIC_1_08k
		return false;
#else
		zCModelAniActive* activeAni = *(zCModelAniActive **)THISPTR_OFFSET(GothicMemoryLocations::zCModel::Offset_AniChannels);

		return GetNumActiveAnimations() == 1 && (/*activeAni->protoAni->IsIdleAni() ||*/ activeAni->protoAni->GetNumAniFrames() <= 1);
#endif
	}

	/** This is needed for the animations to work at full framerate */
	void SetDistanceToCamera(float dist)
	{
		*(float *)THISPTR_OFFSET(GothicMemoryLocations::zCModel::Offset_DistanceModelToCamera) = dist;
	}

	/** Updates stuff like blinking eyes, etc */
	void zCModel::UpdateMeshLibTexAniState() 
	{
		for (int i=0; i<GetMeshLibList()->NumInArray; i++) 
			GetMeshLibList()->Array[i]->TexAniState.UpdateTexList();
	}

	int GetIsVisible()
	{
#ifndef BUILD_GOTHIC_1_08k
		return (*(int *)THISPTR_OFFSET(GothicMemoryLocations::zCModel::Offset_Flags)) & 1;
#else
		return 1;
#endif
	}

	void SetIsVisible(bool visible)
	{
#ifndef BUILD_GOTHIC_1_08k
		int v = visible ? 1 : 0;

		byte* flags = (byte *)THISPTR_OFFSET(GothicMemoryLocations::zCModel::Offset_Flags);

		*flags &= ~1;
		*flags |= v;
#else
		// Do nothing yet
		// FIXME
#endif
	}

	DirectX::SimpleMath::Vector3 GetModelScale()
	{
#ifdef BUILD_GOTHIC_1_08k
		return D3DXVECTOR3(1, 1, 1);
#endif

		return *(DirectX::SimpleMath::Vector3*)THISPTR_OFFSET(GothicMemoryLocations::zCModel::Offset_ModelScale);
	}

	DirectX::XMVECTOR GetModelScaleXM()
	{
#ifdef BUILD_GOTHIC_1_08k
		return DirectX::XMVectorSet(1, 1, 1, 1);
#endif

		return DirectX::XMLoadFloat3((DirectX::XMFLOAT3*)THISPTR_OFFSET(GothicMemoryLocations::zCModel::Offset_ModelScale));
	}

	float GetModelFatness()
	{
#ifdef BUILD_GOTHIC_1_08k
		return 0.0f;
#endif
		float fatness = *(float*)THISPTR_OFFSET(GothicMemoryLocations::zCModel::Offset_ModelFatness);
		// fix fatness value
		if (fatness >= 2.0f)
			fatness = 0.50f;
		else if (fatness == 1.0f)
			fatness = 0.0f;
		else if (fatness <= -1.0f)
			fatness = -0.25f;

		return fatness;
	}

	int GetDrawHandVisualsOnly()
	{
#ifndef BUILD_GOTHIC_1_08k
		return *(int *)THISPTR_OFFSET(GothicMemoryLocations::zCModel::Offset_DrawHandVisualsOnly);
#else
		return 0; // First person not implemented in G1
#endif
	}

	zCArray<zCModelNodeInst *>* GetNodeList()
	{
		return (zCArray<zCModelNodeInst *> *)THISPTR_OFFSET(GothicMemoryLocations::zCModel::Offset_NodeList);
	}

	zCArray<zCMeshSoftSkin *>* GetMeshSoftSkinList()
	{
		return (zCArray<zCMeshSoftSkin *> *)THISPTR_OFFSET(GothicMemoryLocations::zCModel::Offset_MeshSoftSkinList);
	}

	zCArray<zCModelPrototype *>* GetModelProtoList()
	{
		return (zCArray<zCModelPrototype *> *)THISPTR_OFFSET(GothicMemoryLocations::zCModel::Offset_ModelProtoList);
	}

	zCArray<zTMeshLibEntry *>* GetMeshLibList()
	{
		return (zCArray<zTMeshLibEntry *> *)THISPTR_OFFSET(GothicMemoryLocations::zCModel::Offset_MeshLibList);
	}

	zCArray<zTMdl_NodeVobAttachment>* GetAttachedVobList()
	{
		return (zCArray<zTMdl_NodeVobAttachment> *)THISPTR_OFFSET(GothicMemoryLocations::zCModel::Offset_AttachedVobList); 
	}

	/* Updates the world matrices of the attached VOBs */
	void UpdateAttachedVobs()
	{
		XCALL(GothicMemoryLocations::zCModel::UpdateAttachedVobs); 
	}

	/** Fills a vector of (viewspace) bone-transformation matrices for this frame */
	void GetBoneTransforms(std::vector<DirectX::XMFLOAT4X4>* transforms, zCVob* vob = nullptr)
	{
		if (!GetNodeList())
			return;

		// Make this static so we don't reallocate the memory every time
		static std::vector<DirectX::XMFLOAT4X4*, AlignmentAllocator<DirectX::XMFLOAT4X4*, 16>> tptr;
		tptr.resize(GetNodeList()->NumInArray, nullptr);
		for (int i = 0; i < GetNodeList()->NumInArray; i++)
		{
			zCModelNodeInst* node = GetNodeList()->Array[i];
			zCModelNodeInst* parent = node->ParentNode;
			tptr[i] = &node->TrafoObjToCam;

			// Calculate transform for this node
			if (parent)
			{
				XMStoreFloat4x4(&node->TrafoObjToCam, XMLoadFloat4x4(&parent->TrafoObjToCam) * XMLoadFloat4x4(&node->Trafo));
			}
			else
			{
				node->TrafoObjToCam = node->Trafo;
			}

		}
		// Put them into our vector
		for (unsigned int i = 0; i < tptr.size(); i++)
		{
			DirectX::XMFLOAT4X4 m = *tptr[i];
			transforms->push_back(m);
		}
	}

	const char* GetVisualName()
	{
		if (GetMeshSoftSkinList()->NumInArray > 0)
			return GetMeshSoftSkinList()->Array[0]->GetObjectName();

		return "";
		//return __GetVisualName().ToChar();
	}

	zSTRING GetModelName()
	{
		XCALL(GothicMemoryLocations::zCModel::GetVisualName);
	}

private:
	
};