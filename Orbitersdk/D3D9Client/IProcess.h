// =================================================================================================================================
// The MIT Lisence:
//
// Copyright (C) 2016 Jarmo Nikkanen
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation 
// files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, 
// modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software 
// is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
// OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
// LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR
// IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
// =================================================================================================================================


#ifndef __IPROCESS_H
#define __IPROCESS_H

#include <d3d9.h> 
#include <d3dx9.h>
#include "OrbiterAPI.h"

// Address mode WRAP is assumed by default
// Filter POINT is assumed by default
#define IPF_CLAMP_U		0x0001
#define IPF_CLAMP_V		0x0002
#define IPF_CLAMP_W		0x0004
#define IPF_MIRROR_U	0x0008
#define IPF_MIRROR_V	0x0010
#define IPF_MIRROR_W	0x0020
#define IPF_LINEAR		0x0040
#define IPF_PYRAMIDAL	0x0080
#define IPF_GAUSSIAN	0x0100


class ImageProcessing {

public:

	// ----------------------------------------------------------------------------------
	// Create a IPI (Image processing interface) which allows to process and create data via GPU
	// _file is the filename where user shader code exists
	// _entry is the function entry point "myFunc". e.g. float4 myFunc(float x : TEXCOORD0, float y : TEXCOORD1) : COLOR 
	// which contains the executed code with two input variables x, y
	// ppf is a list of preprocessor directives e.g. "_MYSECTION;_DEBUG" used like #if defined(_MYSECTION) ..code.. #endif
	// ----------------------------------------------------------------------------------
			ImageProcessing(LPDIRECT3DDEVICE9 pDev, const char *_file, const char *_entry, const char *ppf=NULL);
			~ImageProcessing();

	// ----------------------------------------------------------------------------------
	// Use the 'Set' functions to assign a value into a shader constants ( e.g. uniform extern float4 myVector; )
	// If the variable "var" is defined but NOT used by the shader code, the variable "var" doesn't exists in
	// a constant table and an error is printed when trying to assign a value to it.
	// ----------------------------------------------------------------------------------
	void	SetFloat(const char *var, float val);
	void	SetInt(const char *var, int val);
	void	SetBool(const char *var, bool val);
	// ----------------------------------------------------------------------------------
	void	SetFloat(const char *var, const void *val, int bytes);
	void	SetInt(const char *var, const int *val, int bytes);
	void	SetBool(const char *var, const bool *val, int bytes);
	void	SetStruct(const char *var, const void *val, int bytes);

	// ----------------------------------------------------------------------------------
	// SetTexture can be used to assign a taxture and a sampler state flags to a sampler
	// In a shader code sampler is defined as (e.g. sampler mySamp; ) where "mySamp" is
	// the variable passed to SetTexture function. It's then used in a shader code like
	// tex2D(mySamp, float2(x,y))
	// ----------------------------------------------------------------------------------
	void	SetTexture(const char *var, SURFHANDLE hTex, DWORD flags);

	// ----------------------------------------------------------------------------------
	// SetOutput assigns a render target to the IP interface. "id" is an index of the render
	// target with a maximum value of 3. It is possible to render in four different targets
	// at the same time. Multisample AA is only supported with one render target. Unbound
	// a render target by setting it to NULL. After a NULL render target all later targets
	// are ignored.
	// ----------------------------------------------------------------------------------
	void	SetOutput(int id, SURFHANDLE hTex);

	// ----------------------------------------------------------------------------------
	bool	IsOK();
	bool	Execute();

	// Native DirectX calls -------------------------------------------------------------
	//
	void	SetOutputNative(int id, LPDIRECT3DSURFACE9 hSrf);
	void	SetTextureNative(const char *var, LPDIRECT3DTEXTURE9 hTex, DWORD flags);

private:

	bool	SetupViewPort();	

	struct {
		LPDIRECT3DTEXTURE9 hTex;
		DWORD flags;
	} pTextures[16];

	LPDIRECT3DDEVICE9 pDevice;
	LPDIRECT3DSURFACE9 pRtg[4], pRtgBak[4];
	LPD3DXCONSTANTTABLE pVSConst;
	LPD3DXCONSTANTTABLE pPSConst;
	LPDIRECT3DPIXELSHADER9 pPixel;
	LPDIRECT3DVERTEXSHADER9 pVertex;
	D3DSURFACE_DESC desc;
	D3DXMATRIX   mVP;
	D3DVIEWPORT9 iVP;
	D3DXHANDLE   hVP;

	char	file[256];
	char	entry[64];
};

#endif