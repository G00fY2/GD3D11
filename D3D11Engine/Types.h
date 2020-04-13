#pragma once

#include <string>

#include <Windows.h>

#include <d3d11.h>
#include <DirectXMath.h>
#include <SimpleMath.h>


/** Defines types used for the project */

/** Errorcodes */
enum XRESULT {
	XR_SUCCESS,
	XR_FAILED,
	XR_INVALID_ARG,
};

struct INT2 {
	INT2(int x, int y) {
		this->x = x;
		this->y = y;
	}

	INT2() { x = 0; y = 0; }

	std::string toString() const {
		return "(" + std::to_string(x) + ", " + std::to_string(y) + ")";
	}

	int x;
	int y;
};

struct INT4 {
	INT4(int x, int y, int z, int w) {
		this->x = x;
		this->y = y;
		this->z = z;
		this->w = w;
	}

	INT4() { x = 0; y = 0; z = 0; w = 0; }

	int x;
	int y;
	int z;
	int w;
};

struct float4;

struct float3 {
	float3(float x, float y, float z) {
		this->x = x;
		this->y = y;
		this->z = z;
	}

	float3(const DWORD & color) {
		BYTE r = (color >> 16) & 0xFF;
		BYTE g = (color >> 8) & 0xFF;
		BYTE b = color & 0xFF;

		x = r / 255.0f;
		y = g / 255.0f;
		z = b / 255.0f;
	}

	float3(const float4 & v) {
		x = ((float3 *)&v)->x;
		y = ((float3 *)&v)->y;
		z = ((float3 *)&v)->z;
	}

	float3(const DirectX::XMFLOAT3 & v) {
		x = v.x;
		y = v.y;
		z = v.z;
	}

	DirectX::XMFLOAT3 * toXMFLOAT3() const {
		return (DirectX::XMFLOAT3 *)this;
	}
	std::string toString() const {
		return std::string("(") + std::to_string(x) + ", " + std::to_string(y) + ", " + std::to_string(z) + ")";
	}

	/** Checks if this float3 is in the range a of the given float3 */
	bool isLike(const float3 & f, float a) const {
		float3 t;
		t.x = abs(x - f.x);
		t.y = abs(y - f.y);
		t.z = abs(z - f.z);

		return t.x < a && t.y < a && t.z < a;
	}

	static float3 FromColor(unsigned char r, unsigned char g, unsigned char b) {
		return float3(r / 255.0f, g / 255.0f, b / 255.0f);
	}

	bool operator<(const float3 & rhs) const  {
		if ((z < rhs.z)) {
			return true;
		}
		if ((z == rhs.z) && (y < rhs.y)) {
			return true;
		}
		if ((z == rhs.z) && (y == rhs.y) && (x < rhs.x)) {
			return true;
		}
		return false;
	}

	bool operator==(const float3 & b) const {
		return isLike(b, 0.0001f);
	}

	float3() { x = 0; y = 0; z = 0; }

	float x, y, z;
};

struct float4 { 
	float4(const DWORD & color) {
		BYTE a = color >> 24;
		BYTE r = (color >> 16) & 0xFF;
		BYTE g = (color >> 8) & 0xFF;
		BYTE b = color & 0xFF;

		x = r / 255.0f;
		y = g / 255.0f;
		z = b / 255.0f;
		w = a / 255.0f;
	}

	float4(float x, float y, float z, float w) {
		this->x = x;
		this->y = y;
		this->z = z;
		this->w = w;
	}

	float4(const float3 & f) {
		this->x = f.x;
		this->y = f.y;
		this->z = f.z;
		this->w = 1.0f;
	}

	float4(const float3 & f, float a) {
		this->x = f.x;
		this->y = f.y;
		this->z = f.z;
		this->w = a;
	}

	float4(const DirectX::XMFLOAT3 & v) {
		x = v.x;
		y = v.y;
		z = v.z;
		w = 1.0f;
	}

	float4(const DirectX::XMFLOAT4 & v) {
		x = v.x;
		y = v.y;
		z = v.z;
		w = v.w;
	}

	float4() { x = 0; y = 0; z = 0; w = 0; }

	DirectX::XMFLOAT4* toXMFLOAT4() const {
		return (DirectX::XMFLOAT4*)this;
	}

	DirectX::XMFLOAT3* toXMFLOAT3() const {
		return (DirectX::XMFLOAT3*)this;
	}
	float* toPtr() const {
		return (float*)this;
	}

	DWORD ToDWORD() const {
		BYTE a = (BYTE)(w * 255.0f);
		BYTE r = (BYTE)(x * 255.0f);
		BYTE g = (BYTE)(y * 255.0f);
		BYTE b = (BYTE)(z * 255.0f);

		char c[4];
		c[0] = a;
		c[1] = r;
		c[2] = g;
		c[3] = b;

		return *(DWORD *)c;
	}

	float x, y, z, w;
};

struct float2 {
	float2(float x, float y) {
		this->x = x;
		this->y = y;
	}

	float2(int x, int y) {
		this->x = (float)x;
		this->y = (float)y;
	}

	float2(const INT2 & i) {
		this->x = (float)i.x;
		this->y = (float)i.y;
	}

	float2() { x = 0; y = 0; }

	std::string toString() const {
		return std::string("(") + std::to_string(x) + ", " + std::to_string(y) + ")";
	}

	bool operator<(const float2 & rhs) const {
		if ((y < rhs.y)) {
			return true;
		}
		if ((y == rhs.y) && (x < rhs.x)) {
			return true;
		}
		return false;
	}

	float x, y;
};

