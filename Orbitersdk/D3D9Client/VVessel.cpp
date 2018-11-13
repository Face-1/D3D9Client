// ==============================================================
// VVessel.cpp
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2006-2016 Martin Schweiger
//				 2010-2016 Jarmo Nikkanen (D3D9Client modification)
// ==============================================================

#include <set>
#include "VVessel.h"
#include "MeshMgr.h"
#include "Texture.h"
#include "AABBUtil.h"
#include "D3D9Surface.h"
#include "D3D9Catalog.h"
#include "D3D9Config.h"
#include "OapiExtension.h"
#include "DebugControls.h"
#include "D3D9Util.h"
#include "MaterialMgr.h"

using namespace oapi;

// ==============================================================
// Local prototypes

void TransformPoint (VECTOR3 &p, const D3DXMATRIX &T);
void TransformDirection (VECTOR3 &a, const D3DXMATRIX &T, bool normalise);
const char *value_string (double val);

// ==============================================================
// class vVessel (implementation)
//
// A vVessel is the visual representation of a vessel object.
// ==============================================================

vVessel::vVessel(OBJHANDLE _hObj, const Scene *scene): vObject (_hObj, scene)
{
	_TRACE;

	//@todo throw @ vObject or visObject???
	if (_hObj==NULL) throw std::invalid_argument("_hObj");

	vessel = oapiGetVesselInterface(_hObj);
	nmesh = 0;
	nanim = 0;
	nEnv  = 0;
	iFace = 0;
	sunLight = *scene->GetSun();
	tCheckLight = oapiGetSimTime()-1.0;
	animstate = NULL;
	vClass = 0;

	pMatMgr = new MatMgr(this, scene->GetClient());
	skinname[0] = NULL;
	for (int i = 0; i < ARRAYSIZE(pEnv); i++) pEnv[i] = NULL;
	
	if (strncmp(vessel->GetClassNameA(), "AMSO", 4) == 0) vClass = VCLASS_AMSO;
	if (strncmp(vessel->GetClassNameA(), "XR2Ravenstar", 12) == 0) vClass = VCLASS_XR2;

	bBSRecompute = true;
	ExhaustLength = 0.0f;
	LoadMeshes();
	
	// Initialize static animations
	//
	GrowAnimstateBuffer(vessel->GetAnimPtr(&anim));
	for (UINT i=0;i<nanim;i++) animstate[i] = anim[i].defstate;
	UpdateAnimations();
}


// ============================================================================================
//
vVessel::~vVessel ()
{
	SAFE_DELETE(pMatMgr);
	
	for (int i = 0; i < ARRAYSIZE(pEnv); i++) SAFE_RELEASE(pEnv[i]);

	LogAlw("Deleting Vessel Visual 0x%X ...",this);
	DisposeAnimations();
	DisposeMeshes();
	LogAlw("Vessel visual deleted succesfully");
}


// ============================================================================================
//
void vVessel::GlobalInit(D3D9Client *gc)
{
	_TRACE;
	gc->GetTexMgr()->LoadTexture("Reentry.dds", &defreentrytex, 0);
	gc->GetTexMgr()->LoadTexture("Exhaust.dds", &defexhausttex, 0);
}


// ============================================================================================
//
void vVessel::GlobalExit ()
{
	SAFE_DELETE(defexhausttex);
	SAFE_DELETE(defreentrytex);
	SAFE_DELETE(tHUD);
}


// ============================================================================================
//
void vVessel::clbkEvent(DWORD evnt, UINT context)
{
	switch (evnt) {

		case EVENT_VESSEL_INSMESH:
			bBSRecompute = true;
			InsertMesh(context);
			break;

		case EVENT_VESSEL_DELMESH:
			bBSRecompute = true;
			DelMesh(context);
			break;

		case EVENT_VESSEL_MESHVISMODE:
		{
			bBSRecompute = true;
			if (context < nmesh) {
				meshlist[context].vismode = vessel->GetMeshVisibilityMode(context);
			}
		} break;

		case EVENT_VESSEL_MESHOFS:
		{
			bBSRecompute = true;
			DWORD idx = (DWORD)context;
			if (idx < nmesh) {
				VECTOR3 ofs;
				vessel->GetMeshOffset (idx, ofs);
				if (length(ofs)) {
					if (meshlist[idx].trans==NULL) meshlist[idx].trans = new D3DXMATRIX;
					D3DMAT_Identity(meshlist[idx].trans);
					D3DMAT_SetTranslation(meshlist[idx].trans, &ofs);
				}
				else {
					SAFE_DELETE(meshlist[idx].trans);
				}
			}
		} break;

		case EVENT_VESSEL_MODMESHGROUP:
			ResetMesh(context);
			break;

		case EVENT_VESSEL_RESETANIM:
			ResetAnimations();
			break;

		case EVENT_VESSEL_CLEARANIM:
			ResetAnimations(context);
			break;

		case EVENT_VESSEL_DELANIM:
			DelAnimation(context);
			break;

		case EVENT_VESSEL_NEWANIM:
			InitNewAnimation(context);
			break;
	}
}


// ============================================================================================
//
DWORD vVessel::GetMeshCount()
{
	return nmesh;
}


// ============================================================================================
//
MESHHANDLE vVessel::GetMesh (UINT idx)
{
	return (idx < nmesh ? meshlist[idx].mesh : NULL);
}


// ============================================================================================
//
bool vVessel::HasExtPass()
{
	for (DWORD i=0;i<nmesh;i++) if (meshlist[i].vismode&MESHVIS_EXTPASS) return true;
	return false;
}


// ============================================================================================
//
const char *vVessel::GetSkinName() const
{
	if (skinname[0]==0) return NULL;
	return skinname;
}

// ============================================================================================
//
void vVessel::SetSkinName(const char *name)
{
	strncpy_s(skinname, 64, name, 63);
}

// ============================================================================================
//
void vVessel::PreInitObject()
{
	if (pMatMgr->LoadConfiguration()) {
		for (DWORD i=0;i<nmesh;i++) if (meshlist[i].mesh) pMatMgr->ApplyConfiguration(meshlist[i].mesh);
		pMatMgr->LoadCameraConfig();
	}
	else LogErr("Failed to load a custom configuration for %s",vessel->GetClassNameA());
}

// ============================================================================================
//
bool vVessel::Update(bool bMainScene)
{
	_TRACE;

	if (!active) return false;

	vObject::Update(bMainScene);

	UpdateAnimations();

	if (fabs(oapiGetSimTime()-tCheckLight)>0.1) {
		sunLight = *scn->GetSun();
		ModLighting(&sunLight);
	}

	bBSRecompute = true;

	return true;
}


// ============================================================================================
//
void vVessel::LoadMeshes()
{
	_TRACE;
	bBSRecompute = true;
	if (nmesh) DisposeMeshes();

	MESHHANDLE hMesh;
	const D3D9Mesh *mesh;
	VECTOR3 ofs;
	UINT idx;

	MeshManager *mmgr = gc->GetMeshMgr();

	nmesh = vessel->GetMeshCount();
	meshlist = new MESHREC[nmesh+1];

	memset2(meshlist, 0, nmesh*sizeof(MESHREC));

	LogAlw("Vessel(0x%X) %s has %u meshes",vessel,vessel->GetClassNameA(),nmesh);

	for (idx=0;idx<nmesh;idx++) {

		hMesh = vessel->GetMeshTemplate(idx);
		if (vClass==VCLASS_AMSO) mmgr->UpdateMesh(hMesh);
		mesh = mmgr->GetMesh(hMesh);

		if (hMesh!=NULL && mesh!=NULL) {
			// copy from preloaded template
			meshlist[idx].mesh = new D3D9Mesh(*mesh);
			meshlist[idx].mesh->SetClass(vClass);
		}
		else {
			// It's vital to use copy here for some reason
			hMesh = vessel->CopyMeshFromTemplate(idx);
			if (hMesh) {
				// load on the fly and discard after copying
				meshlist[idx].mesh = new D3D9Mesh(hMesh, false);
				meshlist[idx].mesh->SetClass(vClass);
				oapiDeleteMesh(hMesh);
			}
		}

		if (meshlist[idx].mesh) {
			meshlist[idx].vismode = vessel->GetMeshVisibilityMode(idx);
			vessel->GetMeshOffset(idx, ofs);
			LogAlw("Mesh(0x%X) Offset = (%g, %g, %g)", hMesh, ofs.x, ofs.y, ofs.z);
			if (length(ofs)) {
				meshlist[idx].trans = new D3DXMATRIX;
				D3DMAT_Identity(meshlist[idx].trans);
				D3DMAT_SetTranslation(meshlist[idx].trans, &ofs);
				// currently only mesh translations are supported
			}
		}
		else {
			LogWrn("Vessel %s has a NULL mesh in index %u",vessel->GetClassNameA(),idx);
		}
	}

	UpdateBoundingBox();

	LogOk("Loaded %u meshed for %s",nmesh,vessel->GetClassNameA());
}


// ============================================================================================
//
void vVessel::InsertMesh(UINT idx)
{
	_TRACE;

	VECTOR3 ofs=_V(0,0,0);

	UINT i;
	LPD3DXMATRIX pT = NULL;

	if (idx >= nmesh) { // append a new entry to the list
		MESHREC *tmp = new MESHREC[idx+1];
		if (nmesh) {
			memcpy2 (tmp, meshlist, nmesh*sizeof(MESHREC));
			delete []meshlist;
		}
		meshlist = tmp;
		for (i = nmesh; i <= idx; i++) { // zero any intervening entries
			meshlist[i].mesh = 0;
			meshlist[i].trans = 0;
			meshlist[i].vismode = 0;
		}
		nmesh = idx+1;
	}
	else if (meshlist[idx].mesh) { // replace existing entry
		SAFE_DELETE(meshlist[idx].mesh);
		SAFE_DELETE(meshlist[idx].trans);
	}

	// now add the new mesh
	MeshManager *mmgr = gc->GetMeshMgr();
	MESHHANDLE hMesh = vessel->GetMeshTemplate(idx);
	if (vClass==VCLASS_AMSO) mmgr->UpdateMesh(hMesh);
	const D3D9Mesh *mesh = mmgr->GetMesh(hMesh);


	if (hMesh && mesh) {
		meshlist[idx].mesh = new D3D9Mesh (*mesh);
		meshlist[idx].mesh->SetClass(vClass);
	} else if (hMesh = vessel->CopyMeshFromTemplate (idx)) {	// It's vital to use a copy here for some reason
		meshlist[idx].mesh = new D3D9Mesh (hMesh);
		meshlist[idx].mesh->SetClass(vClass);
		oapiDeleteMesh (hMesh);
	} else {
		meshlist[idx].mesh = 0;
	}

	if (meshlist[idx].mesh) {
		pMatMgr->ApplyConfiguration(meshlist[idx].mesh);
		meshlist[idx].vismode = vessel->GetMeshVisibilityMode (idx);
		vessel->GetMeshOffset (idx, ofs);
		if (length(ofs)) {
			meshlist[idx].trans = new D3DXMATRIX;
			D3DMAT_Identity (meshlist[idx].trans);
			D3DMAT_SetTranslation (meshlist[idx].trans, &ofs);
			// currently only mesh translations are supported
		} else {
			meshlist[idx].trans = 0;
		}
	}

	UpdateAnimations(idx);

	LogAlw("vVessel(0x%X)::InsertMesh(%u) hMesh=0x%X offset=(%g, %g, %g)",this,idx, hMesh, ofs.x, ofs.y, ofs.z);
}


// ============================================================================================
//
void vVessel::ResetMesh(UINT idx)
{
	LogAlw("MeshModified Event = 0x%X", idx);

	VECTOR3 ofs = _V(0, 0, 0);

	if (idx < nmesh) {

		MESHHANDLE hMesh = vessel->GetMesh(this, idx);

		if (hMesh) {

			meshlist[idx].mesh->ReLoadMeshFromHandle(hMesh);

			pMatMgr->ApplyConfiguration(meshlist[idx].mesh);

			meshlist[idx].vismode = vessel->GetMeshVisibilityMode(idx);
			vessel->GetMeshOffset(idx, ofs);

			if (length(ofs)) {
				if (!meshlist[idx].trans) meshlist[idx].trans = new D3DXMATRIX;
				D3DMAT_Identity(meshlist[idx].trans);
				D3DMAT_SetTranslation(meshlist[idx].trans, &ofs);
			}
			else {
				SAFE_DELETE(meshlist[idx].trans);
			}
		}
	}
}


// ============================================================================================
//
void vVessel::DisposeMeshes()
{
	if (nmesh && meshlist) {
		for (UINT i = 0; i < nmesh; i++) {
			SAFE_DELETE(meshlist[i].mesh);
			SAFE_DELETE(meshlist[i].trans);
		}
	}
	if (meshlist) delete[] meshlist;
	meshlist = 0;
	nmesh = 0;
}


// ============================================================================================
//
void vVessel::DelMesh(UINT idx)
{
	if (idx==0xFFFFFFFF) {
		DisposeMeshes();
		return;
	}

	if (idx >= nmesh) return;
	if (!meshlist[idx].mesh) return;

	SAFE_DELETE(meshlist[idx].mesh);
	SAFE_DELETE(meshlist[idx].trans);
}


// ============================================================================================
//
void vVessel::InitNewAnimation (UINT idx)
{
	// vessel->GetAnimPtr(&anim) returns invalid data here. New idx is not yet included in anim[]
	GrowAnimstateBuffer(idx+1);
	animstate[idx] = -1.0;
}


// ============================================================================================
//
void vVessel::GrowAnimstateBuffer (UINT newSize)
{
	if (newSize > nanim) // append a new entries to the list
	{
		double *pTmp = new double[newSize]();
		if (nanim) {
			memcpy2(pTmp, animstate, nanim*sizeof(double));
			delete []animstate;
		}
		animstate = pTmp;
		nanim = newSize;
	}
}


// ============================================================================================
//
void vVessel::DisposeAnimations ()
{
	if (nanim) {
		LogAlw("Vessel(%s) Vis=0x%X: deleting %u animations", name, this, nanim);
		delete []animstate;
		nanim = 0;
	}
}


// ============================================================================================
//
void vVessel::ResetAnimations (UINT reset/*=1*/)
{
	bBSRecompute = true;

	// Reset transformation matrices
	for (DWORD i=0;i<nmesh;i++) { if (meshlist[i].mesh) meshlist[i].mesh->ResetTransformations(); }

	// nanim should stay the same, but just in case
	// GrowAnimstateBuffer(vessel->GetAnimPtr(&anim));

	if (reset == 1) for (UINT i=0;i<nanim;++i) animstate[i] = anim[i].defstate; // reset to default animation state	
}


// ============================================================================================
//
void vVessel::DelAnimation (UINT idx)
{
	// Do nothing here. Orbiter never reduces the animation buffer size. (i.e. anim[])
	// VESSEL::GetAnimPtr() returns highest existing animation ID + 1, not the actual animation count
}


// ============================================================================================
//
void vVessel::UpdateAnimations (int mshidx)
{
	double newstate;

	//GrowAnimstateBuffer(vessel->GetAnimPtr(&anim));

	UINT x = vessel->GetAnimPtr(&anim);
	if (x != nanim) {
		LogErr("vessel->GetAnimPtr(&anim) returns unexpected value in UpdateAnimations %u vs %u", x, nanim);
		GrowAnimstateBuffer(x);
	}
	
	if (mshidx>=0) {

		// Reset animations when a mesh is added to a vessel --------------------
		//
		for (UINT i = 0; i < nanim; ++i) {
			for (UINT k = 0; k < anim[i].ncomp; ++k) {
				if (anim[i].comp[k]->trans->mesh == mshidx) animstate[i] = anim[i].defstate;
			}
		}
	}
	else {

		// Initialize new dynamic animations -------------------------------------
		//
		for (UINT k=0;k<nanim;k++) if (animstate[k]<-0.5) {
			animstate[k] = anim[k].defstate;
			// Call ResetTransformations for all related meshes
			for (UINT i = 0; i < anim[k].ncomp; ++i) {
				UINT m = anim[k].comp[i]->trans->mesh;
				if (m<nmesh) if (meshlist[m].mesh) meshlist[m].mesh->ResetTransformations();
			}
		}
	}

	// Update animations ---------------------------------------------
	//
	for (UINT i = 0; i < nanim; i++) {
		if (!anim[i].ncomp) continue;
		if (animstate[i] != (newstate = anim[i].state)) {
			Animate(i, newstate, mshidx);
			animstate[i] = newstate;
		}
	}
}


// ============================================================================================
//
bool vVessel::IsInsideShadows()
{
	D3DXVECTOR3 bc;
	const Scene::SHADOWMAPPARAM *shd = scn->GetSMapData();
	D3DXVec3TransformCoord(&bc, &D3DXVECTOR3f4(BBox.bs), &mWorld);
	bc = bc - shd->pos;
	float x = D3DXVec3Dot(&bc, &(shd->ld));

	if (sqrt(D3DXVec3Dot(&bc, &bc) - x*x) < (shd->rad - BBox.bs.w)) return true;

	return false;
}


// ============================================================================================
//
bool vVessel::IntersectShadowTarget()
{
	D3DXVECTOR3 bc;
	const Scene::SHADOWMAPPARAM *shd = scn->GetSMapData();
	D3DXVec3TransformCoord(&bc, &D3DXVECTOR3f4(BBox.bs), &mWorld);
	bc = bc - shd->pos;
	if (D3DXVec3Length(&bc) < (shd->rad + BBox.bs.w)) return true;
	return false;
}


// ============================================================================================
//
bool vVessel::GetMinMaxLightDist(float *mind, float *maxd)
{
	D3DXVECTOR3 bc;
	const Scene::SHADOWMAPPARAM *shd = scn->GetSMapData();
	D3DXVec3TransformCoord(&bc, &D3DXVECTOR3f4(BBox.bs), &mWorld);
	bc = bc - shd->pos;
	float x = D3DXVec3Dot(&bc, &(shd->ld));

	//if (x > BBox.bs.w) return false; //Behind

	if (sqrt(D3DXVec3Dot(&bc, &bc) - x*x) > (shd->rad + BBox.bs.w)) return false;

	*mind = min(*mind, x - shd->rad);
	*maxd = max(*maxd, x + shd->rad);

	return true;
}


// ============================================================================================
//
bool vVessel::Render(LPDIRECT3DDEVICE9 dev)
{
	_TRACE;
	if (!active) return false;
	pCurrentVisual = this; // Set current visual for mesh debugger
	UpdateBoundingBox();
	bool bRet = Render(dev, false);
	if (oapiCameraInternal()==false) RenderReentry(dev);
	return bRet;
}


// ============================================================================================
//
bool vVessel::Render(LPDIRECT3DDEVICE9 dev, bool internalpass)
{
	_TRACE;
	if (!active) return false;

	UINT i, mfd;

	DWORD flags = *(DWORD*)gc->GetConfigParam(CFGPRM_GETDEBUGFLAGS);
	DWORD displ = *(DWORD*)gc->GetConfigParam(CFGPRM_GETDISPLAYMODE);

	bool bCockpit = (oapiCameraInternal() && (hObj == oapiGetFocusObject()));
	// render cockpit view

	bool bVC = (bCockpit && (oapiCockpitMode() == COCKPIT_VIRTUAL));
	// render virtual cockpit

	if (scn->GetRenderPass() == RENDERPASS_CUSTOMCAM) bCockpit = bVC = false;
	// Always render exterior view for custom cams

	if (scn->GetRenderPass() == RENDERPASS_ENVCAM) bCockpit = bVC = false;
	// Always render exterior view for envmaps

	static VCHUDSPEC hudspec_;
	const VCHUDSPEC *hudspec = &hudspec_;
	static bool gotHUDSpec(false);
	const VCMFDSPEC *mfdspec[MAXMFD] = { NULL };
	
	if (vessel->GetAtmPressure()>1.0) D3D9Effect::FX->SetBool(D3D9Effect::eInSpace, false);
	else							  D3D9Effect::FX->SetBool(D3D9Effect::eInSpace, true);

	const Scene::SHADOWMAPPARAM *shd = scn->GetSMapData();

	float s = float(shd->size);
	float sr = 2.0f * shd->rad / s;
	float qw = 1.0f / float(Config->ShadowMapSize);

	HR(D3D9Effect::FX->SetBool(D3D9Effect::eEnvMapEnable, false));
	HR(D3D9Effect::FX->SetMatrix(D3D9Effect::eLVP, &shd->mViewProj));

	if (shd->pShadowMap && (scn->GetRenderPass() == RENDERPASS_MAINSCENE)) {
		HR(D3D9Effect::FX->SetTexture(D3D9Effect::eShadowMap, shd->pShadowMap));
		//HR(D3D9Effect::FX->SetVector(D3D9Effect::eSHD, &D3DXVECTOR4(sr, 1.0f/s, float(oapiRand()), float(CamDist()))));
		HR(D3D9Effect::FX->SetVector(D3D9Effect::eSHD, &D3DXVECTOR4(sr, 1.0f / s, float(oapiRand()), 1.0f/shd->depth)));
		HR(D3D9Effect::FX->SetBool(D3D9Effect::eShadowToggle, true));
	}
	else {
		HR(D3D9Effect::FX->SetBool(D3D9Effect::eShadowToggle, false));
	}


	// Check VC MFD screen resolutions ------------------------------------------------
	//
	if (bVC && internalpass) {
		for (mfd = 0; mfd < MAXMFD; mfd++) gc->GetVCMFDSurface(mfd, &mfdspec[mfd]);
		gotHUDSpec = !!gc->GetVCHUDSurface(&hudspec);
	}

	// Reduce sunlight for virtual cockpit
	//
	D3D9Sun sunLightVC = sunLight;
	sunLightVC.Color *= 0.5f;


	// Render Exterior and Interior (VC) meshes --------------------------------------------
	//
	for (i=0;i<nmesh;i++) {

		if (!meshlist[i].mesh) continue;

		uCurrentMesh = i; // Used for debugging

		// check if mesh should be rendered in this pass
		WORD vismode = meshlist[i].vismode;

		if (DebugControls::IsActive()) if (displ>1) vismode = MESHVIS_ALWAYS;

		if (vismode==0) continue;

		if (internalpass==false) {
			if (vismode==MESHVIS_VC) continue; // Added 3-jan-2011 to prevent VC interior double rendering during exterior and interior passes
			if ((vismode&MESHVIS_EXTPASS)==0 && bCockpit) continue;
		}

		if (bCockpit) {
			if (internalpass && (vismode & MESHVIS_EXTPASS)) continue;
			if (!(vismode & MESHVIS_COCKPIT)) {
				if ((!bVC) || (!(vismode & MESHVIS_VC))) continue;
			}
		} else {
			if (!(vismode & MESHVIS_EXTERNAL)) continue;
		}

		D3DXMATRIX mWT;
		LPD3DXMATRIX pWT;

		// transform mesh
		if (meshlist[i].trans) pWT = D3DXMatrixMultiply(&mWT, (const D3DXMATRIX *)meshlist[i].trans, &mWorld);
		else pWT = &mWorld;


		if (bVC && internalpass) meshlist[i].mesh->SetSunLight(&sunLightVC);
		else					 meshlist[i].mesh->SetSunLight(&sunLight);


		if (bVC && internalpass) {
			for (mfd=0;mfd<MAXMFD;mfd++) {
				if (mfdspec[mfd] && mfdspec[mfd]->nmesh == i) {
					meshlist[i].mesh->SetMFDScreenId(mfdspec[mfd]->ngroup, 1 + mfd);
				}
			}
		}


		// Render vessel meshes --------------------------------------------------------------------------
		//
		if (scn->GetRenderPass() == RENDERPASS_SHADOWMAP) meshlist[i].mesh->RenderShadows(0.0f, pWT, true);
		else {
			if (internalpass) meshlist[i].mesh->Render(pWT, RENDER_VC, NULL, 0);
			else 			  meshlist[i].mesh->Render(pWT, RENDER_VESSEL, pEnv, nEnv);
		}
		

		// render VC HUD and MFDs ------------------------------------------------------------------------
		//
		if (scn->GetRenderPass() == RENDERPASS_MAINSCENE) {
			if (bVC && internalpass && gotHUDSpec) {
				if (hudspec->nmesh == i) {
					meshlist[i].mesh->SetMFDScreenId(hudspec->ngroup, 0x100);
				}
			}
		}
	}

	// Shutdown shadows to prevent from causing problems
	HR(D3D9Effect::FX->SetBool(D3D9Effect::eShadowToggle, false));

	if (scn->GetRenderPass() == RENDERPASS_MAINSCENE) {
		if (DebugControls::IsActive()) {
			if (flags&DBG_FLAGS_SELVISONLY && this != DebugControls::GetVisual()) return true;
			if (flags&DBG_FLAGS_BOXES && !internalpass) {
				D3DXMATRIX id;
				D3D9Effect::RenderBoundingBox(&mWorld, D3DXMatrixIdentity(&id), &BBox.min, &BBox.max, &D3DXVECTOR4(1, 0, 0, 0.75f));
			}
		}
	}

	HR(D3D9Effect::FX->SetBool(D3D9Effect::eEnvMapEnable, false))

	return true;
}


// ============================================================================================
//
void vVessel::RenderAxis(LPDIRECT3DDEVICE9 dev, D3D9Pad *pSkp)
{
	const double threshold = 0;//0.25; // threshold for forces to be drawn
	VECTOR3 vector;
	float lscale = 1e-3f;
	float alpha;
	double len = 1e-9f; // avoids division by zero if len is not updated

	DWORD bfvmode = *(DWORD*)gc->GetConfigParam(CFGPRM_SHOWBODYFORCEVECTORSFLAG);
	float sclset  = *(float*)gc->GetConfigParam(CFGPRM_BODYFORCEVECTORSSCALE);
	float scale   = float(oapiGetSize(hObj)) / 50.0f;

	// -------------------------------------
	// Render Body Force Vectors

	if (bfvmode & BFV_ENABLE)
	{
		char label[64];
		bool bLog;

		if (bfvmode & BFV_SCALE_LOG) bLog = true;
		else                         bLog = false;

		if (!bLog) {
			if (bfvmode & BFV_DRAG) { vessel->GetDragVector(vector); if (length(vector)>len) len = length(vector); }
			if (bfvmode & BFV_WEIGHT) {	vessel->GetWeightVector(vector); if (length(vector)>len) len = length(vector); }
			if (bfvmode & BFV_THRUST) {	vessel->GetThrustVector(vector); if (length(vector)>len) len = length(vector); }
			if (bfvmode & BFV_LIFT) { vessel->GetLiftVector(vector); if (length(vector)>len) len = length(vector); }
			if (bfvmode & BFV_TOTAL) { vessel->GetForceVector(vector); if (length(vector)>len) len = length(vector); }
			if (bfvmode & BFV_TORQUE) {	vessel->GetTorqueVector(vector); if (length(vector)>len) len = length(vector); }

			lscale = float(oapiGetSize(hObj)*sclset/len);
		}
		else {
			lscale = float(oapiGetSize(hObj))*sclset/50.0f;
		}

		alpha = *(float*)gc->GetConfigParam(CFGPRM_BODYFORCEVECTORSOPACITY);

		if (alpha > 1e-9) // skip all this when opacity is to small (ZEROish)
		{
			if (bfvmode & BFV_DRAG) {
				vessel->GetDragVector(vector);
				if (length(vector) > threshold) {
					RenderAxisVector(pSkp, &D3DXCOLOR(1,0,0,alpha), vector, lscale, scale, bLog);
					sprintf_s(label, 64, "D = %sN", value_string(length(vector)));
					RenderAxisLabel(pSkp, &D3DXCOLOR(1,0,0,alpha), vector, lscale, scale, label, bLog);
				}
			}

			if (bfvmode & BFV_WEIGHT) {
				vessel->GetWeightVector(vector);
				if (length(vector) > threshold) {
					RenderAxisVector(pSkp, &D3DXCOLOR(1,1,0,alpha), vector, lscale, scale, bLog);
					sprintf_s(label, 64, "G = %sN", value_string(length(vector)));
					RenderAxisLabel(pSkp, &D3DXCOLOR(1,1,0,alpha), vector, lscale, scale, label, bLog);
				}
			}

			if (bfvmode & BFV_THRUST) {
				vessel->GetThrustVector(vector);
				if (length(vector) > threshold) {
					RenderAxisVector(pSkp, &D3DXCOLOR(0,0,1,alpha), vector, lscale, scale, bLog);
					sprintf_s(label, 64, "T = %sN", value_string(length(vector)));
					RenderAxisLabel(pSkp, &D3DXCOLOR(0,0,1,alpha), vector, lscale, scale, label, bLog);
				}
			}

			if (bfvmode & BFV_LIFT) {
				vessel->GetLiftVector(vector);
				if (length(vector) > threshold) {
					RenderAxisVector(pSkp, &D3DXCOLOR(0,1,0,alpha), vector, lscale, scale, bLog);
					sprintf_s(label, 64, "L = %sN", value_string(length(vector)));
					RenderAxisLabel(pSkp, &D3DXCOLOR(0,1,0,alpha), vector, lscale, scale, label, bLog);
				}
			}

			if (bfvmode & BFV_TOTAL) {
				vessel->GetForceVector(vector);
				if (length(vector) > threshold) {
					RenderAxisVector(pSkp, &D3DXCOLOR(1,1,1,alpha), vector, lscale, scale, bLog);
					sprintf_s(label, 64, "F = %sN", value_string(length(vector)));
					RenderAxisLabel(pSkp, &D3DXCOLOR(1,1,1,alpha), vector, lscale, scale, label, bLog);
				}
			}

			if (bfvmode & BFV_TORQUE) {
				vessel->GetTorqueVector(vector);
				if (length(vector) > threshold) {
					RenderAxisVector(pSkp, &D3DXCOLOR(1,0,1,alpha), vector, lscale, scale, bLog);
					sprintf_s(label, 64, "T = %sNm", value_string(length(vector)));
					RenderAxisLabel(pSkp, &D3DXCOLOR(1,0,1,alpha), vector, lscale, scale, label, bLog);
				}
			}
		}
	}

	// -------------------------------------
	// Render Axes

	DWORD scamode = *(DWORD*)gc->GetConfigParam(CFGPRM_SHOWCOORDINATEAXESFLAG);

	if (scamode&SCA_ENABLE && scamode&SCA_VESSEL)
	{
		float alpha   = *(float*)gc->GetConfigParam(CFGPRM_COORDINATEAXESOPACITY);

		if (alpha > 1e-9) // skip all this when opacity is to small (ZEROish)
		{
			float sclset  = *(float*)gc->GetConfigParam(CFGPRM_COORDINATEAXESSCALE);
			float scale   = float(oapiGetSize(hObj))/50.0f;
			float ascale  = float(oapiGetSize(hObj))*sclset*0.5f;

			RenderAxisVector(pSkp, &D3DXCOLOR(1,1,1,alpha), _V(1,0,0), ascale, scale);
			RenderAxisLabel(pSkp, &D3DXCOLOR(1,1,1,alpha), _V(1,0,0), ascale, scale, "X");

			RenderAxisVector(pSkp, &D3DXCOLOR(1,1,1,alpha), _V(0,1,0), ascale, scale);
			RenderAxisLabel(pSkp, &D3DXCOLOR(1,1,1,alpha), _V(0,1,0), ascale, scale, "Y");

			RenderAxisVector(pSkp, &D3DXCOLOR(1,1,1,alpha), _V(0,0,1), ascale, scale);
			RenderAxisLabel(pSkp, &D3DXCOLOR(1,1,1,alpha), _V(0,0,1), ascale, scale, "Z");

			if (scamode&SCA_NEGATIVE) {
				RenderAxisVector(pSkp, &D3DXCOLOR(1,1,1,alpha*0.5f), _V(-1,0,0), ascale, scale);
				RenderAxisLabel(pSkp, &D3DXCOLOR(1,1,1,alpha), _V(-1,0,0), ascale, scale, "-X");

				RenderAxisVector(pSkp, &D3DXCOLOR(1,1,1,alpha*0.5f), _V(0,-1,0), ascale, scale);
				RenderAxisLabel(pSkp, &D3DXCOLOR(1,1,1,alpha), _V(0,-1,0), ascale, scale, "-Y");

				RenderAxisVector(pSkp, &D3DXCOLOR(1,1,1,alpha*0.5f), _V(0,0,-1), ascale, scale);
				RenderAxisLabel(pSkp, &D3DXCOLOR(1,1,1,alpha), _V(0,0,-1), ascale, scale, "-Z");
			}
		}
	}
}


// ============================================================================================
//
bool vVessel::RenderExhaust()
{
	ExhaustLength = 0.0f;
	if (!active) return false;

	DWORD nexhaust = vessel->GetExhaustCount();
	if (!nexhaust) return true; // nothing to do

	EXHAUSTSPEC es;
	MATRIX3 R;
	vessel->GetRotationMatrix(R);
	VECTOR3 cdir = tmul(R, cpos);

	for (DWORD i=0;i<nexhaust;i++) {
		if (vessel->GetExhaustLevel(i)==0.0) continue;
		vessel->GetExhaustSpec(i, &es);
		D3D9Effect::RenderExhaust(&mWorld, cdir, &es, defexhausttex);
		if (es.lsize>ExhaustLength) ExhaustLength = float(es.lsize);
	}
	return true;
}


// ============================================================================================
//
void vVessel::RenderBeacons(LPDIRECT3DDEVICE9 dev)
{
	DWORD idx = 0;
	const BEACONLIGHTSPEC *bls = vessel->GetBeacon(idx);
	if (!bls) return; // nothing to do
	bool need_setup = true;
	double simt = oapiGetSimTime();

	for (;bls; bls = vessel->GetBeacon(++idx)) {
		if (bls->active) {
			if (bls->period && (fmod(simt+bls->tofs, bls->period) > bls->duration))	continue;
			double size = bls->size;
			if (cdist > 50.0) size *= pow (cdist/50.0, bls->falloff);
			RenderSpot(dev, bls->pos, (float)size, *bls->col, false, bls->shape);
		}
	}
}


// ============================================================================================
//
void vVessel::RenderGrapplePoints (LPDIRECT3DDEVICE9 dev)
{
	if (!oapiGetShowGrapplePoints()) return; // nothing to do

	DWORD i;
	ATTACHMENTHANDLE hAtt;
	VECTOR3 pos, dir, rot;
	const float size = 0.25;
	const float alpha = 0.5;

	// Flash calculations
	static double lastTime = 0;
	static bool isOn = true;
	double simt = oapiGetSysTime();
	if (simt-lastTime > 0.5) // Flashing period (twice per second)
	{
		isOn = !isOn;
		lastTime = simt;
	}
	if (!isOn) return; // nothing to do

	const OBJHANDLE hVessel = vessel->GetHandle();

	// attachment points to parent
	for (i = 0; i < vessel->AttachmentCount(true); ++i)
	{
		hAtt = vessel->GetAttachmentHandle(true, i);
		vessel->GetAttachmentParams(hAtt, pos, dir, rot);
		D3D9Effect::RenderArrow(hVessel, &pos, &dir, &rot, size, &D3DXCOLOR(1,0,0,alpha));
	}

	// attachment points to children
	for (i = 0; i < vessel->AttachmentCount(false); ++i)
	{
		hAtt = vessel->GetAttachmentHandle(false, i);
		vessel->GetAttachmentParams(hAtt, pos, dir, rot);
		D3D9Effect::RenderArrow(hVessel, &pos, &dir, &rot, size, &D3DXCOLOR(0,0.5,1,alpha));
	}
}


// ============================================================================================
//
void vVessel::RenderGroundShadow(LPDIRECT3DDEVICE9 dev, OBJHANDLE hPlanet, float alpha)
{
	if (!bStencilShadow) return;
	if (Config->TerrainShadowing == 0) return;

	static const double eps = 1e-2;
	static const double shadow_elev_limit = 0.07;
	double d, alt, R;
	VECTOR3 pp, sd, pvr;
	oapiGetGlobalPos (hPlanet, &pp); // planet global pos
	vessel->GetGlobalPos (sd);       // vessel global pos
	pvr = sd-pp;                     // planet-relative vessel position
	d = length(pvr);                 // vessel-planet distance
	R = oapiGetSize (hPlanet);       // planet mean radius
	R += vessel->GetSurfaceElevation();
	alt = d-R;                       // altitude above surface
	if (alt*eps > vessel->GetSize()) return; // too high to cast a shadow

	normalise(sd);                  // shadow projection direction

	// calculate the intersection of the vessel's shadow with the planet surface
	double fac1 = dotp (sd, pvr);
	if (fac1 > 0.0) return;          // shadow doesn't intersect planet surface
	double csun = -fac1/d;           // sun elevation above horizon
	if (csun < shadow_elev_limit) return;   // sun too low to cast shadow
	double arg  = fac1*fac1 - (dotp (pvr, pvr) - R*R);
	if (arg <= 0.0) return;                 // shadow doesn't intersect with planet surface
	double a = -fac1 - sqrt(arg);

	MATRIX3 vR;
	vessel->GetRotationMatrix (vR);
	VECTOR3 sdv = tmul (vR, sd);     // projection direction in vessel frame
	VECTOR3 shp = sdv*a;             // projection point
	VECTOR3 hn, hnp = vessel->GetSurfaceNormal();
	vessel->HorizonInvRot (hnp, hn);

	// perform projections
	double nr0 = dotp (hn, shp);
	double nd  = dotp (hn, sdv);
	VECTOR3 sdvs = sdv / nd;

	// build shadow projection matrix
	D3DXMATRIX mProj, mProjWorld, mProjWorldShift;

	mProj._11 = 1.0f - (float)(sdvs.x*hn.x);
	mProj._12 =      - (float)(sdvs.y*hn.x);
	mProj._13 =      - (float)(sdvs.z*hn.x);
	mProj._14 = 0;
	mProj._21 =      - (float)(sdvs.x*hn.y);
	mProj._22 = 1.0f - (float)(sdvs.y*hn.y);
	mProj._23 =      - (float)(sdvs.z*hn.y);
	mProj._24 = 0;
	mProj._31 =      - (float)(sdvs.x*hn.z);
	mProj._32 =      - (float)(sdvs.y*hn.z);
	mProj._33 = 1.0f - (float)(sdvs.z*hn.z);
	mProj._34 = 0;
	mProj._41 =        (float)(sdvs.x*nr0);
	mProj._42 =        (float)(sdvs.y*nr0);
	mProj._43 =        (float)(sdvs.z*nr0);
	mProj._44 = 1;

	D3DXMatrixMultiply(&mProjWorld, &mProj, &mWorld);

	bool isProjWorld = false;

	float scale = float( min(1, (csun-0.07)/0.015) );
	if (scale<1) alpha *= scale;

	// project all vessel meshes. This should be replaced by a dedicated shadow mesh

	for (UINT i=0;i<nmesh;i++) {

		if (meshlist[i].mesh==NULL) continue;
		if (!(meshlist[i].vismode & MESHVIS_EXTERNAL)) continue; // only render shadows for externally visible meshes
		if (meshlist[i].mesh->HasShadow()==false) continue;

		D3D9Mesh *mesh = meshlist[i].mesh;

		if (meshlist[i].trans) {
			D3DXMatrixMultiply(&mProjWorldShift, meshlist[i].trans, &mProjWorld);
			mesh->RenderShadows(alpha, &mProjWorldShift);
		}
		else mesh->RenderShadows(alpha, &mProjWorld);
	}
}

// ============================================================================================
// Return true if it's time to move to a next vessel
// false, if more rendereing is required here.
//
bool vVessel::RenderENVMap(LPDIRECT3DDEVICE9 pDev, DWORD cnt, DWORD flags)
{

	bool bReflective = false;

	if (meshlist) {
		for (DWORD i=0;i<nmesh;i++) {
			if (meshlist[i].mesh) {
				if (meshlist[i].mesh->IsReflective()) {
					bReflective = true;
					break;
				}
			}
		}
	}

	if (!bReflective) return true;

	LPDIRECT3DSURFACE9 pEnvDS = GetScene()->GetEnvDepthStencil();

	if (!pEnvDS) {
		LogErr("EnvDepthStencil doesn't exists");
		return true;
	}


	// Create a main EnvMap with mipmap chain for blurred maps --------------------------------------------------------------------
	//
	if (pEnv[ENVMAP_MAIN] == NULL) {
		D3DSURFACE_DESC desc;
		pEnvDS->GetDesc(&desc);
		if (D3DXCreateCubeTexture(pDev, desc.Width, 5, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &pEnv[ENVMAP_MAIN]) != S_OK) {
			LogErr("Failed to create env cubemap for visual 0x%X", this);
			return true;
		}
		nEnv = 1;
	}


	// Create blurred maps  -------------------------------------------------------------------------------
	//
	if (iFace >= 6) {
		iFace = 0;
		scn->RenderBlurredMap(pDev, pEnv[ENVMAP_MAIN]);
		return true;
	}

	double tot_env = D3D9GetTime();



	// Render EnvMaps ---------------------------------------------------------------------------------------
	//

	scn->ClearOmitFlags();

	ENVCAMREC *eCam = pMatMgr->GetCamera(0);

	// Omit the focus object
	if ((eCam->flags&ENVCAM_FOCUS)==0) bOmit = true;

	DWORD nAtc = vessel->AttachmentCount(false);
	DWORD nDoc = vessel->DockCount();

	if (eCam->flags & ENVCAM_OMIT_ATTC) {
		for (DWORD i=0;i<nAtc;i++) {
			ATTACHMENTHANDLE hAtc = vessel->GetAttachmentHandle(false, i);
			if (hAtc) {
				OBJHANDLE hAtcObj = vessel->GetAttachmentStatus(hAtc);
				if (hAtcObj) {
					vObject *vObj = gc->GetScene()->GetVisObject(hAtcObj);
					if (vObj) vObj->bOmit = true;
				}
			}
		}
	}
	else {

		DWORD nAttc = eCam->nAttc;

		for (DWORD i=0;i<nAttc;i++) {
			DWORD id = DWORD(eCam->pOmitAttc[i]);
			ATTACHMENTHANDLE hAtc = vessel->GetAttachmentHandle(false, id);
			if (hAtc) {
				OBJHANDLE hAtcObj = vessel->GetAttachmentStatus(hAtc);
				if (hAtcObj) {
					vObject *vObj = gc->GetScene()->GetVisObject(hAtcObj);
					if (vObj) vObj->bOmit = true;
				}
			}
		}
	}

	
	// -----------------------------------------------------------------------------------------------
	//
	VECTOR3 gpos;
	vessel->Local2Global(_V(eCam->lPos.x, eCam->lPos.y, eCam->lPos.z), gpos);

	// Prepare camera and scene for env map rendering
	scn->PushCamera();
	scn->SetupInternalCamera(NULL, &gpos, 0.7853981634, 1.0);
	scn->BeginPass(RENDERPASS_ENVCAM);

	gc->PushRenderTarget(NULL, pEnvDS);

	D3DXMATRIX mEnv;
	D3DXVECTOR3 dir, up;
	LPDIRECT3DSURFACE9 pSrf = NULL;

	
	for (DWORD i=0;i<cnt;i++) {

		assert(SUCCEEDED(pEnv[0]->GetCubeMapSurface(D3DCUBEMAP_FACES(iFace), 0, &pSrf)));

		gc->AlterRenderTarget(pSrf, pEnvDS);

		EnvMapDirection(iFace, &dir, &up);

		D3DXVECTOR3 cp;
		D3DXVec3Cross(&cp, &up, &dir);
		D3DXVec3Normalize(&cp, &cp);
		D3DXMatrixIdentity(&mEnv);
		D3DMAT_FromAxis(&mEnv, &cp, &up, &dir);

		scn->SetupInternalCamera(&mEnv, NULL, 0.7853981634, 1.0);
		scn->RenderSecondaryScene(this, true, flags);
			
		SAFE_RELEASE(pSrf);
		
		iFace++;
		if (iFace >= 6) break;
	}

	gc->PopRenderTargets();

	scn->PopPass();
	scn->PopCamera();

	return false;
}


// ============================================================================================
//
LPDIRECT3DCUBETEXTURE9 vVessel::GetEnvMap(int idx)
{
	if (idx>=0 && idx<4) return pEnv[idx];
	return NULL;
}


// ============================================================================================
//
bool vVessel::ModLighting(D3D9Sun *light)
{
	VECTOR3 GV;

	// we only test the closest celestial body for shadowing
	OBJHANDLE hP = vessel->GetSurfaceRef();

	if (hP==NULL) {
		LogErr("Vessel's surface reference is NULL");
		return false;
	}

	vessel->GetGlobalPos(GV);

	tCheckLight = oapiGetSimTime();

	DWORD dAmbient = *(DWORD*)gc->GetConfigParam(CFGPRM_AMBIENTLEVEL);
	float fAmbient = float(dAmbient)*0.0039f;

	if (DebugControls::IsActive()) {
		if (*(DWORD*)gc->GetConfigParam(CFGPRM_GETDEBUGFLAGS)&DBG_FLAGS_AMBIENT) fAmbient = 0.25f;
	}

	vPlanet *vP = (vPlanet *)GetScene()->GetVisObject(hP);

	if (vP) OrbitalLighting(light, vP, GV, fAmbient);

	return true;
}



// ============================================================================================
//
void vVessel::Animate(UINT an, double state, UINT mshidx)
{
	double s0, s1, ds;
	UINT i, ii;
	D3DXMATRIX T;
	ANIMATION *A = anim+an;

	for (ii = 0; ii < A->ncomp; ii++) {
		i = (state > animstate[an] ? ii : A->ncomp-ii-1);
		ANIMATIONCOMP *AC = A->comp[i];

		// What is this ??? mshidx is always -1
 		if (mshidx != ((UINT)-1) && mshidx != AC->trans->mesh) continue;

		s0 = animstate[an]; // current animation state in the visual
		if      (s0 < AC->state0) s0 = AC->state0;
		else if (s0 > AC->state1) s0 = AC->state1;
		s1 = state;           // required animation state
		if      (s1 < AC->state0) s1 = AC->state0;
		else if (s1 > AC->state1) s1 = AC->state1;
		if ((ds = (s1-s0)) == 0) continue; // nothing to do for this component
		ds /= (AC->state1 - AC->state0);   // stretch to range 0..1

		// Build transformation matrix
		switch (AC->trans->Type())
		{
			case MGROUP_TRANSFORM::NULLTRANSFORM:
			{
				D3DMAT_Identity (&T);
				AnimateComponent (AC, T);
			}	break;

			case MGROUP_TRANSFORM::ROTATE:
			{
				MGROUP_ROTATE *rot = (MGROUP_ROTATE*)AC->trans;
				D3DXVECTOR3 ax(float(rot->axis.x), float(rot->axis.y), float(rot->axis.z));
				D3DMAT_RotationFromAxis (ax, (float)ds*rot->angle, &T);
				float dx = D3DVAL(rot->ref.x), dy = D3DVAL(rot->ref.y), dz = D3DVAL(rot->ref.z);
				T._41 = dx - T._11*dx - T._21*dy - T._31*dz;
				T._42 = dy - T._12*dx - T._22*dy - T._32*dz;
				T._43 = dz - T._13*dx - T._23*dy - T._33*dz;
				AnimateComponent (AC, T);
			} break;

			case MGROUP_TRANSFORM::TRANSLATE:
			{
				MGROUP_TRANSLATE *lin = (MGROUP_TRANSLATE*)AC->trans;
				D3DMAT_Identity (&T);
				T._41 = (float)(ds*lin->shift.x);
				T._42 = (float)(ds*lin->shift.y);
				T._43 = (float)(ds*lin->shift.z);
				AnimateComponent (AC, T);
			} break;

			case MGROUP_TRANSFORM::SCALE:
			{
				MGROUP_SCALE *scl = (MGROUP_SCALE*)AC->trans;
				s0 = (s0-AC->state0)/(AC->state1-AC->state0);
				s1 = (s1-AC->state0)/(AC->state1-AC->state0);
				D3DMAT_Identity (&T);
				T._11 = (float)((s1*(scl->scale.x-1)+1)/(s0*(scl->scale.x-1)+1));
				T._22 = (float)((s1*(scl->scale.y-1)+1)/(s0*(scl->scale.y-1)+1));
				T._33 = (float)((s1*(scl->scale.z-1)+1)/(s0*(scl->scale.z-1)+1));
				T._41 = (float)scl->ref.x * (1.0f-T._11);
				T._42 = (float)scl->ref.y * (1.0f-T._22);
				T._43 = (float)scl->ref.z * (1.0f-T._33);
				AnimateComponent (AC, T);
			} break;
		}
	}
}


// ============================================================================================
//
void vVessel::AnimateComponent (ANIMATIONCOMP *comp, const D3DXMATRIX &T)
{
	UINT i;

	bBSRecompute = true;

	MGROUP_TRANSFORM *trans = comp->trans;

	if (trans->mesh == LOCALVERTEXLIST) { // transform a list of individual vertices
		VECTOR3 *vtx = (VECTOR3*)trans->grp;
		for (i = 0; i < trans->ngrp; i++) TransformPoint(vtx[i], T);
	}
	else { // transform mesh groups

		if (trans->mesh >= nmesh) return; // mesh index out of range
		D3D9Mesh *mesh = meshlist[trans->mesh].mesh;
		if (!mesh) return;

		if (trans->grp) { // animate individual mesh groups
			for (i=0;i<trans->ngrp;i++) mesh->TransformGroup(trans->grp[i], &T);
		}
		else {          // animate complete mesh
			mesh->Transform(&T);
		}
	}

	// recursively transform all child animations
	for (i = 0; i < comp->nchildren; i++) {

		ANIMATIONCOMP *child = comp->children[i];
		AnimateComponent (child, T);

		switch (child->trans->Type()) {

			case MGROUP_TRANSFORM::NULLTRANSFORM:
				break;

			case MGROUP_TRANSFORM::ROTATE: {
				MGROUP_ROTATE *rot = (MGROUP_ROTATE*)child->trans;
				TransformPoint (rot->ref, T);
				TransformDirection (rot->axis, T, true);
			} break;

			case MGROUP_TRANSFORM::TRANSLATE: {
				MGROUP_TRANSLATE *lin = (MGROUP_TRANSLATE*)child->trans;
				TransformDirection (lin->shift, T, false);
			} break;

			case MGROUP_TRANSFORM::SCALE: {
				MGROUP_SCALE *scl = (MGROUP_SCALE*)child->trans;
				TransformPoint (scl->ref, T);
				// we can't transform anisotropic scaling vector
			} break;
		}
	}
}


// ============================================================================================
//

void vVessel::RenderReentry(LPDIRECT3DDEVICE9 dev)
{

	if (defreentrytex==NULL) return;

	double p = vessel->GetAtmDensity();
	double v = vessel->GetAirspeed();

	float lim  = 100000000.0;
	float www  = float(p*v*v*v);
	float ints = max(0,(www-lim)) / (5.0f*lim);

	if (ints>1.0f) ints = 1.0f;
	if (ints<0.01f) return;

	VECTOR3 d;
	vessel->GetShipAirspeedVector(d);
	vessel->GlobalRot(d, d);
	normalise(d);

	float x = float(dotp(d, unit(cpos)));
	if (x<0) x=-x;	x=pow(x,0.3f);

	float alpha_B = (x*0.40f + 0.60f) * ints;
	float alpha_A = (1.0f - x*0.50f) * ints * 1.2f;

	float size = float(vessel->GetSize()) * 1.7f;

	D3DXVECTOR3 vPosA(float(cpos.x), float(cpos.y), float(cpos.z));
	D3DXVECTOR3 vDir(float(d.x), float(d.y), float(d.z));
	D3DXVECTOR3 vPosB = vPosA + vDir * (size*0.25f);

	D3D9Effect::RenderReEntry(defreentrytex, &vPosA, &vPosB, &vDir, alpha_A, alpha_B, size);
}


// ===========================================================================================
//
bool vVessel::GetMinMaxDistance(float *zmin, float *zmax, float *dmin)
{
	if (bBSRecompute) UpdateBoundingBox();

	D3DXMATRIX mWorldView, mWorldViewTrans, mTF;

	WORD vismode = MESHVIS_EXTERNAL | MESHVIS_EXTPASS;

	Scene *scn = gc->GetScene();

	D3DXVECTOR4 Field = D9LinearFieldOfView(scn->GetProjectionMatrix());

	D3DXMatrixMultiply(&mWorldView, &mWorld, scn->GetViewMatrix());

	for (DWORD i=0;i<nmesh;i++) {
		if (meshlist[i].vismode&vismode && meshlist[i].mesh) {
			if (meshlist[i].trans) {
				D3DXMatrixMultiply(&mWorldViewTrans, (const D3DXMATRIX *)meshlist[i].trans, &mWorldView);
				D9ComputeMinMaxDistance(gc->GetDevice(), meshlist[i].mesh->GetAABB(), &mWorldViewTrans, &Field, zmin, zmax, dmin);
			}
			else {
				D9ComputeMinMaxDistance(gc->GetDevice(), meshlist[i].mesh->GetAABB(), &mWorldView, &Field, zmin, zmax, dmin);
			}
		}
	}

	return true;
}

// ===========================================================================================
//
void vVessel::UpdateBoundingBox()
{
	if (nmesh==0) return;
	if (bBSRecompute==false) return;

	bBSRecompute = false;
	bool bFirst = true;

	WORD vismode = MESHVIS_EXTERNAL | MESHVIS_EXTPASS;

	D3DXMATRIX mTF;

	for (DWORD i=0;i<nmesh;i++) {

		D3DXVECTOR3 q,w;
		if (meshlist[i].mesh==NULL) continue;

		if (meshlist[i].vismode&vismode) {

			LPD3DXMATRIX pMeshTF = meshlist[i].mesh->GetTransform();
			LPD3DXMATRIX pTF = &mTF;

			if (meshlist[i].trans) {
				if (pMeshTF) D3DXMatrixMultiply(pTF, meshlist[i].trans, pMeshTF);
				else         pTF = meshlist[i].trans;
			} else {
				if (pMeshTF) pTF = pMeshTF;
				else         pTF = NULL;
			}

			D9AddAABB(meshlist[i].mesh->GetAABB(), pTF, &BBox, bFirst);
			bFirst = false;
		}
		else {
			meshlist[i].mesh->UpdateBoundingBox();
		}
	}

	DWORD nexhaust = vessel->GetExhaustCount();

	if (nexhaust) {
		EXHAUSTSPEC es;
		for (DWORD i=0;i<nexhaust;i++) {
			double lvl = vessel->GetExhaustLevel(i);
			if (lvl==0.0) continue;
			vessel->GetExhaustSpec(i, &es);
			VECTOR3 e = (*es.lpos) - (*es.ldir) * (es.lofs + es.lsize*lvl);
			D3DXVECTOR3 ext(float(e.x), float(e.y), float(e.z));
			D9AddPointAABB(&BBox, &ext);
		}

		for (DWORD i=0;i<nexhaust;i++) {
			vessel->GetExhaustSpec(i, &es);
			VECTOR3 r = (*es.lpos);
			D3DXVECTOR3 ref(float(r.x), float(r.y), float(r.z));
			D9AddPointAABB(&BBox, &ref);
		}
	}

	D9UpdateAABB(&BBox);
}

// ===========================================================================================
//
D3D9Pick vVessel::Pick(const D3DXVECTOR3 *vDir)
{
	D3DXMATRIX mWT;
	LPD3DXMATRIX pWT = NULL;

	DWORD flags = *(DWORD*)gc->GetConfigParam(CFGPRM_GETDEBUGFLAGS);
	DWORD displ = *(DWORD*)gc->GetConfigParam(CFGPRM_GETDISPLAYMODE);

	bool bCockpit = (oapiCameraInternal() && (hObj == oapiGetFocusObject()));
	bool bVC = (bCockpit && (oapiCockpitMode() == COCKPIT_VIRTUAL));

	D3D9Pick result;
	result.dist  = 1e30f;
	result.pMesh = NULL;
	result.vObj  = NULL;
	result.face  = -1;
	result.group = -1;

	//if (!meshlist || nmesh==0 || this!=DebugControls::GetVisual()) return result;
	if (!meshlist || nmesh==0) return result;

	for (DWORD i=0;i<nmesh;i++) {

		D3D9Mesh *hMesh = meshlist[i].mesh;

		if (!hMesh) continue;

		// check if mesh should be rendered in this pass
		WORD vismode = meshlist[i].vismode;

		if (DebugControls::IsActive()) if (displ>1) vismode = MESHVIS_ALWAYS;

		if (vismode==0) continue;

		if (bCockpit) {
			if (!(vismode & MESHVIS_COCKPIT)) {
				if ((!bVC) || (!(vismode & MESHVIS_VC))) continue;
			}
		}
		else {
			if (!(vismode & MESHVIS_EXTERNAL)) continue;
		}

		if (meshlist[i].trans) pWT = D3DXMatrixMultiply(&mWT, (const D3DXMATRIX *)meshlist[i].trans, &mWorld);
		else pWT = &mWorld;

		D3D9Pick pick = hMesh->Pick(pWT, vDir);
		if (pick.pMesh) if (pick.dist<result.dist) result = pick;
	}

	if (result.pMesh) result.vObj = this;

	return result;
}

// ===========================================================================================
//

D3D9ClientSurface * vVessel::tHUD = 0;
D3D9ClientSurface * vVessel::defreentrytex = 0;
D3D9ClientSurface * vVessel::defexhausttex = 0;


// ==============================================================
// Nonmember helper functions

void TransformPoint (VECTOR3 &p, const D3DXMATRIX &T)
{
	double x = p.x*T._11 + p.y*T._21 + p.z*T._31 + T._41;
	double y = p.x*T._12 + p.y*T._22 + p.z*T._32 + T._42;
	double z = p.x*T._13 + p.y*T._23 + p.z*T._33 + T._43;
	double w = 1.0/(p.x*T._14 + p.y*T._24 + p.z*T._34 + T._44);
	p.x = x*w;
	p.y = y*w;
	p.z = z*w;
}

// ===============================================================
//
void TransformDirection (VECTOR3 &a, const D3DXMATRIX &T, bool normalise)
{
	double x = a.x*T._11 + a.y*T._21 + a.z*T._31;
	double y = a.x*T._12 + a.y*T._22 + a.z*T._32;
	double z = a.x*T._13 + a.y*T._23 + a.z*T._33;
	a.x = x, a.y = y, a.z = z;
	if (normalise) {
		double len = 1.0/sqrt (x*x + y*y + z*z);
		a.x *= len;
		a.y *= len;
		a.z *= len;
	}
}

// ===========================================================================
// Add ISO-prefix to a value
//
#define ARRAY_ELEMS(a) (sizeof(a) / sizeof((a)[0]))

static inline const int clip(int v, int vMin, int vMax)
{
	if      (v < vMin) return vMin;
	else if (v > vMax) return vMax;
	else               return v;
}

inline const char *value_string (char *buf, size_t buf_size, double val)
{
	static const char unit_prefixes[] = { '�', 'm', '\0', 'k' , 'M' , 'G' , 'T' , 'P' };

	int index = (int) (log10(val)) / 3;
	val /= pow(10.0, index*3);

	// apply array index offset (+2) [-2...5] => [0...7]
	index = clip(index+2, 0, ARRAY_ELEMS(unit_prefixes) -1);
	sprintf_s(buf, buf_size, "%.3f %c", val, unit_prefixes[index]);

	return buf;
}

const char *value_string (double val)
{
	static char buf[16];
	return value_string(buf, sizeof(buf), val);
}