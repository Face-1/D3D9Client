// ==============================================================
// HazeMgr.h
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2007 Martin Schweiger
// Copyright (C) 2014 Jarmo Nikkanen
// ==============================================================

// ==============================================================
// class HazeManager (interface)
//
// Planetary atmospheric haze rendering
// Implemented as transparent overlay on planetary disc
// ==============================================================

#ifndef __HAZEMGR_H
#define __HAZEMGR_H

#include "D3D9Client.h"
#include "D3D9Effect.h"
#include "PlanetRenderer.h"

#define HORIZON_NSEG 128	// number of mesh segments
#define HORIZON2_NSEG 512	// Horizon ring segments
#define HORIZON2_NRING 30	// Horizon ring ring count
#define HORIZON2_XSEG 10	// Sperical patch segments
#define HORIZON2_YSEG 40	// Sperical patch segments

class vPlanet;

class HazeManager : private D3D9Effect 
{
public:
	HazeManager (const oapi::D3D9Client *gclient, const vPlanet *vplanet);
	static void GlobalInit (oapi::D3D9Client *gclient);
	static void GlobalExit();

	void Render (LPDIRECT3DDEVICE9 dev, D3DXMATRIX &wmat, bool dual = false);
	
private:
	const oapi::D3D9Client *gc;
	OBJHANDLE obj;
	const vPlanet *vp;
	VECTOR3 basecol;
	double rad;    // planet radius
	float  hralt;  // relative horizon altitude
	float  dens0;  // atmosphere density factor
	double hshift; // horizon reference shift factor
	double cloudalt; // cloud layer altitude
	float  hscale; // inner haze ring radius (in planet radii)
	static WORD Idx[HORIZON_NSEG*2+2];
	static DWORD nIdx;
	static struct HVERTEX {
		float x,y,z;
		DWORD    dcol;
		float tu, tv; } Vtx[HORIZON_NSEG*2];
	static float CosP[HORIZON_NSEG], SinP[HORIZON_NSEG];
	static LPD3D9CLIENTSURFACE horizon;
};



// ==============================================================
// class HazeManager2 (interface)
//
// Planetary atmospheric haze rendering with scattering technique
// HazeManager2 is used with TileManager2
// ==============================================================

class HazeManager2 : public PlanetRenderer 
{
public:
	HazeManager2 (const oapi::D3D9Client *gclient, const vPlanet *vplanet);


	static	void GlobalInit (oapi::D3D9Client *gclient);
	static	void GlobalExit();
	static	void CreateSkydomeBuffers();
	static	void CreateRingBuffers();

	void	Render(D3DXMATRIX &wmat, float hz_aperture_deg);
	
private:

	void	RenderRing(VECTOR3 cpos, VECTOR3 cdir, double rad, double hralt);
	void	RenderSky(VECTOR3 cpos, VECTOR3 cdir, double rad, double apr);
	void	RenderSkySegment(D3DXMATRIX &wmat);

	OBJHANDLE obj;
	const vPlanet *vp;
	double hralt, rad;

	static LPDIRECT3DVERTEXBUFFER9 pSkyVB;
	static LPDIRECT3DVERTEXBUFFER9 pRingVB;
};




#endif // !__HAZEMGR_H