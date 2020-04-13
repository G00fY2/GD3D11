#include "BaseWidget.h"

#include "BaseLineRenderer.h"
#include "EditorLinePrimitive.h"
#include "Engine.h"
#include "GothicAPI.h"
using namespace DirectX;

BaseWidget::BaseWidget(WidgetContainer * container) {
	OwningContainer = container;

	Position = XMFLOAT3(0, 0, 0);
	Scale = XMFLOAT3(1, 1, 1);
	XMStoreFloat4x4(&Rotation, XMMatrixIdentity());
}

BaseWidget::~BaseWidget() {
}

/** Called when an object was added to the selection */
void BaseWidget::OnSelectionAdded(zCVob * vob) {
}

/** Captures the mouse in the middle of the screen and returns the delta since last frame */
float2 BaseWidget::GetMouseDelta() const {
	// Get current cursor pos
	POINT p;
	GetCursorPos(&p);
	//= D2DView::GetCursorPosition();
	
	RECT r;
	GetWindowRect(Engine::GAPI->GetOutputWindow(), &r);

	POINT mid;
	mid.x = (int)(r.left / 2 + r.right / 2);
	mid.y = (int)(r.top / 2 + r.bottom / 2);

	// Get difference to last frame
	float2 diff((float)(p.x - mid.x), (float)(p.y - mid.y));

	// Lock the mouse in center
	SetCursorPos(mid.x, mid.y);

	return diff;
}

/** Hides/Shows the mouse */
void BaseWidget::SetMouseVisibility(bool visible) {
	static HCURSOR s_oldCursor = GetCursor();

	if (!visible) {
		s_oldCursor = GetCursor();
		SetCursor(nullptr);
	} else {
		SetCursor(s_oldCursor);
	}
}

/** Renders the widget */
void BaseWidget::RenderWidget() {
}

/** Called when a mousebutton was clicked */
void BaseWidget::OnMButtonClick(int button) {
}

/** Called when the owning window got a message */
void BaseWidget::OnWindowMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
}

/** Widget primitives */
void BaseWidget::CreateArrowCone(int Detail, int Axis, const float4& Color, EditorLinePrimitive * Prim) {
	UINT NumVerts;
	NumVerts = Detail * 6; 

	LineVertex * vx = new LineVertex[NumVerts];

	float Step = (XM_PI * 2) / ((float)Detail - 1);
	float s = 0;

	float Length = BASEWIDGET_CONE_LENGTH;
	float Radius = BASEWIDGET_CONE_RADIUS;

	UINT i = 0;
	while (i < NumVerts) {
		// First vertex of the circle-line
		switch (Axis) {
		case 0:
			vx[i].Position = float3(Length, (sinf(s) * Radius), cosf(s) * Radius);	
			break;

		case 1:
			vx[i].Position = float3((sinf(s) * Radius), Length, cosf(s) * Radius);
			break;

		case 2:
			vx[i].Position = float3((sinf(s) * Radius), cosf(s) * Radius, Length);
			break;
		}
		EditorLinePrimitive::EncodeColor(&vx[i], Color);
		i++;

		s += Step;

		// Connector tri
		
		switch (Axis) {
		case 0:
			vx[i].Position = float3(BASEWIDGET_TRANS_LENGTH + 0.125, 0, 0);
			break;

		case 1:
			vx[i].Position = float3(0, BASEWIDGET_TRANS_LENGTH + 0.125, 0);
			break;

		case 2:
			vx[i].Position = float3(0, 0, BASEWIDGET_TRANS_LENGTH + 0.125);
			break;
		}
		EditorLinePrimitive::EncodeColor(&vx[i], Color);
		i++;

		// Second vertex of the circle-line
		switch (Axis) {
		case 0:
			vx[i].Position = float3(Length, (sinf(s) * Radius), cosf(s) * Radius);
			break;

		case 1:
			vx[i].Position = float3((sinf(s) * Radius), Length, cosf(s) * Radius);
			break;

		case 2:
			vx[i].Position = float3((sinf(s) * Radius), cosf(s) * Radius, Length);
			break;
		}
		EditorLinePrimitive::EncodeColor(&vx[i], Color);

		i++;

		switch (Axis) {
		case 0:
			// inner circle
			vx[i].Position = float3(Length, (sinf(s - Step) * Radius), cosf(s - Step) * Radius);
			EditorLinePrimitive::EncodeColor(&vx[i], Color);
			i++;

			// inner circle #2
			vx[i].Position = float3(Length, (sinf(s) * Radius), cosf(s) * Radius);
			EditorLinePrimitive::EncodeColor(&vx[i], Color);
			i++;

			// inner circle #3
			vx[i].Position = float3(BASEWIDGET_TRANS_LENGTH + 0.125, 0, 0);
			EditorLinePrimitive::EncodeColor(&vx[i], Color);
			i++;
			break;

		case 1:
			// inner circle
			vx[i].Position = float3((sinf(s - Step) * Radius), Length, cosf(s - Step) * Radius);
			EditorLinePrimitive::EncodeColor(&vx[i], Color);
			i++;

			// inner circle #2
			vx[i].Position = float3((sinf(s) * Radius), Length, cosf(s) * Radius);
			EditorLinePrimitive::EncodeColor(&vx[i], Color);
			i++;

			// inner circle #3
			vx[i].Position = float3(0, BASEWIDGET_TRANS_LENGTH + 0.125, 0);
			EditorLinePrimitive::EncodeColor(&vx[i], Color);
			i++;
			break;

		case 2:
			// inner circle
			vx[i].Position = float3((sinf(s - Step) * Radius), cosf(s - Step) * Radius, Length);
			EditorLinePrimitive::EncodeColor(&vx[i], Color);
			i++;

			// inner circle #2
			vx[i].Position = float3((sinf(s) * Radius), cosf(s) * Radius, Length);
			EditorLinePrimitive::EncodeColor(&vx[i], Color);
			i++;

			// inner circle #3
			vx[i].Position = float3(0, 0, BASEWIDGET_TRANS_LENGTH + 0.125);
			EditorLinePrimitive::EncodeColor(&vx[i], Color);
			i++;
			break;
		}
	}

	HRESULT hr;
	LE(Prim->CreateSolidPrimitive(vx, NumVerts, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST));

	delete[] vx;
}

void BaseWidget::CreateArrowCube(XMFLOAT3 * Offset, float Extends, const float4& Color, EditorLinePrimitive * Prim) {
	LineVertex vx[36];
	int i = 0;

	vx[i].Position = DirectX::SimpleMath::Vector3(-1, -1, -1);
	EditorLinePrimitive::EncodeColor(&vx[i++], Color);

	vx[i].Position = DirectX::SimpleMath::Vector3(1, -1, 1);
	EditorLinePrimitive::EncodeColor(&vx[i++], Color);

	vx[i].Position = DirectX::SimpleMath::Vector3(-1, -1, 1);
	EditorLinePrimitive::EncodeColor(&vx[i++], Color);

	vx[i].Position = DirectX::SimpleMath::Vector3(-1, -1, -1);
	EditorLinePrimitive::EncodeColor(&vx[i++], Color);

	vx[i].Position = DirectX::SimpleMath::Vector3(1, -1, -1);
	EditorLinePrimitive::EncodeColor(&vx[i++], Color);

	vx[i].Position = DirectX::SimpleMath::Vector3(1, -1, 1);
	EditorLinePrimitive::EncodeColor(&vx[i++], Color);

	vx[i].Position = DirectX::SimpleMath::Vector3(-1, 1, -1);
	EditorLinePrimitive::EncodeColor(&vx[i++], Color);

	vx[i].Position = DirectX::SimpleMath::Vector3(-1, 1, 1);
	EditorLinePrimitive::EncodeColor(&vx[i++], Color);

	vx[i].Position = DirectX::SimpleMath::Vector3(1, 1, 1);
	EditorLinePrimitive::EncodeColor(&vx[i++], Color);

	vx[i].Position = DirectX::SimpleMath::Vector3(-1, 1, -1);
	EditorLinePrimitive::EncodeColor(&vx[i++], Color);

	vx[i].Position = DirectX::SimpleMath::Vector3(1, 1, 1);
	EditorLinePrimitive::EncodeColor(&vx[i++], Color);

	vx[i].Position = DirectX::SimpleMath::Vector3(1, 1, -1);
	EditorLinePrimitive::EncodeColor(&vx[i++], Color);

	vx[i].Position = DirectX::SimpleMath::Vector3(-1, -1, -1);
	EditorLinePrimitive::EncodeColor(&vx[i++], Color);

	vx[i].Position = DirectX::SimpleMath::Vector3(-1, -1, 1);
	EditorLinePrimitive::EncodeColor(&vx[i++], Color);

	vx[i].Position = DirectX::SimpleMath::Vector3(-1, 1, 1);
	EditorLinePrimitive::EncodeColor(&vx[i++], Color);

	vx[i].Position = DirectX::SimpleMath::Vector3(-1, -1, -1);
	EditorLinePrimitive::EncodeColor(&vx[i++], Color);

	vx[i].Position = DirectX::SimpleMath::Vector3(-1, 1, 1);
	EditorLinePrimitive::EncodeColor(&vx[i++], Color);

	vx[i].Position = DirectX::SimpleMath::Vector3(-1, 1, -1);
	EditorLinePrimitive::EncodeColor(&vx[i++], Color);

	vx[i].Position = DirectX::SimpleMath::Vector3(1, -1, -1);
	EditorLinePrimitive::EncodeColor(&vx[i++], Color);

	vx[i].Position = DirectX::SimpleMath::Vector3(1, 1, 1);
	EditorLinePrimitive::EncodeColor(&vx[i++], Color);

	vx[i].Position = DirectX::SimpleMath::Vector3(1, -1, 1);
	EditorLinePrimitive::EncodeColor(&vx[i++], Color);

	vx[i].Position = DirectX::SimpleMath::Vector3(1, -1, -1);
	EditorLinePrimitive::EncodeColor(&vx[i++], Color);

	vx[i].Position = DirectX::SimpleMath::Vector3(1, 1, -1);
	EditorLinePrimitive::EncodeColor(&vx[i++], Color);

	vx[i].Position = DirectX::SimpleMath::Vector3(1, 1, 1);
	EditorLinePrimitive::EncodeColor(&vx[i++], Color);

	vx[i].Position = DirectX::SimpleMath::Vector3(-1, -1, -1);
	EditorLinePrimitive::EncodeColor(&vx[i++], Color);

	vx[i].Position = DirectX::SimpleMath::Vector3(1, 1, -1);
	EditorLinePrimitive::EncodeColor(&vx[i++], Color);

	vx[i].Position = DirectX::SimpleMath::Vector3(1, -1, -1);
	EditorLinePrimitive::EncodeColor(&vx[i++], Color);

	vx[i].Position = DirectX::SimpleMath::Vector3(-1, -1, -1);
	EditorLinePrimitive::EncodeColor(&vx[i++], Color);

	vx[i].Position = DirectX::SimpleMath::Vector3(-1, 1, -1);
	EditorLinePrimitive::EncodeColor(&vx[i++], Color);

	vx[i].Position = DirectX::SimpleMath::Vector3(1, 1, -1);
	EditorLinePrimitive::EncodeColor(&vx[i++], Color);

	vx[i].Position = DirectX::SimpleMath::Vector3(-1, -1, 1);
	EditorLinePrimitive::EncodeColor(&vx[i++], Color);

	vx[i].Position = DirectX::SimpleMath::Vector3(1, -1, 1);
	EditorLinePrimitive::EncodeColor(&vx[i++], Color);

	vx[i].Position = DirectX::SimpleMath::Vector3(1, 1, 1);
	EditorLinePrimitive::EncodeColor(&vx[i++], Color);

	vx[i].Position = DirectX::SimpleMath::Vector3(-1, -1, 1);
	EditorLinePrimitive::EncodeColor(&vx[i++], Color);

	vx[i].Position = DirectX::SimpleMath::Vector3(1, 1, 1);
	EditorLinePrimitive::EncodeColor(&vx[i++], Color);

	vx[i].Position = DirectX::SimpleMath::Vector3(-1, 1, 1);
	EditorLinePrimitive::EncodeColor(&vx[i++], Color);

	// Loop through all vertices and apply the offset and the extends
	XMVECTOR xmOffset = XMLoadFloat3(Offset);
	for (i = 0; i < 36; i++) {
		XMStoreFloat3(vx[i].Position.toXMFLOAT3(), XMLoadFloat3(vx[i].Position.toXMFLOAT3()) * Extends);
		XMStoreFloat3(vx[i].Position.toXMFLOAT3(), XMLoadFloat3(vx[i].Position.toXMFLOAT3()) + xmOffset);
	}

	Prim->CreateSolidPrimitive(&vx[0], 36);
}
