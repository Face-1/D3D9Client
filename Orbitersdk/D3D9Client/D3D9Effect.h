// ===========================================================================================
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2011 - 2016 Jarmo Nikkanen
// ===========================================================================================

#ifndef __D3D9EFFECT_H
#define __D3D9EFFECT_H

#include "D3D9Client.h"
#include <d3d9.h> 
#include <d3dx9.h>

using namespace oapi;

class D3D9Effect {

	DWORD d3d9id;

public:
	static void D3D9TechInit(D3D9Client *gc, LPDIRECT3DDEVICE9 pDev, const char *folder);

	/**
	 * \brief Release global parameters
	 */
	static void GlobalExit();

	static void ShutDown();

	D3D9Effect();
	~D3D9Effect();

	static void UpdateEffectCamera(OBJHANDLE hPlanet);
	static void InitLegacyAtmosphere(OBJHANDLE hPlanet, float GlobalAmbient);
	static void SetViewProjMatrix(LPD3DXMATRIX pVP);

	static void RenderBoundingBox(const LPD3DXMATRIX pW, const LPD3DXMATRIX pGT, const D3DXVECTOR4 *bmin, const D3DXVECTOR4 *bmax, const LPD3DXVECTOR4 color);
	static void RenderBoundingSphere(const LPD3DXMATRIX pW, const LPD3DXMATRIX pGT, const D3DXVECTOR4 *bs, const LPD3DXVECTOR4 color);
	static void RenderBillboard(const LPD3DXMATRIX pW, LPD3D9CLIENTSURFACE pTex);
	static void RenderExhaust(const LPD3DXMATRIX pW, VECTOR3 &cdir, EXHAUSTSPEC *es, LPD3D9CLIENTSURFACE def);
	static void RenderSpot(float intens, const LPD3DXCOLOR color, const LPD3DXMATRIX pW, LPD3D9CLIENTSURFACE pTex);
	static void RenderArrow(OBJHANDLE hObj, const VECTOR3 *ofs, float size, const D3DXCOLOR *pColor);
	static void Render2DPanel(const MESHGROUP *mg, const LPD3D9CLIENTSURFACE pTex, const LPD3DXMATRIX pW, float alpha, float scale, bool additive);
	static void RenderReEntry(const LPD3D9CLIENTSURFACE pTex, const LPD3DXVECTOR3 vPosA, const LPD3DXVECTOR3 vPosB, const LPD3DXVECTOR3 vDir, float alpha_a, float alpha_b, float size);
	static void RenderArrow(OBJHANDLE hObj, const VECTOR3 *ofs, const VECTOR3 *dir, const VECTOR3 *rot, float size, const D3DXCOLOR *pColor);  
	static void RenderAxisVector(LPD3DXMATRIX pW, const LPD3DXCOLOR pColor, float len);
	
	static LPDIRECT3DDEVICE9 pDev;      ///< Static (global) render device
	static LPDIRECT3DVERTEXBUFFER9 VB;  ///< Static (global) Vertex buffer pointer
	static LPDIRECT3DTEXTURE9 pNoise;   ///< Static (global) noise texture
	static SURFHANDLE hNoise;           ///< Static (global) noise surface handle
	
	// Rendering Technique related parameters
	static ID3DXEffect	*FX;
	static D3D9Client   *gc; ///< The graphics client instance
	static D3D9Mesh     *hArrow;

	static D3D9MatExt	defmat;
	static D3D9MatExt	night_mat;
	static D3D9MatExt	emissive_mat;
	
	// Techniques ----------------------------------------------------
	static D3DXHANDLE	eVesselTech;     ///< Vessel exterior, surface bases
	static D3DXHANDLE	eSimple;
	static D3DXHANDLE	eBBTech;         ///< Bounding Box Tech
	static D3DXHANDLE	eBSTech;         ///< Bounding Sphere Tech
	static D3DXHANDLE   eExhaust;        ///< Render engine exhaust texture
	static D3DXHANDLE   eSpotTech;       ///< Vessel beacons
	static D3DXHANDLE   ePanelTech;      ///< Used to draw a new style 2D panel
	static D3DXHANDLE   ePanelTechB;     ///< Used to draw a new style 2D panel
	static D3DXHANDLE	eBaseTile;
	static D3DXHANDLE	eRingTech;       ///< Planet rings technique
	static D3DXHANDLE	eRingTech2;      ///< Planet rings technique
	static D3DXHANDLE	eShadowTech;     ///< Vessel ground shadows
	static D3DXHANDLE	eBaseShadowTech; ///< Used to draw transparent surface without texture
	static D3DXHANDLE	eBeaconArrayTech;
	static D3DXHANDLE	eArrowTech;      ///< (Grapple point) arrows
	static D3DXHANDLE	eAxisTech;
	static D3DXHANDLE	eVCHudTech;
	static D3DXHANDLE	eVCMFDTech;
	static D3DXHANDLE	eVCTech;
	static D3DXHANDLE	ePlanetTile;
	static D3DXHANDLE	eCloudTech;
	static D3DXHANDLE	eCloudShadow;
	static D3DXHANDLE	eSkyDomeTech;
	static D3DXHANDLE	eDiffuseTech;
	static D3DXHANDLE	eEmissiveTech;
	static D3DXHANDLE	eHazeTech;

	// Transformation Matrices ----------------------------------------
	static D3DXHANDLE	eVP;         ///< Combined View & Projection Matrix
	static D3DXHANDLE	eW;          ///< World Matrix
	static D3DXHANDLE	eWI;         ///< World inverse Matrix
	static D3DXHANDLE	eGT;         ///< MeshGroup transformation matrix
	static D3DXHANDLE	eGTI;        ///< Inverse mesh grp transformation matrix
	static D3DXHANDLE	eInstMatrix; ///< Instance Matrix array

	// Lighting related parameters ------------------------------------
	static D3DXHANDLE   eMtrl;
	static D3DXHANDLE	eMat;        ///< Material
	static D3DXHANDLE	eWater;      ///< Water
	static D3DXHANDLE	eSun;        ///< Sun
	static D3DXHANDLE	eLights;     ///< Additional light sources
	static D3DXHANDLE	eLightCount; ///< Number of additional light sources

	// Auxilliary params ----------------------------------------------
	static D3DXHANDLE   eModAlpha;     ///< BOOL multiply material alpha with texture alpha
	static D3DXHANDLE	eFullyLit;     ///< BOOL
	static D3DXHANDLE	eUseSpec;      ///< BOOL
	static D3DXHANDLE	eUseEmis;      ///< BOOL
	static D3DXHANDLE	eUseRefl;      ///< BOOL
	static D3DXHANDLE	eUseTransl;
	static D3DXHANDLE	eUseTransm;
	static D3DXHANDLE	eDebugHL;
	static D3DXHANDLE	eEnvMapEnable; ///< BOOL
	static D3DXHANDLE	eUseDisl;      ///< BOOL
	static D3DXHANDLE	eInSpace;      ///< BOOL
	static D3DXHANDLE	eNoColor;      ///< BOOL
	static D3DXHANDLE	eLocalLights;  ///< BOOL		
	static D3DXHANDLE	eGlow;	       ///< BOOL
	static D3DXHANDLE	eInvProxySize;
	static D3DXHANDLE	eMix;          ///< FLOAT Auxiliary factor/multiplier
	static D3DXHANDLE   eColor;        ///< Auxiliary color input
	static D3DXHANDLE   eFogColor;     ///< Fog color input
	static D3DXHANDLE   eTexOff;       ///< Surface tile texture offsets
	static D3DXHANDLE	eSpecularMode;
	static D3DXHANDLE	eHazeMode;
	static D3DXHANDLE	eNormalMap;
	static D3DXHANDLE	eTextured;
	static D3DXHANDLE	eClamp;
	static D3DXHANDLE   eTime;         ///< FLOAT Simulation elapsed time
	static D3DXHANDLE	eExposure;
	static D3DXHANDLE	eCameraPos;	
	static D3DXHANDLE   eDistScale;
	static D3DXHANDLE   eGlowConst;
	static D3DXHANDLE   eRadius;
	static D3DXHANDLE	eFogDensity;
	static D3DXHANDLE	ePointScale;
	static D3DXHANDLE	eAtmColor;
	static D3DXHANDLE	eProxySize;
	static D3DXHANDLE	eMtrlAlpha;
	static D3DXHANDLE	eAttennuate;
	static D3DXHANDLE	eInScatter;

	// Textures --------------------------------------------------------
	static D3DXHANDLE	eTex0;    ///< Primary texture
	static D3DXHANDLE	eTex1;    ///< Secondary texture
	static D3DXHANDLE	eTex3;    ///< Tertiary texture
	static D3DXHANDLE	eSpecMap;
	static D3DXHANDLE	eEmisMap;
	static D3DXHANDLE	eEnvMap;
	static D3DXHANDLE	eDislMap;
	static D3DXHANDLE	eReflMap;
	static D3DXHANDLE	eTranslMap;
	static D3DXHANDLE	eTransmMap;

	// Legacy Atmosphere -----------------------------------------------
	static D3DXHANDLE	eGlobalAmb;	 
	static D3DXHANDLE	eSunAppRad;	 
	static D3DXHANDLE	eAmbient0;	 
	static D3DXHANDLE	eDispersion;	  
};

#endif // !__D3D9EFFECT_H