#include "pch.h"
#include "D3D11OcclusionQuerry.h"
#include "Engine.h"
#include "D3D11GraphicsEngine.h"
#include "GothicAPI.h"
#include "zCBspTree.h"
#include "Toolbox.h"
#include "zCCamera.h"

using namespace DirectX::SimpleMath;

// Delay to recheck visible objects for occlusion
const int VISIBLE_RECHECK_FRAME_DELAY = 1;

D3D11OcclusionQuerry::D3D11OcclusionQuerry()
{
	FrameID = 0;
}


D3D11OcclusionQuerry::~D3D11OcclusionQuerry()
{
	for(size_t i=0;i<Predicates.size();i++)
	{
		Predicates[i]->Release();
	}
}

/** Creates a new predication-object and returns its ID */
unsigned int D3D11OcclusionQuerry::AddPredicationObject()
{
	D3D11GraphicsEngine * g = (D3D11GraphicsEngine *)Engine::GraphicsEngine;

	HRESULT hr;
	ID3D11Predicate* p = nullptr;

	// Create new predication-object
	D3D11_QUERY_DESC qd;
	qd.Query = D3D11_QUERY_OCCLUSION_PREDICATE;
	qd.MiscFlags = 0;
	LE(g->GetDevice()->CreatePredicate(&qd, &p));

	// Add to the end of the list and return its ID
	Predicates.push_back(p);
	return Predicates.size() - 1;
}

/** Checks the BSP-Tree for visibility */
void D3D11OcclusionQuerry::DoOcclusionForBSP(BspInfo* root)
{
	if (!root || !root->OriginalNode)
		return;

	D3D11GraphicsEngine * g = (D3D11GraphicsEngine *)Engine::GraphicsEngine;

	// Check if this node has it's queryID
	if (root->OcclusionInfo.QueryID == -1)
	{
		// Add new object
		root->OcclusionInfo.QueryID = AddPredicationObject();

		// Create the occlusion-mesh for the node
		CreateOcclusionNodeMeshFor(root);
	}

	float4 c = float4(1, 1, 1, 1);

	// Check last frustum-culling state
	int clipFlags = 63;
	int fstate = zCCamera::GetCamera()->BBox3DInFrustum(root->OriginalNode->BBox3D, clipFlags);


	// If this node wasn't inside the frustum last frame, but got inside it this frame, just draw it
	// to reduce the popping in dialogs where the camera switches heavily between targets
	// This may introduce a little framedrop when the camera switches targets, but it has to be ok.
	if (root->OcclusionInfo.LastCameraClipType == ZTCAM_CLIPTYPE_OUT &&
		fstate != ZTCAM_CLIPTYPE_OUT)
	{
		// Mark entire subtree visible
		MarkTreeVisible(root->Front, true);
		MarkTreeVisible(root->Back, true);

		root->OcclusionInfo.VisibleLastFrame = true;
		root->OcclusionInfo.LastVisitedFrameID = FrameID;

		// Save this for the next frame
		root->OcclusionInfo.LastCameraClipType = fstate;
		return;
	}

	// Save this
	root->OcclusionInfo.LastCameraClipType = fstate;

	// If the node wasn't visible last frame, we need to test it again
	// Invisible nodes need to be tested each frame in case they go visible
	// Visible nodes don't need to be tested every frame
	if (!root->OcclusionInfo.VisibleLastFrame ||
	   (root->OcclusionInfo.LastVisitedFrameID + VISIBLE_RECHECK_FRAME_DELAY <= FrameID && root->OcclusionInfo.VisibleLastFrame))
	{
		

		// Take those which have the camera inside as visible
		// Also make leafs which don't contain anything just visible so we can save the draw-call
		if (Toolbox::PositionInsideBox(Engine::GAPI->GetCameraPosition(), root->OriginalNode->BBox3D.Min, root->OriginalNode->BBox3D.Max) ||
			(root->IsEmpty() && root->OriginalNode->IsLeaf())) 
		{
			DoOcclusionForBSP(root->Front);
			DoOcclusionForBSP(root->Back);

			root->OcclusionInfo.VisibleLastFrame = true;
			root->OcclusionInfo.LastVisitedFrameID = FrameID;
			return;
		}

		ID3D11Predicate* p = Predicates[root->OcclusionInfo.QueryID];

		// Check if there is data available from the last query
		if (root->OcclusionInfo.LastVisitedFrameID != 0 && // Always do the first query
			S_OK != g->GetContext()->GetData(p, nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH))
		{
			c = float4(0, 0, 1, 1);

			// Query is in progress and still not done, wait for it...
			if (!root->OcclusionInfo.VisibleLastFrame)
			{
				// Continue with the sub-nodes to see if they are visible to make sure nothing pops in
				// Don't continue with the sub-nodes if this was visible last time we checked
				DoOcclusionForBSP(root->Front);
				DoOcclusionForBSP(root->Back);
			}
		} else
		{
			// Query is done. Save the result!
			UINT32 data;
			g->GetContext()->GetData(p, &data, sizeof(UINT32), D3D11_ASYNC_GETDATA_DONOTFLUSH);
			root->OcclusionInfo.VisibleLastFrame = data > 0; // data contains visible pixels of the object

			if (data == 0)
			{
				// Mark entire subtree invisible and don't waste draw-calls for it
				MarkTreeVisible(root->Front, false);
				MarkTreeVisible(root->Back, false);
			} else
			{
				// Try to check the next nodes as well
				DoOcclusionForBSP(root->Front);
				DoOcclusionForBSP(root->Back);
			}

			if (data > 0)
				c = float4(0, 1, 0, 1);
			else
				c = float4(1, 0, 0, 1);

			// Issue the new query
			MeshInfo * mi = root->OcclusionInfo.NodeMesh;

			g->GetContext()->Begin(p);
			g->DrawVertexBufferIndexed(mi->MeshVertexBuffer, mi->MeshIndexBuffer, mi->Indices.size());
			g->GetContext()->End(p);
		}

		root->OcclusionInfo.LastVisitedFrameID = FrameID;
	}

	if (!Engine::GAPI->GetRendererState()->RendererSettings.DisableWatermark && root->OriginalNode->IsLeaf())
	{
		if (!root->OcclusionInfo.VisibleLastFrame)
		{
			//DebugVisualizeNodeMesh(root->OcclusionInfo.NodeMesh, c);
			Engine::GraphicsEngine->GetLineRenderer()->AddAABBMinMax(root->OriginalNode->BBox3D.Min,
																 root->OriginalNode->BBox3D.Max, c);
		}
	}
}

/** Begins the occlusion-checks */
void D3D11OcclusionQuerry::BeginOcclusionPass()
{
	D3D11GraphicsEngine * g = (D3D11GraphicsEngine *)Engine::GraphicsEngine;

	// Bind shaders and constant buffers
	g->SetupVS_ExMeshDrawCall();
	g->SetupVS_ExConstantBuffer();

	// Unbind not needed shaders
	g->GetContext()->PSSetShader(nullptr, nullptr, 0);
	g->GetContext()->HSSetShader(nullptr, nullptr, 0);
	g->GetContext()->DSSetSamplers(0, 0, nullptr);
}

/** Ends the occlusion-checks */
void D3D11OcclusionQuerry::EndOcclusionPass()
{

}

/** Advances the frame counter of this */
void D3D11OcclusionQuerry::AdvanceFrameCounter()
{
	FrameID++;
}

/** Creates the occlusion-node-mesh for the specific bsp-node */
void D3D11OcclusionQuerry::CreateOcclusionNodeMeshFor(BspInfo* node)
{
	MeshInfo * mi = new MeshInfo;
	auto bbmin = node->OriginalNode->BBox3D.Min;
	auto bbmax = node->OriginalNode->BBox3D.Max;
	Vector3 n3 = Vector3(0, 0, 0);
	Vector2 n2 = Vector2(0, 0);

	ExVertexStruct vx[8] = {
	{bbmin, n3, n2, n2, 0},								// front bot left 0
	{Vector3(bbmin.x, bbmin.y, bbmax.z), n3, n2, n2, 0}, // back bot left 1
	{Vector3(bbmax.x, bbmin.y, bbmax.z), n3, n2, n2, 0}, // back bot right 2
	{Vector3(bbmax.x, bbmin.y, bbmin.z), n3, n2, n2, 0}, // front bot right 3
	{Vector3(bbmin.x, bbmax.y, bbmin.z), n3, n2, n2, 0},	// front top left 4
	{Vector3(bbmin.x, bbmax.y, bbmax.z), n3, n2, n2, 0}, // back top left 5
	{Vector3(bbmax.x, bbmax.y, bbmax.z), n3, n2, n2, 0},	// back top right 6
	{Vector3(bbmax.x, bbmax.y, bbmin.z), n3, n2, n2, 0}};// front top right 7

	VERTEX_INDEX idx[] = {
		// bottom
		0, 1, 2,
		0, 2, 3,

		// top
		4, 5, 6,
		4, 6, 7,

		// left
		1, 5, 4,
		1, 4, 0,

		// back
		1, 6, 5,
		1, 2, 6,

		// right
		3, 7, 6,
		3, 6, 2,

		// front
		0, 4, 7,
		0, 7, 3
	};

	// Create the buffers
	mi->Create(vx, sizeof(vx)/sizeof(vx[0]), idx, sizeof(idx)/sizeof(idx[0]));

	node->OcclusionInfo.NodeMesh = mi;
}

void D3D11OcclusionQuerry::DebugVisualizeNodeMesh(MeshInfo * m, const float4 & color)
{
	for(unsigned int i=0;i<m->Indices.size();i+=3)
	{
		Vector3 tri[3];
		float edge[3];

		tri[0] = m->Vertices[m->Indices[i]].Position;
		tri[1] = m->Vertices[m->Indices[i+1]].Position;
		tri[2] = m->Vertices[m->Indices[i+2]].Position;

		edge[0] = m->Vertices[m->Indices[i]].TexCoord2.x;
		edge[1] = m->Vertices[m->Indices[i+1]].TexCoord2.x;
		edge[2] = m->Vertices[m->Indices[i+2]].TexCoord2.x;

		Engine::GraphicsEngine->GetLineRenderer()->AddLine(LineVertex(tri[0], color), LineVertex(tri[1], color));
		Engine::GraphicsEngine->GetLineRenderer()->AddLine(LineVertex(tri[0], color), LineVertex(tri[1], color));
		Engine::GraphicsEngine->GetLineRenderer()->AddLine(LineVertex(tri[1], color), LineVertex(tri[2], color));

	}
}

/** Marks the entire subtree visible */
void D3D11OcclusionQuerry::MarkTreeVisible(BspInfo* root, bool visible)
{
	if (!root || !root->OriginalNode)
		return;

	root->OcclusionInfo.LastVisitedFrameID = FrameID;
	root->OcclusionInfo.VisibleLastFrame = visible;

	MarkTreeVisible(root->Front, visible);
	MarkTreeVisible(root->Back, visible);
}