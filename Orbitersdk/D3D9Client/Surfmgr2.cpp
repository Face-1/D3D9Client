// ==============================================================
//   ORBITER VISUALISATION PROJECT (OVP)
//   Copyright (C) 2006-2014 Martin Schweiger
//   Dual licensed under GPL v3 and LGPL v3
// ==============================================================

// ==============================================================
// surfmgr2.cpp
// class SurfaceManager2 (implementation)
//
// Planetary surface rendering engine v2, including a simple
// LOD (level-of-detail) algorithm for surface patch resolution.
// ==============================================================

#include "Surfmgr2.h"
#include "Texture.h"

// =======================================================================

#pragma pack(push,1)

struct ELEVFILEHEADER { // file header for patch elevation data file
	char id[4];            // ID string + version ('E','L','E',1)
	int hdrsize;           // header size (76 expected)
	int dtype;             // data format (0=flat, no data block; 8=uint8; -8=int8; 16=uint16; -16=int16)
	int xgrd,ygrd;         // data grid size (259 x 259 expected)
	int xpad,ypad;         // horizontal, vertical padding width (1, 1 expected)
	double scale;          // data scaling factor (1.0 expected)
	double offset;         // data offset (elevation = raw value * scale + offset)
	double latmin, latmax; // latitude range [rad]
	double lngmin, lngmax; // longitude range [rad]
	double emin, emax, emean; // min, max, mean elevation [m]
};

#pragma pack(pop)

// =======================================================================
// Utility functions

static void VtxInterpolate (VERTEX_2TEX &res, const VERTEX_2TEX &a, const VERTEX_2TEX &b, double w)
{
	float w1 = (float)w;
	float w0 = 1.0f-w1;
	res.x = a.x*w0 + b.x*w1;
	res.y = a.y*w0 + b.y*w1;
	res.z = a.z*w0 + b.z*w1;
	res.nx = a.nx*w0 + b.nx*w1;
	res.ny = a.ny*w0 + b.ny*w1;
	res.nz = a.nz*w0 + b.nz*w1;
}

// =======================================================================
// =======================================================================

SurfTile::SurfTile (TileManager2Base *_mgr, int _lvl, int _ilat, int _ilng)
: Tile (_mgr, _lvl, _ilat, _ilng)
{
	smgr = static_cast<TileManager2<SurfTile>* > (_mgr);
	node = 0;
	elev = NULL;
	// ggelev = NULL;
	ltex = 0;
}

// -----------------------------------------------------------------------

SurfTile::~SurfTile ()
{
	// ggelev = NULL;
	if (elev) {
		delete []elev;
		elev = NULL;
	}
	if (ltex && owntex) {
		ltex->Release();
	}
}

// -----------------------------------------------------------------------

void SurfTile::Load ()
{
	bool bLoadMip = true; // for now

	DWORD Mips = (bLoadMip ? 1:3); // Load main level only or 2 additional mip sub-level
	char path[MAX_PATH] = {'\0'};

	LPDIRECT3DDEVICE9 pDev = mgr->Dev();

	// Load surface texture
	sprintf_s (path, "Textures\\%s\\Surf\\%02d\\%06d\\%06d.dds", mgr->CbodyName(), lvl+4, ilat, ilng);
	
	owntex = true;
	if (D3DXCreateTextureFromFileExA(pDev, path, 0, 0, Mips, 0, D3DFMT_FROM_FILE, D3DPOOL_DEFAULT, D3DX_DEFAULT, D3DX_DEFAULT, 0, NULL, NULL, &tex) != S_OK) {
		if (GetParentSubTexRange (&texrange)) {
			tex = getSurfParent()->Tex();
			owntex = false;
		} else
			tex = 0;
	}
	
	// Load mask texture
	ltex = NULL;
	if (mgr->Cprm().bSpecular || mgr->Cprm().bLights) {
		if (owntex) {
			sprintf_s (path, "Textures\\%s\\Mask\\%02d\\%06d\\%06d.dds", mgr->CbodyName(), lvl+4, ilat, ilng);
			D3DXCreateTextureFromFileExA(pDev, path, 0, 0, Mips, 0, D3DFMT_FROM_FILE, D3DPOOL_DEFAULT, D3DX_DEFAULT, D3DX_DEFAULT, 0, NULL, NULL, &ltex);
		} else if (node->Parent()) {
			ltex = getSurfParent()->ltex;
		}
	}

	// Load elevation data
	INT16 *elev = ElevationData ();
	bool shift_origin = (lvl >= 4);

	if (!lvl) {
		// create hemisphere mesh for western or eastern hemispheres
		mesh = CreateMesh_hemisphere (TILE_PATCHRES, elev, 0.0);
	//} else if (ilat == 0 || ilat == (1<<lvl)-1) {
		// create triangular patch for north/south pole region
	//	mesh = CreateMesh_tripatch (patch_res, elev, shift_origin, &vtxshift);
	} else {
		// create rectangular patch
		mesh = CreateMesh_quadpatch (TILE_PATCHRES, TILE_PATCHRES, elev, 0.0, &texrange, shift_origin, &vtxshift);
	}

}

// -----------------------------------------------------------------------

bool SurfTile::LoadElevationData ()
{
	// Note: a tile's elevation data are retrieved from its great-grandparent tile. Each tile stores the elevation
	// data for its area at 8x higher resolution than required by itself, so they can be used by the grandchildren.
	// This reduces the number of elevation files, by storing more information in each. It also has a caching effect:
	// Once a grandparent has loaded its elevation data on request of a grandchild, any other grandchildren's requests
	// can be served directly without further disk I/O.

	if (elev) return true; // already present
	int mode = mgr->Cprm().elevMode;
	if (!mode) return false;

	int ndat = TILE_ELEVSTRIDE*TILE_ELEVSTRIDE;

	int i, j;
	char path[256];
	sprintf_s (path, "Textures\\%s\\Elev\\%02d\\%06d\\%06d.elv", mgr->CbodyName(), lvl+4, ilat, ilng);
	FILE *f = NULL;
	fopen_s(&f, path, "rb");
	if (f) {
		elev = new INT16[ndat];
		// read the elevation file header
		ELEVFILEHEADER hdr;
		fread (&hdr, sizeof(ELEVFILEHEADER), 1, f);
		if (hdr.hdrsize != sizeof(ELEVFILEHEADER)) {
			fseek (f, hdr.hdrsize, SEEK_SET);
		}
		mean_elev = hdr.emean;
		// read the elevation data
		switch (hdr.dtype) {
		case 0: // flat tile, defined by offset
			for (i = 0; i < ndat; i++) elev[i] = (INT16)hdr.offset;
			break;
		case 8: {
			UINT8 *tmp = new UINT8[ndat];
			fread (tmp, sizeof(UINT8), ndat, f);
			for (i = 0; i < ndat; i++) elev[i] = (INT16)(tmp[i]+hdr.offset);
			delete []tmp;
			}
			break;
		case -16:
			fread (elev, sizeof(INT16), ndat, f);
			if (hdr.offset)
				for (i = 0; i < ndat; i++) elev[i] = (INT16)(elev[i]+hdr.offset);
			break;
		}

		bool elev_exaggerate = false; // for now
		double elev_exaggerate_factor = 3.0; // for now
		if (elev_exaggerate)
			for (i = 0; i < ndat; i++)
				elev[i] = (INT16)(elev[i] * elev_exaggerate_factor);

		fclose(f);
	} else {
		// try fetching elevation data recursively from parent and interpolate them
		if (node->Parent() && node->Parent()->Entry()->LoadElevationData ()) {
			int lat_ofs = (ilat & 1 ? 0 : TILE_PATCHRES*4);
			int lng_ofs = (ilng & 1 ? TILE_PATCHRES*4 : 0);
			elev = new INT16[ndat];
			mean_elev = 0.0;
			INT16 *celev = elev + TILE_ELEVSTRIDE+1;   // pointer to beginning of unpadded block
			INT16 *pelev = node->Parent()->Entry()->elev + lat_ofs*TILE_ELEVSTRIDE + lng_ofs + TILE_ELEVSTRIDE+1;

			for (i = 0; i <= TILE_PATCHRES*4; i++) {
				for (j = 0; j <= TILE_PATCHRES*4; j++) {
					mean_elev += (celev[i*2*TILE_ELEVSTRIDE + j*2] = pelev[i*TILE_ELEVSTRIDE + j]);
				}
			}
			if (mode == 1) { // linear
				for (i = 0; i <= TILE_PATCHRES*4; i++) {
					for (j = -1; j <= TILE_PATCHRES*4; j++) {
						celev[i*2*TILE_ELEVSTRIDE + j*2+1] = (pelev[i*TILE_ELEVSTRIDE+j] + pelev[i*TILE_ELEVSTRIDE+j+1]) / 2;
					}
				}
				for (i = -1; i <= TILE_PATCHRES*4; i++) {
					for (j = 0; j <= TILE_PATCHRES*4; j++) {
						celev[(i*2+1)*TILE_ELEVSTRIDE + j*2] = (pelev[i*TILE_ELEVSTRIDE+j] + pelev[(i+1)*TILE_ELEVSTRIDE+j]) / 2;
					}
				}
				for (i = -1; i <= TILE_PATCHRES*4; i++) {
					for (j = -1; j <= TILE_PATCHRES*4; j++) {
						celev[(i*2+1)*TILE_ELEVSTRIDE + j*2+1] = (pelev[i*TILE_ELEVSTRIDE+j] + pelev[i*TILE_ELEVSTRIDE+j+1] + pelev[(i+1)*TILE_ELEVSTRIDE+j] + pelev[(i+1)*TILE_ELEVSTRIDE+j+1]) / 4;
					}
				}
			} else { // cubic spline interpolation
				double a_m1, a_0, a_p1, a_p2, b_m1, b_0, b_p1, b_p2, res;
				for (i = 0; i <= TILE_PATCHRES*4; i++) {
					for (j = 0; j < TILE_PATCHRES*4; j++) {
						a_m1 = pelev[i*TILE_ELEVSTRIDE+j-1];
						a_0  = pelev[i*TILE_ELEVSTRIDE+j];
						a_p1 = pelev[i*TILE_ELEVSTRIDE+j+1];
						a_p2 = pelev[i*TILE_ELEVSTRIDE+j+2];
						res = 0.5 * (2.0*a_0 + 0.5*(-a_m1+a_p1) + 0.25*(2.0*a_m1-5.0*a_0+4.0*a_p1-a_p2) + 0.125*(-a_m1+3.0*a_0-3.0*a_p1+a_p2));
						celev[i*2*TILE_ELEVSTRIDE + j*2+1] = (INT16)(res+0.5);
					}
				}
				for (i = 0; i < TILE_PATCHRES*4; i++) {
					for (j = 0; j <= TILE_PATCHRES*4; j++) {
						a_m1 = pelev[(i-1)*TILE_ELEVSTRIDE+j];
						a_0  = pelev[i*TILE_ELEVSTRIDE+j];
						a_p1 = pelev[(i+1)*TILE_ELEVSTRIDE+j];
						a_p2 = pelev[(i+2)*TILE_ELEVSTRIDE+j];
						res = 0.5 * (2.0*a_0 + 0.5*(-a_m1+a_p1) + 0.25*(2.0*a_m1-5.0*a_0+4.0*a_p1-a_p2) + 0.125*(-a_m1+3.0*a_0-3.0*a_p1+a_p2));
						celev[(i*2+1)*TILE_ELEVSTRIDE + j*2] = (INT16)(res+0.5);
					}
				}
				for (i = 0; i < TILE_PATCHRES*4; i++) {
					for (j = 0; j < TILE_PATCHRES*4; j++) {
						if (i) b_m1 = celev[(i*2-2)*TILE_ELEVSTRIDE + j*2+1];
						else {
							a_m1 = pelev[(i-1)*TILE_ELEVSTRIDE+j-1];
							a_0  = pelev[(i-1)*TILE_ELEVSTRIDE+j];
							a_p1 = pelev[(i-1)*TILE_ELEVSTRIDE+j+1];
							a_p2 = pelev[(i-1)*TILE_ELEVSTRIDE+j+2];
							b_m1 = 0.5 * (2.0*a_0 + 0.5*(-a_m1+a_p1) + 0.25*(2.0*a_m1-5.0*a_0+4.0*a_p1-a_p2) + 0.125*(-a_m1+3.0*a_0-3.0*a_p1+a_p2));
						}
						b_0  = celev[(i*2)*TILE_ELEVSTRIDE + j*2+1];
						b_p1 = celev[(i*2+2)*TILE_ELEVSTRIDE + j*2+1];
						if (i < TILE_PATCHRES*4-1) b_p2 = celev[(i*2+4)*TILE_ELEVSTRIDE + j*2+1];
						else {
							a_m1 = pelev[(i+2)*TILE_ELEVSTRIDE+j-1];
							a_0  = pelev[(i+2)*TILE_ELEVSTRIDE+j];
							a_p1 = pelev[(i+2)*TILE_ELEVSTRIDE+j+1];
							a_p2 = pelev[(i+2)*TILE_ELEVSTRIDE+j+2];
							b_p2 = 0.5 * (2.0*a_0 + 0.5*(-a_m1+a_p1) + 0.25*(2.0*a_m1-5.0*a_0+4.0*a_p1-a_p2) + 0.125*(-a_m1+3.0*a_0-3.0*a_p1+a_p2));
						}
						res = 0.5 * (2.0*b_0 + 0.5*(-b_m1+b_p1) + 0.25*(2.0*b_m1-5.0*b_0+4.0*b_p1-b_p2) + 0.125*(-b_m1+3.0*b_0-3.0*b_p1+b_p2));
						celev[(i*2+1)*TILE_ELEVSTRIDE + j*2+1] = (INT16)(res+0.5);
					}
				}
				// do linear interpolation for border nodes
				for (i = 0; i <= TILE_PATCHRES*4; i++) {
					celev[(i*2)*TILE_ELEVSTRIDE-1] = (pelev[i*TILE_ELEVSTRIDE-1]+pelev[i*TILE_ELEVSTRIDE])/2;
					celev[(i*2)*TILE_ELEVSTRIDE+TILE_PATCHRES*8+1] = (pelev[i*TILE_ELEVSTRIDE+TILE_PATCHRES*4]+pelev[i*TILE_ELEVSTRIDE+TILE_PATCHRES*4+1]) /2;
				}
				for (j = 0; j <= TILE_PATCHRES*4; j++) {
					celev[-TILE_ELEVSTRIDE+j*2] = (pelev[-TILE_ELEVSTRIDE+j]+pelev[j])/2;
					celev[(TILE_PATCHRES*8+1)*TILE_ELEVSTRIDE+j*2] = (pelev[(TILE_PATCHRES*4)*TILE_ELEVSTRIDE+j]+pelev[(TILE_PATCHRES*4+1)*TILE_ELEVSTRIDE+j])/2;
				}
				for (i = -1; i <= TILE_PATCHRES*4; i++) {
					celev[(i*2+1)*TILE_ELEVSTRIDE-1] = (pelev[i*TILE_ELEVSTRIDE-1] + pelev[i*TILE_ELEVSTRIDE] + pelev[(i+1)*TILE_ELEVSTRIDE-1] + pelev[(i+1)*TILE_ELEVSTRIDE])/4;
					celev[(i*2+1)*TILE_ELEVSTRIDE+TILE_PATCHRES*8+1] = (pelev[i*TILE_ELEVSTRIDE+TILE_PATCHRES*4] + pelev[i*TILE_ELEVSTRIDE+TILE_PATCHRES*4+1] + pelev[(i+1)*TILE_ELEVSTRIDE+TILE_PATCHRES*4] + pelev[(i+1)*TILE_ELEVSTRIDE+TILE_PATCHRES*4+1])/4;
				}
				for (j = 0; j < TILE_PATCHRES*4; j++) {
					celev[-TILE_ELEVSTRIDE+j*2+1] = (pelev[-TILE_ELEVSTRIDE+j] + pelev[-TILE_ELEVSTRIDE+j+1] + pelev[j] + pelev[j+1])/4;
					celev[(TILE_PATCHRES*8+1)*TILE_ELEVSTRIDE+j*2+1] = (pelev[(TILE_PATCHRES*4)*TILE_ELEVSTRIDE+j] + pelev[(TILE_PATCHRES*4)*TILE_ELEVSTRIDE+j+1] + pelev[(TILE_PATCHRES*4+1)*TILE_ELEVSTRIDE+j] + pelev[(TILE_PATCHRES*4+1)*TILE_ELEVSTRIDE+j+1])/4;
				}
			}
			mean_elev /= ((TILE_PATCHRES*4+1)*(TILE_PATCHRES*4+1));
		} else {
			elev = NULL;
		}
	}
	return (elev != NULL);
}

// -----------------------------------------------------------------------

INT16 *SurfTile::ElevationData () const
{
	QuadTreeNode<SurfTile> *ggparent;
	if ( node->Parent() &&
	     node->Parent()->Parent() &&
	    (ggparent = node->Parent()->Parent()->Parent()) && ggparent->Entry()->LoadElevationData() )
	{
		// compute pixel offset into great-grandparent tile set
		int ofs = ((7 - ilat & 7) * TILE_ELEVSTRIDE + (ilng & 7)) * TILE_PATCHRES;
		mean_elev = ggparent->Entry()->mean_elev; // not quite true, since the ancestor covers a larger area
		return ggparent->Entry()->elev + ofs;
	}
	return NULL;

	/*
	 * The SurfTile::ggelev member will point to already disposed memory!
	 * Until there's a kind of 'ClearGrandGrandParent()' method, this is dangerous!
	 *
	if (!ggelev) {
		QuadTreeNode<SurfTile> *ggparent;
		if (node->Parent() && node->Parent()->Parent() && (ggparent = node->Parent()->Parent()->Parent())) {
			if (ggparent->Entry()->LoadElevationData ()) {
				// compute pixel offset into great-grandparent tile set
				int ofs = ((7 - ilat & 7) * TILE_ELEVSTRIDE + (ilng & 7)) * TILE_PATCHRES;
				mean_elev = ggparent->Entry()->mean_elev; // not quite true, since the ancestor covers a larger area
				ggelev = ggparent->Entry()->elev + ofs;
			}
		}
	}
	return ggelev;
	*/
}

// -----------------------------------------------------------------------

void SurfTile::Render ()
{
	UINT numPasses = 0;

	LPDIRECT3DDEVICE9 pDev = mgr->Dev();
	ID3DXEffect *Shader = mgr->Shader();

	bool render_cloud_shadows = false;
	bool render_lights = mgr->Cprm().bLights;
	bool has_specular = false;
	double sdist = 0.0;

	if (!mesh) return; // DEBUG : TEMPORARY

	if (ltex) {
		sdist = acos (dotp (mgr->prm.sdir, cnt));
		static const double rad0 = sqrt(2.0)*PI05;
		double rad = rad0/(double)(2<<lvl); // tile radius

		has_specular = (sdist < PI05+rad);
		render_cloud_shadows = (sdist < PI05+rad && !mgr->prm.rprm->bCloudFlatShadows /*&& mgr->prm.cloudmgr*/);
		render_lights = (render_lights && sdist > 1.45);
	}

	// ---------------------------------------------------------------------
	// Feed tile specific data to shaders
	//
	// ---------------------------------------------------------------------------------------------------
	HR(Shader->SetVector(TileManager2Base::svTexOff, &D3DXVECTOR4(1.0f, 0.0f, 1.0f, 0.0f)));
	// ---------------------------------------------------------------------------------------------------
	HR(Shader->SetTexture(TileManager2Base::stDiff, tex));
	HR(Shader->SetTexture(TileManager2Base::stMask, ltex));
	// ---------------------------------------------------------------------------------------------------
	HR(Shader->SetBool(TileManager2Base::sbSpecular, has_specular));
	HR(Shader->SetBool(TileManager2Base::sbCloudSh, render_cloud_shadows));
	HR(Shader->SetBool(TileManager2Base::sbLights, render_lights));
	// ---------------------------------------------------------------------------------------------------
	if (render_lights) { HR(Shader->SetFloat(TileManager2Base::sfNight, float(mgr->Cprm().lightfac))); }
	else {				 HR(Shader->SetFloat(TileManager2Base::sfNight, 0.0f));	}

	Shader->CommitChanges();
	HR(Shader->Begin(&numPasses, D3DXFX_DONOTSAVESTATE));

	// -------------------------------------------------------------------
	// render surface mesh

	HR(Shader->BeginPass(0));
	pDev->SetVertexDeclaration(pPatchVertexDecl);
	pDev->SetStreamSource(0, mesh->pVB, 0, sizeof(VERTEX_2TEX));
	pDev->SetIndices(mesh->pIB);
	pDev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, mesh->nv, 0, mesh->nf);
	HR(Shader->EndPass());


	// add cloud shadows
	/*
	if (render_cloud_shadows) {

		const TileManager2<CloudTile> *cmgr = mgr->GetPlanet()->CloudMgr2();

		if (cmgr) {

			int nlng = 2 << lvl;
			int nlat = 1 << lvl;
			double minlat = PI * (double)(nlat/2-ilat-1)/(double)nlat;
			double maxlat = PI * (double)(nlat/2-ilat)/(double)nlat;
			double minlng = PI2 * (double)(ilng-nlng/2)/(double)nlng;
			double maxlng = PI2 * (double)(ilng-nlng/2+1)/(double)nlng;

			const Tile *tbuf[2];
			int maxlvl = min(lvl,9);
			int ncloud = cmgr->Coverage(minlat, maxlat, minlng, maxlng, maxlvl, tbuf, 2);
			if (ncloud > 0) {

				float alpha = (float)mgr->prm.rprm->shadowalpha;

				HR(Shader->SetFloat(eAlpha, alpha));

				//if (alpha < 0.01f) return; // don't render cloud shadows for this planet
				
				if (ncloud == 1) { // other cases still to be done

					for (int i = 0; i < ncloud; i++) {

						HR(Shader->SetTexture(eDiffTex, tbuf[i]->Tex()));

						// to be completed: create new mesh with modified texture coordinates
						const TEXCRDRANGE2 *ctexrange = tbuf[i]->GetTexRange();
						double cminlat, cmaxlat, cminlng, cmaxlng;
						float tu0, tu1, tv0, tv1, tuscale, tvscale;
						tbuf[i]->Extents (&cminlat, &cmaxlat, &cminlng, &cmaxlng);

						cminlng -= mgr->prm.rprm->cloudrot;
						cmaxlng -= mgr->prm.rprm->cloudrot;
						if (cmaxlng < -PI) {
							cminlng += PI2;
							cmaxlng += PI2;
						}

						static int ncvtx = (TILE_PATCHRES+3)*(TILE_PATCHRES+3);
						static VERTEX_2TEX *cvtx = new VERTEX_2TEX[ncvtx];
						if (mesh->nv > ncvtx) {
							delete []cvtx;
							cvtx = new VERTEX_2TEX[ncvtx=mesh->nv];
						}
						memcpy2(cvtx, mesh->vtx, mesh->nv*sizeof(VERTEX_2TEX));
						tu0 = ctexrange->tumin + (ctexrange->tumax-ctexrange->tumin)*(float)((minlng-cminlng)/(cmaxlng-cminlng));
						tu1 = ctexrange->tumin + (ctexrange->tumax-ctexrange->tumin)*(float)((maxlng-cminlng)/(cmaxlng-cminlng));
						tv0 = ctexrange->tvmin + (ctexrange->tvmax-ctexrange->tvmin)*(float)((maxlat-cmaxlat)/(cminlat-cmaxlat));
						tv1	= ctexrange->tvmin + (ctexrange->tvmax-ctexrange->tvmin)*(float)((minlat-cmaxlat)/(cminlat-cmaxlat));
						tuscale = (tu1-tu0)/(texrange.tumax-texrange.tumin);
						tvscale = (tv1-tv0)/(texrange.tvmax-texrange.tvmin);

						HR(Shader->SetVector(eTexOff, &D3DXVECTOR4(tuscale, texrange.tumin, tvscale, texrange.tvmin)));

						//cvtx[j].tu0 = tu0 + tuscale * (cvtx[j].tu0-texrange.tumin);
						//cvtx[j].tv0 = tv0 + tvscale * (cvtx[j].tv0-texrange.tvmin);
						
						Shader->CommitChanges();

						HR(Shader->BeginPass(1));
						//pDev->DrawIndexedPrimitive (D3DPT_TRIANGLELIST, FVF_2TEX, cvtx, mesh->nv, mesh->idx, mesh->ni, 0);
						HR(Shader->EndPass());
					}
				}
			}
		}
	}*/

	HR(Shader->End());	
}


// -----------------------------------------------------------------------

void SurfTile::MatchEdges ()
{
	if (edgeok) return; // done already
	edgeok = true;
	if (!mesh) return;  // sanity check

	QuadTreeNode<SurfTile> *lngnbr = smgr->FindNode (lvl, ilat, ilng + (ilng & 1 ? 1 : -1));
	QuadTreeNode<SurfTile> *latnbr = smgr->FindNode (lvl, ilat + (ilat & 1 ? 1 : -1), ilng);
	QuadTreeNode<SurfTile> *dianbr = smgr->FindNode (lvl, ilat + (ilat & 1 ? 1 : -1), ilng + (ilng & 1 ? 1 : -1));

	if (lngnbr && !(lngnbr->Entry()->state & TILE_VALID)) lngnbr = 0;
	if (latnbr && !(latnbr->Entry()->state & TILE_VALID)) latnbr = 0;
	if (dianbr && !(dianbr->Entry()->state & TILE_VALID)) dianbr = 0;

	int new_lngnbr_lvl = (lngnbr ? lngnbr->Entry()->lvl : lvl);
	int new_latnbr_lvl = (latnbr ? latnbr->Entry()->lvl : lvl);
	int new_dianbr_lvl = (dianbr ? dianbr->Entry()->lvl : lvl);

	// fix lower-level neighbours first
	bool nbr_updated = false;
	if (new_lngnbr_lvl < lvl) {
		lngnbr->Entry()->MatchEdges ();
		nbr_updated = true;
	}
	if (new_latnbr_lvl < lvl) {
		latnbr->Entry()->MatchEdges ();
		nbr_updated = true;
	}
	if (!nbr_updated && new_dianbr_lvl < lvl) {
		// now we need to check the diagonal neighbour, since this could be lower resolution
		// and thus responsible for the corner node elevation
		dianbr->Entry()->MatchEdges ();
	}

	bool lngedge_changed = (new_lngnbr_lvl != lngnbr_lvl);
	bool latedge_changed = (new_latnbr_lvl != latnbr_lvl);
	bool diaedge_changed = (new_dianbr_lvl != dianbr_lvl);

	if (lngedge_changed || latedge_changed || diaedge_changed) {
		if (new_dianbr_lvl < new_lngnbr_lvl && new_dianbr_lvl < new_latnbr_lvl) {
			FixCorner (dianbr ? dianbr->Entry() : 0);
			FixLatitudeBoundary (latnbr ? latnbr->Entry() : 0, true);
			FixLongitudeBoundary (lngnbr ? lngnbr->Entry() : 0, true);
		} else if (new_latnbr_lvl < new_lngnbr_lvl) {
			FixLatitudeBoundary (latnbr ? latnbr->Entry() : 0);
			FixLongitudeBoundary (lngnbr ? lngnbr->Entry() : 0, true);
		} else {
			FixLongitudeBoundary (lngnbr ? lngnbr->Entry() : 0);
			FixLatitudeBoundary (latnbr ? latnbr->Entry() : 0, true);
		}
		mesh->MapVertices(mgr->Dev()); // copy the updated vertices to the vertex buffer
		lngnbr_lvl = new_lngnbr_lvl;
		latnbr_lvl = new_latnbr_lvl;
		dianbr_lvl = new_dianbr_lvl;
	}
}

// -----------------------------------------------------------------------

void SurfTile::FixCorner (const SurfTile *nbr)
{
	if (!mesh) return; // sanity check

	int vtx_idx = (ilat & 1 ? 0 : (TILE_PATCHRES+1)*TILE_PATCHRES) + (ilng & 1 ? TILE_PATCHRES : 0);
	VERTEX_2TEX &vtx_store = mesh->vtx[mesh->nv + (ilat & 1 ? 0 : TILE_PATCHRES)];

	INT16 *elev = ElevationData();
	if (!elev) return;

	if (nbr) {
		INT16 *nbr_elev = nbr->ElevationData();
		if (!nbr_elev) return;

		INT16 corner_elev = elev[TILE_ELEVSTRIDE+1 + (ilat & 1 ? 0 : TILE_ELEVSTRIDE*TILE_PATCHRES) + (ilng & 1 ? TILE_PATCHRES : 0)];
		INT16 nbr_corner_elev = nbr_elev[TILE_ELEVSTRIDE+1 + (ilat & 1 ? TILE_ELEVSTRIDE*TILE_PATCHRES : 0) + (ilng & 1 ? 0 : TILE_PATCHRES)];
	
		double rad = mgr->CbodySize();
		double radfac = (rad+nbr_corner_elev)/(rad+corner_elev);
		mesh->vtx[vtx_idx].x = vtx_store.x*radfac + vtxshift.x*(radfac-1.0);
		mesh->vtx[vtx_idx].y = vtx_store.y*radfac + vtxshift.y*(radfac-1.0);
		mesh->vtx[vtx_idx].z = vtx_store.z*radfac + vtxshift.z*(radfac-1.0);
	}
}

// -----------------------------------------------------------------------

void SurfTile::FixLongitudeBoundary (const SurfTile *nbr, bool keep_corner)
{
	// Fix the left or right edge
	int i, i0, i1, j, vtx_ofs, nbrlvl, dlvl;
	VERTEX_2TEX *vtx_store;
	static int subidxmask[4] = {0,1,3,7};

	vtx_ofs = (ilng & 1 ? TILE_PATCHRES : 0); // check if neighbour is at left or right edge
	if (nbr && nbr->mesh && nbr->mesh->vtx) {
		nbrlvl = min(nbr->lvl, lvl); // we don't need to worry about neigbour levels higher than ours
		vtx_store = mesh->vtx + mesh->nv;
		if (nbrlvl == lvl) { // put my own edge back
			i0 = 0;
			i1 = TILE_PATCHRES;
			if (keep_corner)
				if (ilat & 1) i0++; else i1--;
			for (i = i0; i <= i1; i++)
				mesh->vtx[vtx_ofs+i*(TILE_PATCHRES+1)] = vtx_store[i];
		} else {  // interpolate to neighbour's left edge
			dlvl = lvl-nbrlvl;
			if (dlvl <= 5) { // for larger tile level differences the interleaved sampling method doesn't work
				INT16 *elev = ElevationData();
				INT16 *nbr_elev = nbr->ElevationData();
				if (elev && nbr_elev) {
					elev += TILE_ELEVSTRIDE+1 + vtx_ofs; // add offset
					int nsub = 1 << dlvl;    // number of tiles fitting alongside the lowres neighbour
					int vtx_skip = nsub*(TILE_PATCHRES+1);  // interleave factor for nodes attaching to neigbour nodes
					int nbr_range = TILE_PATCHRES/nsub; // number of neighbour vertices attaching to us - 1
					int subidx = ilat & subidxmask[dlvl];
					int nbr_ofs = (subidxmask[dlvl]-subidx) * nbr_range * TILE_ELEVSTRIDE;
					nbr_elev += TILE_ELEVSTRIDE+1 + nbr_ofs + (TILE_PATCHRES-vtx_ofs);
					double rad = mgr->CbodySize();
					// match nodes to neighbour elevations
					i0 = 0;
					i1 = nbr_range;
					if (keep_corner)
						if (ilat & 1) i0++; else i1--;
					for (i = i0; i <= i1; i++) {
						double radfac = (rad+nbr_elev[i*TILE_ELEVSTRIDE])/(rad+elev[i*TILE_ELEVSTRIDE*nsub]);
						mesh->vtx[vtx_ofs+i*vtx_skip].x = vtx_store[i*nsub].x*radfac + vtxshift.x*(radfac-1.0);
						mesh->vtx[vtx_ofs+i*vtx_skip].y = vtx_store[i*nsub].y*radfac + vtxshift.y*(radfac-1.0);
						mesh->vtx[vtx_ofs+i*vtx_skip].z = vtx_store[i*nsub].z*radfac + vtxshift.z*(radfac-1.0);
					}
					// interpolate the nodes that fall between neighbour nodes
					for (i = 0; i < nbr_range; i++)
						for (j = 1; j < nsub; j++)
							VtxInterpolate (mesh->vtx[vtx_ofs+j*(TILE_PATCHRES+1)+i*vtx_skip], mesh->vtx[vtx_ofs+i*vtx_skip], mesh->vtx[vtx_ofs+(i+1)*vtx_skip], (double)j/double(nsub));
				}
			} else {
				// problems
			}
		}
	}
}

// -----------------------------------------------------------------------

void SurfTile::FixLatitudeBoundary (const SurfTile *nbr, bool keep_corner)
{
	// Fix the top or bottom edge
	int i, i0, i1, j, nbrlvl, dlvl;
	VERTEX_2TEX *vtx_store;
	static int subidxmask[4] = {0,1,3,7};

	int line = (ilat & 1 ? 0 : TILE_PATCHRES);
	int vtx_ofs = (ilat & 1 ? 0 : line*(TILE_PATCHRES+1));
	if (nbr && nbr->mesh && nbr->mesh->vtx) {
		nbrlvl = min(nbr->lvl, lvl); // we don't need to worry about neigbour levels higher than ours
		vtx_store = mesh->vtx + mesh->nv + TILE_PATCHRES + 1;
		if (nbrlvl == lvl) { // put my own edge back
			i0 = 0;
			i1 = TILE_PATCHRES;
			if (keep_corner)
				if (ilng & 1) i1--; else i0++;
			for (i = i0; i <= i1; i++)
				mesh->vtx[vtx_ofs+i] = vtx_store[i];
		} else {
			dlvl = lvl-nbrlvl;
			if (dlvl <= 5) { // for larger tile level differences the interleaved sampling method doesn't work
				INT16 *elev = ElevationData();
				INT16 *nbr_elev = nbr->ElevationData();
				if (elev && nbr_elev) {
					elev += (line+1)*TILE_ELEVSTRIDE + 1; // add offset
					int nsub = 1 << dlvl;    // number of tiles fitting alongside the lowres neighbour
					int vtx_skip = nsub;     // interleave factor for nodes attaching to neigbour nodes
					int nbr_range = TILE_PATCHRES/nsub; // number of neighbour vertices attaching to us - 1
					int subidx = ilng & subidxmask[dlvl];
					int nbr_ofs = subidx * nbr_range;
					nbr_elev += TILE_ELEVSTRIDE+1 + nbr_ofs + (TILE_PATCHRES-line)*TILE_ELEVSTRIDE;
					double rad = mgr->CbodySize();
					// match nodes to neighbour elevations
					i0 = 0;
					i1 = nbr_range;
					if (keep_corner)
						if (ilng & 1) i1--; else i0++;
					for (i = i0; i <= i1; i++) {
						double radfac = (rad+nbr_elev[i])/(rad+elev[i*nsub]);
						mesh->vtx[vtx_ofs+i*vtx_skip].x = vtx_store[i*nsub].x*radfac + vtxshift.x*(radfac-1.0);
						mesh->vtx[vtx_ofs+i*vtx_skip].y = vtx_store[i*nsub].y*radfac + vtxshift.y*(radfac-1.0);
						mesh->vtx[vtx_ofs+i*vtx_skip].z = vtx_store[i*nsub].z*radfac + vtxshift.z*(radfac-1.0);
					}
					// interpolate the nodes that fall between neighbour nodes
					for (i = 0; i < nbr_range; i++)
						for (j = 1; j < nsub; j++)
							VtxInterpolate (mesh->vtx[vtx_ofs+i*vtx_skip+j], mesh->vtx[vtx_ofs+i*vtx_skip], mesh->vtx[vtx_ofs+(i+1)*vtx_skip], (double)j/double(nsub));
				}
			} else {
				// problems
			}
		}
	}
}



// =======================================================================
// =======================================================================

template<>
void TileManager2<SurfTile>::Render (MATRIX4 &dwmat, bool use_zbuf, const vPlanet::RenderPrm &rprm)
{
	// set generic parameters
	SetRenderPrm (dwmat, 0, use_zbuf, rprm);

	double np = 0.0, fp = 0.0;
	int i;

	class Scene *scene = GClient()->GetScene();

	// adjust scaling parameters (can only be done if no z-buffering is in use)
	if (!use_zbuf) {
		double R = obj_size;
		double D = prm.cdist*R;
		double zmax = (D - R*R/D) * 1.5;
		double zmin = max (2.0, min (zmax*1e-4, (D-R) * 0.8));
		double zscale = 1.0;

		np = scene->GetCameraNearPlane();
		fp = scene->GetCameraFarPlane();
		scene->SetCameraFrustumLimits (zmin, zmax);
	}

	// build a transformation matrix for frustum testing
	MATRIX4 Mproj = _MATRIX4(scene->GetProjectionMatrix());
	Mproj.m33 = 1.0; Mproj.m43 = -1.0;  // adjust near plane to 1, far plane to infinity
	MATRIX4 Mview = _MATRIX4(scene->GetViewMatrix());
	prm.dviewproj = mul(Mview,Mproj);

	// ---------------------------------------------------------------------

	static float spec_base = 0.7f; // 0.95f;

	// ---------------------------------------------------------------------
	// Initialize shading technique and feed planet specific data to shaders
	//

	HR(Shader()->SetTechnique(eTileTech));
	HR(Shader()->SetMatrix(smViewProj, scene->GetProjectionViewMatrix()));
	HR(Shader()->SetVector(svWater, &D3DXVECTOR4(spec_base*1.2f, spec_base*1.0f, spec_base*0.8f, 50.0f)));

	if (rprm.bAddBkg) { HR(Shader()->SetValue(svAddBkg, &rprm.SkyColor, sizeof(D3DXCOLOR))); }
	else			  { HR(Shader()->SetVector(svAddBkg, &D3DXVECTOR4(0, 0, 0, 0))); }

	// ------------------------------------------------------------------
	// TODO: render full sphere for levels < 4

	loader->WaitForMutex();

	// update the tree
	for (i = 0; i < 2; i++)
		ProcessNode (tiletree+i);

	// render the tree
	for (i = 0; i < 2; i++)
		RenderNode (tiletree+i);

	loader->ReleaseMutex();

	if (np)
		scene->SetCameraFrustumLimits(np,fp);
}