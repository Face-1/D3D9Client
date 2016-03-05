// ==============================================================
// VPlanet.cpp
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2006-2016 Martin Schweiger
//				 2010-2016 Jarmo Nikkanen
// ==============================================================

// ==============================================================
// class vPlanet (implementation)
//
// A vPlanet is the visual representation of a "planetary" object
// (planet, moon, asteroid).
// Currently this only supports spherical objects, without
// variations in elevation.
// ==============================================================

#define D3D_OVERLOADS

#include "D3D9Client.h"
#include "D3D9Config.h"
#include "VPlanet.h"
#include "VBase.h"
#include "SurfMgr.h"
#include "surfmgr2.h"
#include "cloudmgr2.h"
#include "CloudMgr.h"
#include "HazeMgr.h"
#include "RingMgr.h"
#include "FileParser.h"
#include "DebugControls.h"
#include "AtmoControls.h"
#include "VectorHelpers.h"
#include "OapiExtension.h"

using namespace oapi;

// ==============================================================

static double farplane = 1e6;
static double max_surf_dist = 1e4;

extern int SURF_MAX_PATCHLEVEL;

// ==============================================================

vPlanet::vPlanet (OBJHANDLE _hObj, const Scene *scene): vObject (_hObj, scene)
{
	char path[MAX_PATH];
	FILE *fp = NULL;

	memset(&MicroCfg, 0, sizeof(MicroCfg));
	vRefPoint = _V(1,0,0);
	bScatter = false;
	rad = (float)size;
	render_rad = (float)(0.1*rad);
	dist_scale = 1.0f;
	max_centre_dist = 0.9*scene->GetCameraFarPlane();
	maxdist = max (max_centre_dist, max_surf_dist + rad);
	max_patchres = *(DWORD*)oapiGetObjectParam (_hObj, OBJPRM_PLANET_SURFACEMAXLEVEL);
	max_patchres = min (max_patchres, *(DWORD*)gc->GetConfigParam (CFGPRM_SURFACEMAXLEVEL));
	tilever = *(int*)oapiGetObjectParam (_hObj, OBJPRM_PLANET_TILEENGINE);
	if (tilever < 2) {
		surfmgr = new SurfaceManager (gc, this);
		surfmgr2 = NULL;

		// Check existance of tileformat (1) texture data
		sprintf_s(path, MAX_PATH, "Textures\\%s.tex", name);
		if (fopen_s(&fp, path, "rb")) {
			LogErr("WARNING: No texture data found for %s (TileFormat = 1)", name);
		} else fclose(fp);

	} else {
		bScatter = LoadAtmoConfig(false);
		if (bScatter) LoadAtmoConfig(true);
		surfmgr = NULL;
		int patchlvl = 1 << (5 + Config->MeshRes);
		surfmgr2 = new TileManager2<SurfTile> (this, max_patchres, patchlvl);
		prm.horizon_excess = *(double*)oapiGetObjectParam (_hObj, OBJPRM_PLANET_HORIZONEXCESS);
		prm.tilebb_excess = *(double*)oapiGetObjectParam (_hObj, OBJPRM_PLANET_TILEBBEXCESS);

		// Check existance of tileformat (2) texture data
		sprintf_s(path, MAX_PATH, "Textures\\%s\\Surf\\%02d\\%06d\\%06d.dds", name, 1, 0, 0);
		if (fopen_s(&fp, path, "rb")) {
			LogErr("WARNING: No texture data found for %s (TileFormat = 2)", name);
		} else fclose(fp);
	}
	prm.bAtm = oapiPlanetHasAtmosphere (_hObj);
	if (prm.bAtm) {
		const ATMCONST *atmc = oapiGetPlanetAtmConstants(_hObj);
		prm.atm_hzalt = atmc->horizonalt;
		prm.atm_href = log(atmc->rho0)*2e4 + 2e4;
		prm.atm_amb0 = min (0.7, log (atmc->rho0+1.0)*0.35);
		DWORD amb0 = *(DWORD*)gc->GetConfigParam (CFGPRM_AMBIENTLEVEL);
		prm.amb0col = 0;
		for (int i = 0; i < 4; i++) prm.amb0col |= amb0 << (i<<3);
	}
	hazemgr = NULL;
	hazemgr2 = NULL;
	hashaze = *(bool*)gc->GetConfigParam (CFGPRM_ATMHAZE) && prm.bAtm;
	bRipple = *(bool*)gc->GetConfigParam (CFGPRM_SURFACERIPPLE) &&
		*(bool*)oapiGetObjectParam (_hObj, OBJPRM_PLANET_SURFACERIPPLE);
	if (bRipple) {
		if (surfmgr) surfmgr->SetMicrotexture ("waves.dds");
	}

	shadowalpha = (float)(/*1.0f -*/ *(double*)oapiGetObjectParam (_hObj, OBJPRM_PLANET_SHADOWCOLOUR));
	bVesselShadow = *(bool*)gc->GetConfigParam (CFGPRM_VESSELSHADOWS) && (shadowalpha >= 0.01);

	clouddata = 0;
	cloudmgr2 = 0;
	prm.bCloud = (*(bool*)gc->GetConfigParam (CFGPRM_CLOUDS) &&
		*(bool*)oapiGetObjectParam (_hObj, OBJPRM_PLANET_HASCLOUDS));
	if (prm.bCloud) {
		int cloudtilever = *(int*)oapiGetObjectParam (_hObj, OBJPRM_PLANET_CLOUDTILEENGINE);
		prm.cloudalt = *(double*)oapiGetObjectParam (_hObj, OBJPRM_PLANET_CLOUDALT);
		prm.bCloudBrighten = *(bool*)oapiGetObjectParam (_hObj, OBJPRM_PLANET_CLOUDOVERSATURATE);
		prm.bCloudShadow = *(bool*)gc->GetConfigParam (CFGPRM_CLOUDSHADOWS);
		prm.shadowalpha = 1.0 - *(float*)oapiGetObjectParam (_hObj, OBJPRM_PLANET_CLOUDSHADOWCOL);
		if (prm.shadowalpha < 0.01)
			prm.bCloudShadow = false;
		if (cloudtilever == 1) { // legacy cloud engine
			clouddata = new CloudData;
			clouddata->cloudmgr = new CloudManager (gc, this);
			clouddata->cloudshadow = prm.bCloudShadow;
			if (clouddata->cloudshadow) {
				clouddata->shadowalpha = (float)prm.shadowalpha;
			}
			if (*(bool*)oapiGetObjectParam (_hObj, OBJPRM_PLANET_CLOUDMICROTEX)) {
				clouddata->cloudmgr->SetMicrotexture ("cloud1.dds");
				clouddata->microalt0 = *(double*)oapiGetObjectParam (_hObj, OBJPRM_PLANET_CLOUDMICROALTMIN);
				clouddata->microalt1 = *(double*)oapiGetObjectParam (_hObj, OBJPRM_PLANET_CLOUDMICROALTMAX);
			}
		} else { // v2 cloud engine
			DWORD maxlvl = (DWORD)*(int*)oapiGetObjectParam (_hObj, OBJPRM_PLANET_CLOUDMAXLEVEL);
			maxlvl = min (maxlvl, *(DWORD*)gc->GetConfigParam (CFGPRM_SURFACEMAXLEVEL));
			cloudmgr2 = new TileManager2<CloudTile> (this, maxlvl, 32);
		}
	} else {
		prm.bCloudShadow = false;
	}

	if (*(bool*)oapiGetObjectParam (_hObj, OBJPRM_PLANET_HASRINGS)) {
		double minrad = *(double*)oapiGetObjectParam (_hObj, OBJPRM_PLANET_RINGMINRAD);
		double maxrad = *(double*)oapiGetObjectParam (_hObj, OBJPRM_PLANET_RINGMAXRAD);
		ringmgr = new RingManager (this, minrad, maxrad);
		render_rad = (float)(rad*maxrad);
	} else {
		ringmgr = 0;
	}
	
	memcpy2 (&fog, oapiGetObjectParam (_hObj, OBJPRM_PLANET_FOGPARAM), sizeof (FogParam));
	prm.bFogEnabled = (fog.dens_0 > 0);

	patchres = 0;

	//if (*(bool*)gc->GetConfigParam(CFGPRM_ATMFOG)==false) prm.bFogEnabled = false;

	
	nbase = oapiGetBaseCount (_hObj);
	if (nbase)	vbase = new vBase*[nbase];
	else		vbase = NULL;
	for (DWORD i = 0; i < nbase; i++)
		vbase[i] = NULL;

	VESSEL *hVes = oapiGetFocusInterface();

	if (hVes) {
		if (Config->PreLBaseVis && _hObj==hVes->GetSurfaceRef()) {
			LogAlw("PreLoading Base Visuals");
			for (DWORD i=0;i<nbase;i++) {
				OBJHANDLE hBase = oapiGetBaseByIndex (_hObj, i);
				vbase[i] = new vBase (hBase, scn, this);
			}
		}
	} 
	
	mesh = NULL;
	if (surfmgr && surfmgr->GetMaxLevel() == 0) {
		char cbuf[256];
		oapiGetObjectName (hObj, cbuf, 256);
		OBJHANDLE hMesh = oapiLoadMesh (cbuf);
		if (hMesh) {
			mesh = new D3D9Mesh (gc, hMesh);
			oapiDeleteMesh (hMesh);
		}
	}

	MicroCfg.bEnabled = ParseMicroTextures();

	albedo = gc->GetFileParser()->GetAlbedo(hObj);
	LogMsg("vPlanet constructor exiting");
}

// ==============================================================

vPlanet::~vPlanet ()
{
	if (nbase) {
		for (DWORD i = 0; i < nbase; i++)
			if (vbase[i]) delete vbase[i];
		delete []vbase;
	}
	if (surfmgr) delete surfmgr;
	else if (surfmgr2) delete surfmgr2;
	if (cloudmgr2) delete cloudmgr2;

	if (MicroCfg.bLoaded) {
		SAFE_RELEASE(MicroCfg.Level[0].pTex);
		SAFE_RELEASE(MicroCfg.Level[1].pTex);
		SAFE_RELEASE(MicroCfg.Level[2].pTex);
	}

	if (clouddata) {
		delete clouddata->cloudmgr;
		delete clouddata;
	}
	if (hazemgr)  delete hazemgr;
	if (hazemgr2) delete hazemgr2;
	if (ringmgr)  delete ringmgr;
	if (mesh)     delete mesh;
}

// ==============================================================

bool vPlanet::CameraInAtmosphere() const
{
	double calt = CamDist() - size;
	double halt = GetHorizonAlt();
	if (prm.bAtm==false) return false;
	if (calt>halt) return false;
	return true;
}

// ==============================================================

double	vPlanet::GetHorizonAlt() const
{
	if (!prm.bAtm) return 0.0;
	if (!bScatter) return prm.atm_hzalt;
	return SPrm.height*9e3;
}

// ==============================================================

double vPlanet::GetMinElevation() const
{
	if (surfmgr2) return surfmgr2->GetMinElev();
	return 0.0f;
}


// ==============================================================

DWORD vPlanet::GetBaseCount()
{
	return nbase;
}

// ==============================================================

vBase* vPlanet::GetBaseByIndex(DWORD index)
{
	return vbase[index];
}

// ==============================================================

vBase* vPlanet::GetBaseByHandle(OBJHANDLE hBase)
{
	if (vbase) for (DWORD i=0;i<nbase;i++) if (vbase[i]) if (vbase[i]->Object()==hBase) return vbase[i];
	return NULL;
}

// ===========================================================================================
//
bool vPlanet::GetMinMaxDistance(float *zmin, float *zmax, float *dmin)
{
	if (mesh==NULL) return false;
	if (bBSRecompute) UpdateBoundingBox();

	D3DXVECTOR3 pos = D3DXVECTOR3(mWorld._41, mWorld._42, mWorld._43);

	float dst = D3DXVec3Length(&pos);

	*dmin = dst - float(oapiGetSize(hObj));
	*zmin = *dmin;
	*zmax = dst + float(oapiGetSize(hObj));

	return true;
}


// ===========================================================================================
//
void vPlanet::UpdateBoundingBox()
{
	bBSRecompute = false;
}

// ==============================================================

bool vPlanet::Update (bool bMainScene)
{
	_TRACE;
	if (!active) return false;
	
	vObject::Update(bMainScene);

	if (patchres==0) return true;

	// Check if micro textures needs loading
	if (MicroCfg.bEnabled && !MicroCfg.bLoaded && scn->GetCameraProxyVisual()==this) LoadMicroTextures();

	int i, j;
	float rad_scale = rad;
	bool rescale = false;
	dist_scale = 1.0f;

	if (cdist > maxdist) {
		rescale = true;
		dist_scale = (FLOAT)(max_centre_dist/cdist);
		prm.DistScale = dist_scale;
	}
	if (rescale) {
		rad_scale *= dist_scale;
		mWorld._41 *= dist_scale;
		mWorld._42 *= dist_scale;
		mWorld._43 *= dist_scale;
	}

	// scale up from template sphere radius 1
	mWorld._11 *= rad_scale; mWorld._12 *= rad_scale; mWorld._13 *= rad_scale;
	mWorld._21 *= rad_scale; mWorld._22 *= rad_scale; mWorld._23 *= rad_scale;
	mWorld._31 *= rad_scale; mWorld._32 *= rad_scale; mWorld._33 *= rad_scale;

	// cloud layer world matrix
	if (prm.bCloud) {
		double cloudrad = size + prm.cloudalt;
		prm.cloudrot = *(double*)oapiGetObjectParam (hObj, OBJPRM_PLANET_CLOUDROTATION);
		prm.cloudvis = (cdist < cloudrad ? 1:0);
		if (cdist > cloudrad*(1.0-1.5e-4)) prm.cloudvis |= 2;
		prm.bCloudFlatShadows = (cdist >= 1.05*size);
		//prm.bCloudFlatShadows = (cdist >= (size+GetHorizonAlt()));

		if (clouddata) {
			if (prm.cloudvis & 1) {
				clouddata->viewap = acos (size/cloudrad);
				if (size < cdist) clouddata->viewap += acos (size/cdist);
			} else {
				clouddata->viewap = 0;
			}

			float cloudscale = (float)(cloudrad/size);

			// world matrix for cloud shadows on the surface
			memcpy (&clouddata->mWorldC0, &mWorld, sizeof (D3DMATRIX));
			if (prm.cloudrot) {
				static D3DXMATRIX crot (1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1);
				crot._11 =   crot._33 = (float)cos(prm.cloudrot);
				crot._13 = -(crot._31 = (float)sin(prm.cloudrot));
				D3DXMatrixMultiply (&clouddata->mWorldC0, &crot, &clouddata->mWorldC0);
			}

			// world matrix for cloud layer
			memcpy (&clouddata->mWorldC, &clouddata->mWorldC0, sizeof (D3DMATRIX));
			for (i = 0; i < 3; i++)
				for (j = 0; j < 3; j++) {
					clouddata->mWorldC.m[i][j] *= cloudscale;
				}

			// set microtexture intensity
			double alt = cdist-rad;
			double lvl = (clouddata->microalt1-alt)/(clouddata->microalt1-clouddata->microalt0);
			clouddata->cloudmgr->SetMicrolevel (max (0, min (1, lvl)));
		}
	}

	// check all base visuals
	if (nbase) {	
		VECTOR3 pos, cpos = scn->GetCameraGPos();
		double scale = (double)scn->ViewH()/scn->GetTanAp();
		for (DWORD i = 0; i < nbase; i++) {
			OBJHANDLE hBase = oapiGetBaseByIndex (hObj, i);
			oapiGetGlobalPos (hBase, &pos);
			double rad = oapiGetSize (hBase);
			double dst = dist (pos, cpos);
			double apprad = rad*scale/dst;

			// -----------------------------------------------------------------
			//
			if (bMainScene) {
				if (vbase[i]) { // base visual exists
					if (apprad < 1.0) { // out of visual range
						delete vbase[i];
						vbase[i] = 0;
					}
				} else {        // base visual doesn't exist
					if (apprad > 2.0) { // within visual range
						vbase[i] = new vBase (hBase, scn, this);
					}
				}
			}

			// Toggle surface base on/off based on visual size -----------------
			// 
			if (vbase[i]) {
				if (apprad < 1.0) vbase[i]->Activate(false);
				else if (apprad > 2.0) vbase[i]->Activate(true);
				vbase[i]->Update(bMainScene);	
			}
		}
	}
	return true;
}

// ==============================================================

void vPlanet::CheckResolution()
{
	double alt = max (1.0, cdist-rad);
	double apr = rad * scn->ViewH()*0.5 / (alt * scn->GetTanAp());
	// apparent planet radius in units of screen pixels

	DWORD new_patchres;
	double ntx;

	if (apr < 2.5) { // render planet as 2x2 pixels
		renderpix = true;
		new_patchres = 0;
		ntx = 0;
	} else {
		renderpix = false;
		ntx = PI*2.0 * apr;

		static const double scal2 = 1.0/log(2.0);
		const double shift = (surfmgr2 ? 6.0 : 5.0); // reduce level for tile mgr v2, because of increased patch size
		new_patchres = min (max ((DWORD)(scal2*log(ntx)-shift),1), max_patchres);
	}
	if (new_patchres != patchres) {
		if (hashaze) {
			if (new_patchres < 1) {
				if (hazemgr) { delete hazemgr; hazemgr = NULL; }
				if (hazemgr2) { delete hazemgr2; hazemgr2 = NULL; }
			} else {
				if (tilever>1 && bScatter) { 
					if (!hazemgr2) hazemgr2 = new HazeManager2 (scn->GetClient(), this); 
				}
				else if (!hazemgr) hazemgr = new HazeManager (scn->GetClient(), this);
			}
		}
		if (ringmgr) {
			int ringres = (new_patchres <= 3 ? 0 : new_patchres <= 4 ? 1:2);
			ringmgr->SetMeshRes (ringres);
		}
		patchres = new_patchres;
	}
}

// ==============================================================

void vPlanet::RenderZRange (double *nplane, double *fplane)
{
	double d = dotp (scn->GetCameraGDir(), cpos);
	*fplane = max (1e3, d+rad*1.2);
	*nplane = max (1e0, d-rad*1.2);
	*fplane = min (*fplane, *nplane*1e5);
}

// ==============================================================

bool vPlanet::Render(LPDIRECT3DDEVICE9 dev)
{
	_TRACE;
	if (!active) return false;

	D3D9Effect::UpdateEffectCamera(hObj);
	D3D9Effect::FX->SetFloat(D3D9Effect::eDistScale, 1.0f/float(dist_scale));

	PlanetRenderer::InitializeScattering(this);
	PlanetRenderer::SetViewProjectionMatrix(scn->GetProjectionViewMatrix());

	if (DebugControls::IsActive()) {
		// DWORD flags  = *(DWORD*)gc->GetConfigParam(CFGPRM_GETDEBUGFLAGS);
		DWORD displ  = *(DWORD*)gc->GetConfigParam(CFGPRM_GETDISPLAYMODE);
		vObject *vSel =  DebugControls::GetVisual();
		if (vSel && displ>0) {
			if (vSel->GetObjectA()) {
				if (oapiGetObjectType(vSel->GetObjectA())==OBJTP_VESSEL) return false;
			}
		}
	}

	pCurrentVisual = this;

	if (renderpix) { // render as 2x2 pixel block
		RenderDot (dev);
	} else {             // render as sphere
		DWORD amb = prm.amb0col;
		bool ringpostrender = false;
		float fogfactor;

		D3DCOLOR bg		= scn->GetBgColour();
		prm.bFog		= prm.bFogEnabled;
		prm.bTint		= prm.bFogEnabled;
		prm.bAddBkg		= ((bg & 0xFFFFFF) && (hObj != scn->GetCameraProxyBody()));
		prm.FogDensity	= 0.0f;
		prm.SkyColor	= D3DXCOLOR(bg);
		prm.AmbColor	= D3DXCOLOR(0,0,0,0);
		prm.FogColor	= D3DXCOLOR(0,0,0,0);
		prm.TintColor	= D3DXCOLOR(0,0,0,0);
		prm.SunDir		= _D3DXVECTOR3(SunDirection());

		if (ringmgr) {
			ringmgr->Render(dev, mWorld, false);
			dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);	
		}
		
		if (hazemgr2) {
			double apr = 180.0 * scn->GetCameraAperture() / (scn->GetCameraAspect() * PI);
			hazemgr2->Render(mWorld, float(apr));
		}

		if (prm.bCloud && (prm.cloudvis & 1))
			RenderCloudLayer (dev, D3DCULL_NONE);      // render clouds from below

		if (hazemgr) hazemgr->Render(dev, mWorld);       // horizon ring
	
		
		if (prm.bAtm) {
			if (ModLighting (amb))
				prm.AmbColor = D3DXCOLOR(amb);
		}

		if (prm.bFog) { // set up distance fog
			double h = max (1.0, cdist-size);

			VECTOR3 fogcol = fog.col;
			double h_ref = fog.alt_ref;   // 3e3;
			double fog_0 = fog.dens_0;    // 5e-5;
			double fog_ref = fog.dens_ref; // 3e-5;
			double h_max = size*1.5; // At this altitude, fog effect drops to zero
			double scl = h_ref*fog_ref;

			if (h < h_ref) {
				// linear zone
				fogfactor = (float)(h/h_ref * (fog_ref-fog_0) + fog_0);
			} else {
				// hyperbolic zone: fogfactor = a/(h+b) + c
				// a, b and c are designed such that
				// * fogfactor(h) is continuous at h = h_ref
				// * d fogfactor / dh is continuous at h = h_ref
				// * fogfactor(h_max) = 0
				double b = - (fog_ref*h_max + (fog_ref-fog_0)*(h_max-h_ref)) / (fog_ref + (fog_ref-fog_0)/h_ref * (h_max-h_ref));
				double a = fog_ref*(h_ref+b)*(h_max+b)/(h_max-h_ref);
				double c = -a/(h_max+b);
				fogfactor = (float)(a/(h+b)+c);
			}

			if (fogfactor < 0.0) prm.bFog = false;
			else {
				// day/nighttime fog lighting
				VECTOR3 ppos;
				oapiGetGlobalPos (hObj, &ppos);
				double cosa = dotp (unit(ppos), unit(cpos));
				double bright = 1.0 * max (0.0, min (1.0, cosa + 0.3));
				float rfog = (float)(bright*(min(1.0,fogcol.x)+0.0)); // "whiten" the fog colour
				float gfog = (float)(bright*(min(1.0,fogcol.y)+0.0));
				float bfog = (float)(bright*(min(1.0,fogcol.z)+0.0));
				prm.FogDensity = fogfactor;
				prm.FogColor = D3DXCOLOR(rfog, gfog, bfog, 1.0f);
			}
		}


		if (mesh) {
			mesh->SetSunLight((D3D9Light *)scn->GetLight(-1));
			mesh->Render(dev, &mWorld, RENDER_ASTEROID);
		} else {
			RenderSphere (dev);                               
		}

		if (nbase) RenderBaseStructures (dev);

		if (prm.bCloud && (prm.cloudvis & 2))
			RenderCloudLayer (dev, D3DCULL_CCW);	  // render clouds from above

		if (hazemgr) hazemgr->Render (dev, mWorld, true); // haze across planet disc
		if (ringmgr) {
			ringmgr->Render (dev, mWorld, true);
			dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);	
		}

	}
	return true;
}

// ==============================================================

void vPlanet::RenderBeacons(LPDIRECT3DDEVICE9 dev)
{
	// Beacons rendered elsewhere before the cloud layer	
}


// ==============================================================

void vPlanet::RenderSphere (LPDIRECT3DDEVICE9 dev)
{
	float fogfactor;
	D3D9Effect::FX->GetFloat(D3D9Effect::eFogDensity, &fogfactor);

	if (surfmgr2) {
		if (cdist>=1.3*rad && cdist>3e6) surfmgr2->Render (dmWorld, false, prm);
		else							 surfmgr2->Render (dmWorld, true,  prm);
	} 
	else {
		dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);	
		if (prm.bFog) D3D9Effect::FX->SetFloat(D3D9Effect::eFogDensity, fogfactor/dist_scale);
		surfmgr->SetAmbientColor(prm.AmbColor);
		surfmgr->Render (dev, mWorld, dist_scale, patchres, 0.0, prm.bFog); // surface
		if (prm.bFog) D3D9Effect::FX->SetFloat(D3D9Effect::eFogDensity, fogfactor);
	}

	if (nbase) {
		RenderBaseSurfaces (dev);                     // base surfaces
		RenderBaseShadows (dev, shadowalpha);         // base shadows
	}
	if (prm.bCloudShadow)
		RenderCloudShadows (dev);                	// cloud shadows

	if (bVesselShadow && hObj == oapiCameraProxyGbody())
	// cast shadows only on planet closest to camera
		scn->RenderVesselShadows (hObj, shadowalpha); // vessel shadows
}

// ==============================================================

void vPlanet::RenderCloudLayer (LPDIRECT3DDEVICE9 dev, DWORD cullmode)
{
	if (cullmode != D3DCULL_CCW) dev->SetRenderState (D3DRS_CULLMODE, cullmode);
	if (cloudmgr2)
		cloudmgr2->Render (dmWorld, false, prm);
	else
		clouddata->cloudmgr->Render (dev, clouddata->mWorldC, dist_scale, min(patchres,8), clouddata->viewap); // clouds
	if (cullmode != D3DCULL_CCW) dev->SetRenderState (D3DRS_CULLMODE, D3DCULL_CCW);
}

// ==============================================================

void vPlanet::RenderCloudShadows (LPDIRECT3DDEVICE9 dev)
{
	if (cloudmgr2) {
		if (prm.bCloudFlatShadows)
			cloudmgr2->RenderFlatCloudShadows (dmWorld, prm);
	} 
	else if (clouddata) { // legacy method
		float fogfactor;
		D3D9Effect::FX->GetFloat(D3D9Effect::eFogDensity, &fogfactor);
		if (prm.bFog) D3D9Effect::FX->SetFloat(D3D9Effect::eFogDensity, fogfactor/dist_scale);
		clouddata->cloudmgr->RenderShadow(dev, clouddata->mWorldC0, dist_scale, min(patchres,8), clouddata->viewap, clouddata->shadowalpha);	
		if (prm.bFog) D3D9Effect::FX->SetFloat(D3D9Effect::eFogDensity, fogfactor);
	}
}

// ==============================================================

void vPlanet::RenderBaseSurfaces(LPDIRECT3DDEVICE9 dev)
{
	for (DWORD i=0;i<nbase;i++) if (vbase[i]) {
		vbase[i]->RenderSurface(dev);	
		vbase[i]->RenderRunwayLights(dev);
	}
}

// ==============================================================

void vPlanet::RenderBaseShadows(LPDIRECT3DDEVICE9 dev, float depth)
{
	if (bObjectShadow) {
		for (DWORD i=0;i<nbase;i++) if (vbase[i]) vbase[i]->RenderGroundShadow(dev, depth);
		// reset device parameters
		dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
	}
}

// ==============================================================

void vPlanet::RenderBaseStructures (LPDIRECT3DDEVICE9 dev)
{
	for (DWORD i=0;i<nbase;i++) if (vbase[i]) vbase[i]->RenderStructures(dev);
	for (DWORD i=0;i<nbase;i++) if (vbase[i]) vbase[i]->RenderBeacons(dev);
}

// ==============================================================

bool vPlanet::ModLighting (DWORD &ambient)
{
	// modify ambient light level inside atmospheres as a function of sun elevation
	if (!prm.bAtm) return false;
	if (cdist >= size+prm.atm_href) return false;

	double alpha = acos (dotp (unit(scn->GetCameraGPos()), -unit(cpos)));
	// angular distance between sun and planet as seen from camera

	double sunelev = alpha - PI05; // elevation of sun above horizon (assuming camera on ground)
	if (sunelev < -14.0*RAD) return false;  // total darkness

	double rscale = (size-cdist)/prm.atm_href + 1.0;    // effect altitude scale (1 on ground, 0 at reference alt)
	double amb = prm.atm_amb0 * min (1.0, (sunelev+14.0*RAD)/(20.0*RAD)); // effect magnitude (dependent on sun elevation)
	if (amb < 0.05) return false;
	amb = max (0, amb-0.05);

	DWORD addamb = (DWORD)(amb*rscale*256.0);
	DWORD newamb = *(DWORD*)gc->GetConfigParam (CFGPRM_AMBIENTLEVEL) + addamb;
	ambient = 0;
	for (int i = 0; i < 4; i++)
		ambient |= min (255, newamb) << (i<<3);
	return true;
}

// ==============================================================

D3DXVECTOR3 vPlanet::GetSunLightColor(VECTOR3 vPos, float fAmbient, float fGlobalAmb)
{
	double fAlt = length(vPos) - size;
	if (fAlt>prm.SclHeight*20.0) return D3DXVECTOR3(1,1,1);

	float  rp = -float(SPrm.rpow);
	float  mp = -float(SPrm.mpow);

	D3DXVECTOR3 lambda4 = D3DXVECTOR3(pow(float(SPrm.red),rp), pow(float(SPrm.green),rp), pow(float(SPrm.blue),rp));
	D3DXVECTOR3 lambda2 = D3DXVECTOR3(pow(float(SPrm.red),mp), pow(float(SPrm.green),mp), pow(float(SPrm.blue),mp));
	
	D3DXVec3Normalize(&lambda4, &lambda4);
	D3DXVec3Normalize(&lambda2, &lambda2);

	D3DXVECTOR3 vOutTotSun = lambda4*float(SPrm.rout) + lambda2*float(SPrm.mie);
	D3DXVECTOR3 vRayInSct  = lambda4*float(SPrm.rin * SPrm.rout);

	double fDPS = max(0.34, dotp(unit(vPos), sundir));	
	double fDns = exp2(-fAlt * prm.InvSclHeight);

	return exp2(-vOutTotSun * float(fDns * AngleCoEff(fDPS)));
}

// ==============================================================
// Get a "semi" fixed surface reference point. Update if camera
// movement is greater that 2deg
//
VECTOR3 vPlanet::ReferencePoint()
{
	MATRIX3 mRot;
	oapiGetRotationMatrix(hObj, &mRot);
	VECTOR3 vLPos = unit(tmul(mRot, PosFromCamera()));
	if (dotp(vLPos, vRefPoint)<0.9993) vRefPoint = vLPos;
	return vRefPoint;
}

// ==============================================================

double vPlanet::AngleCoEff(double cd)
{
	cd = 1.0/max(0.0954, cd+0.2);
	double val = 0.0, x = 1.0;
	for (int i=0;i<8;i++) {	val += prm.ScatterCoEff[i]*x; x*=cd; }
	return val;
}

// ==============================================================

void vPlanet::UpdateAtmoConfig()
{
	prm.SclHeight	 = float(SPrm.height*1e3);
	prm.InvSclHeight = 1.0f / float(prm.SclHeight);
	double outer = size + SPrm.height * 12.0 * 1e3;
	double height = size + SPrm.height * 5.0;
	double angle = (PI-asin(size/height)) * DEG;

	SolveXScatter(prm.SclHeight, size, outer, prm.ScatterCoEff, 96.0, 8);
}


double GaussLobatto(double alt, double dir, double R0, double R1, double h0)
{
	double R = R0 + alt;

	double rdt = -R * cos(dir);
	double Ray = rdt + sqrt(R1*R1 - (R*R - rdt*rdt));	

	double p0 = Ray * 0.0; 
	double p1 = Ray * 0.2765; 
	double p2 = Ray * 0.7235; 
	double p3 = Ray * 1.0; 

	double a0 = sqrt(R*R + p0*p0 + 2.0*R*p0*cos(dir)) - R0;
	double a1 = sqrt(R*R + p1*p1 + 2.0*R*p1*cos(dir)) - R0;
	double a2 = sqrt(R*R + p2*p2 + 2.0*R*p2*cos(dir)) - R0;
	double a3 = sqrt(R*R + p3*p3 + 2.0*R*p3*cos(dir)) - R0;

	double s0 = exp2(-a0/h0);
	double s1 = exp2(-a1/h0);
	double s2 = exp2(-a2/h0);
	double s3 = exp2(-a3/h0);

	double sum = (s0*0.167 + s1*0.833 + s2*0.833 + s3*0.167) * Ray * 0.5 * 0.6931471806;

	return sum;
}

// ==============================================================

void vPlanet::DumpDebugFile()
{
	/*
	int samples = 100;
	double max_angle = 100.0;
	double delta = 3.1415*max_angle/float(samples*180);
	double angle = 0.0;
	double outer = size + (prm.SclHeight*8.0);

	FILE *fp = NULL;
	fopen_s(&fp, "OpticalDebug.txt", "w");
	if (fp==NULL) return;

	for (int i=0;i<samples;i++) {
		double exact	= ExactOpticalDepth(0.0, angle, size, outer, prm.SclHeight) / prm.SclHeight;
		double gauss	= GaussLobatto(0.0, angle, size, outer, prm.SclHeight) / prm.SclHeight;
		double accur    = OpticalDepth(0.0, cos(angle)) / double(prm.SclHeight);	
		angle += delta;
		fprintf(fp,"%d %6.6g %6.6g %6.6g\n", i, exact, accur, gauss);
	}
	fclose(fp);*/
}

ScatterParams * vPlanet::GetAtmoParams(int mode)
{
	if (!prm.bAtm || prm.atm_hzalt==0.0) return &SPrm;	// Return surface setup if a planet doesn't have atmosphere

	double alt  = saturate((CamDist()-size) / (GetHorizonAlt()*3.0));

	alt = sqrt(alt);

	if (mode==0 && alt>0.5) mode = 2;
	if (mode==0 && alt<0.5) mode = 1;

	if (mode==1) return &SPrm;		// Surface configuration 
	if (mode==2) return &OPrm;		// Orbital configuration
	
	// ----------------------------------------------------
	CPrm.aux1   = lerp(SPrm.aux1,	OPrm.aux1,		alt);
	CPrm.aux2   = lerp(SPrm.aux2,	OPrm.aux2,		alt);
	CPrm.aux3	= lerp(SPrm.aux3,	OPrm.aux3,		alt);
	CPrm.aux4	= lerp(SPrm.aux4,	OPrm.aux4,		alt);
	CPrm.balance= lerp(SPrm.balance,OPrm.balance,	alt);
	CPrm.depth  = lerp(SPrm.depth,	OPrm.depth,		alt);
	CPrm.expo   = lerp(SPrm.expo,	OPrm.expo,		alt);
	CPrm.mie    = lerp(SPrm.mie,	OPrm.mie,		alt);
	CPrm.mphase = lerp(SPrm.mphase, OPrm.mphase,	alt);
	CPrm.mpow	= lerp(SPrm.mpow,	OPrm.mpow,		alt);
	CPrm.rin	= lerp(SPrm.rin,	OPrm.rin,		alt);
	CPrm.rout	= lerp(SPrm.rout,	OPrm.rout,		alt);
	CPrm.rpow	= lerp(SPrm.rpow,	OPrm.rpow,		alt);
	// ----------------------------------------------------
	CPrm.red	= SPrm.red;
	CPrm.green	= SPrm.green;
	CPrm.blue   = SPrm.blue;
	CPrm.height = SPrm.height;
	CPrm.mheight= SPrm.mheight;
	CPrm.rphase = SPrm.rphase;

	return &CPrm;
}
	
	

// ==============================================================

bool vPlanet::LoadAtmoConfig(bool bOrbit)
{
	char name[32];
	char path[256];

	if (!oapiPlanetHasAtmosphere(hObj)) return false;

	oapiGetObjectName(hObj, name, 32);

	if (bOrbit) sprintf_s(path,"GC/%s.atmo.cfg",name);
	else		sprintf_s(path,"GC/%s.atms.cfg",name);

	FILEHANDLE hFile = oapiOpenFile(path, FILE_IN_ZEROONFAIL, CONFIG);

	if (!hFile) return false;

	LogAlw("Loading Atmospheric Configuration file [%s] Handle=0x%X",path,hFile);

	ScatterParams *prm;

	if (bOrbit) prm = &OPrm;
	else		prm = &SPrm;

	prm->orbit = bOrbit;

	oapiReadItem_float(hFile, "Red", prm->red);
	oapiReadItem_float(hFile, "Green", prm->green);
	oapiReadItem_float(hFile, "Blue", prm->blue);
	oapiReadItem_float(hFile, "RWaveDep", prm->rpow);
	oapiReadItem_float(hFile, "MWaveDep", prm->mpow);
	oapiReadItem_float(hFile, "ScaleHeight", prm->height);
	oapiReadItem_float(hFile, "DepthClamp", prm->depth);
	// -----------------------------------------------------------------
	oapiReadItem_float(hFile, "Exposure", prm->expo);
	oapiReadItem_float(hFile, "Balance", prm->balance);
	// -----------------------------------------------------------------
	oapiReadItem_float(hFile, "OutScatter", prm->rout);
	oapiReadItem_float(hFile, "InScatter", prm->rin);
	oapiReadItem_float(hFile, "RayleighPhase", prm->rphase);
	// -----------------------------------------------------------------
	oapiReadItem_float(hFile, "MieOffset", prm->mheight);
	oapiReadItem_float(hFile, "MiePower", prm->mie);
	oapiReadItem_float(hFile, "MiePhase", prm->mphase);
	// -----------------------------------------------------------------
	oapiReadItem_float(hFile, "Aux1", prm->aux1);
	oapiReadItem_float(hFile, "Aux2", prm->aux2);
	oapiReadItem_float(hFile, "Aux3", prm->aux3);
	oapiReadItem_float(hFile, "Aux4", prm->aux4);
	// -----------------------------------------------------------------
	
	oapiCloseFile(hFile, FILE_IN_ZEROONFAIL);

	UpdateAtmoConfig();
	return true;
	
}

// ==============================================================

void vPlanet::SaveAtmoConfig(bool bOrbit)
{
	char name[64];
	char path[256];

	oapiGetObjectName(hObj, name, 64);

	if (bOrbit) sprintf_s(path,"GC/%s.atmo.cfg",name);
	else		sprintf_s(path,"GC/%s.atms.cfg",name);

	FILEHANDLE hFile = oapiOpenFile(path, FILE_OUT, CONFIG);

	ScatterParams *prm;

	if (bOrbit) prm = &OPrm;
	else		prm = &SPrm;

	oapiWriteItem_float(hFile, "Red", prm->red);
	oapiWriteItem_float(hFile, "Green", prm->green);
	oapiWriteItem_float(hFile, "Blue", prm->blue);
	oapiWriteItem_float(hFile, "RWaveDep", prm->rpow);
	oapiWriteItem_float(hFile, "MWaveDep", prm->mpow);
	oapiWriteItem_float(hFile, "ScaleHeight", prm->height);
	oapiWriteItem_float(hFile, "DepthClamp", prm->depth);
	// -----------------------------------------------------------------
	oapiWriteItem_float(hFile, "Exposure", prm->expo);
	oapiWriteItem_float(hFile, "Balance", prm->balance);
	// -----------------------------------------------------------------
	oapiWriteItem_float(hFile, "OutScatter", prm->rout);
	oapiWriteItem_float(hFile, "InScatter", prm->rin);
	oapiWriteItem_float(hFile, "RayleighPhase", prm->rphase);
	// -----------------------------------------------------------------
	oapiWriteItem_float(hFile, "MieOffset", prm->mheight);
	oapiWriteItem_float(hFile, "MiePower", prm->mie);
	oapiWriteItem_float(hFile, "MiePhase", prm->mphase);
	// -----------------------------------------------------------------	
	oapiWriteItem_float(hFile, "Aux1", prm->aux1);
	oapiWriteItem_float(hFile, "Aux2", prm->aux2);
	oapiWriteItem_float(hFile, "Aux3", prm->aux3);
	oapiWriteItem_float(hFile, "Aux4", prm->aux4);
	// -----------------------------------------------------------------
	
	oapiCloseFile(hFile, FILE_OUT);

	DumpDebugFile();
}


// ===========================================================================================
//
bool vPlanet::ParseMicroTextures()
{
	if (Config->MicroMode==0) return false;	// Micro textures are disabled
	if (surfmgr2==NULL) return false; // Only supported with tile format 2 

	FILE* file = NULL;
	char cbuf[256];
	char fname[256];
	sprintf_s(fname, 256, "%sMicroTex.cfg", OapiExtension::GetConfigDir());

	memset(&MicroCfg, 0, sizeof(MicroCfg));

	fopen_s(&file, fname, "r");

	if (!file) {
		LogErr("Could not open MicroTex.cfg file");
		return false;
	}

	bool bFound = false;

	while (fgets(cbuf, 256, file)) {
	
		if (!strncmp(cbuf, "//", 2)) continue;

		if (!strncmp(cbuf, "BODY", 4)) {
			if (bFound) break;
			if (sscanf(cbuf, "BODY %s", fname)==1) {
				if (strcmp(fname, GetName())==0) bFound = true;
			}
			else {
				LogErr("Error in MicroTex.cfg");
				fclose(file);
				return false;
			}
		}
		
		if (bFound) {

			if(!strncmp(cbuf, "NORMALS", 7)) {
				int lvl;
				if (sscanf(cbuf, "NORMALS %d", &lvl)==1) {
					MicroCfg.bNormals = (lvl==1);
				}
				else {
					LogErr("Error in MicroTex.cfg");
					fclose(file);
					return false;
				}

			}

			if(!strncmp(cbuf, "LEVEL", 5)) {
				float reso; int lvl;
				if (sscanf(cbuf, "LEVEL %d %s %f", &lvl, fname, &reso)==3) {
					lvl = min(2, max(lvl,0));
					MicroCfg.Level[lvl].reso = double(reso);
					strcpy_s(MicroCfg.Level[lvl].file, 32, fname);
				}
				else {
					LogErr("Error in MicroTex.cfg");
					fclose(file);
					return false;
				}
			}
		}
	}

	fclose(file);

	return bFound;
}


// ===========================================================================================
//
bool vPlanet::LoadMicroTextures()
{
	LogOapi("Loading Micro Textures for %s", GetName());
	char file[256];
	for (int i=0;i<3;i++) {
		sprintf_s(file, 256, "Textures/%s", MicroCfg.Level[i].file);
		HR(D3DXCreateTextureFromFileA(GetDevice(), file, &MicroCfg.Level[i].pTex));
		D3DSURFACE_DESC desc;
		if (MicroCfg.Level[i].pTex) {
			MicroCfg.Level[i].pTex->GetLevelDesc(0, &desc);
			MicroCfg.Level[i].size = double(desc.Width) / MicroCfg.Level[i].reso;
			MicroCfg.Level[i].px = double(desc.Width);
			DWORD mips = MicroCfg.Level[i].pTex->GetLevelCount();
			LogOapi("Level %u, %s, %.1fpx/m, %.1fm, Mipmap count=%u", i, MicroCfg.Level[i].file, MicroCfg.Level[i].reso, MicroCfg.Level[i].size, mips);
		}
	}
	MicroCfg.bLoaded = true;
	for (int i=0;i<3;i++) if (!MicroCfg.Level[i].pTex) {
		MicroCfg.bEnabled = false;
		MicroCfg.bLoaded = false;
	}
	if (MicroCfg.bLoaded) LogOapi("Micro textures Loaded");
	else {
		LogOapi("Failed to load micro textures");
		SAFE_RELEASE(MicroCfg.Level[0].pTex);
		SAFE_RELEASE(MicroCfg.Level[1].pTex);
		SAFE_RELEASE(MicroCfg.Level[2].pTex);
	}

	return MicroCfg.bEnabled;
}
