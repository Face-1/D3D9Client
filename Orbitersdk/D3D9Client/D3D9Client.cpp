// ==============================================================
// D3D9Client.cpp
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2006-2016 Martin Schweiger
//				 2012-2016 Jarmo Nikkanen
// ==============================================================


#define STRICT 1
#define ORBITER_MODULE

#include <set> // ...for Brush-, Pen- and Font-accounting
#include "Orbitersdk.h"
#include "D3D9Client.h"
#include "D3D9Config.h"
#include "D3D9Util.h"
#include "D3D9Catalog.h"
#include "D3D9Surface.h"
#include "D3D9TextMgr.h"
#include "D3D9Frame.h"
#include "D3D9Pad.h"
#include "CSphereMgr.h"
#include "Scene.h"
#include "Mesh.h"
#include "VVessel.h"
#include "VStar.h"
#include "Texture.h"
#include "MeshMgr.h"
#include "Particle.h"
#include "TileMgr.h"
#include "RingMgr.h"
#include "HazeMgr.h"
#include "Log.h"
#include "VideoTab.h"
#include "GDIPad.h"
#include "FileParser.h"
#include "OapiExtension.h"
#include "DebugControls.h"
#include "Surfmgr2.h"
#include <unordered_map>


#if defined(_MSC_VER) && (_MSC_VER <= 1700 ) // Microsoft Visual Studio Version 2012 and lower
#define round(v) floor(v+0.5)
#endif

// ==============================================================
// Structure definitions

struct D3D9Client::RenderProcData {
	__gcRenderProc proc;
	void *pParam;
	DWORD id;
};

struct D3D9Client::GenericProcData {
	__gcGenericProc proc;
	void *pParam;
	DWORD id;
};

using namespace oapi;

HINSTANCE g_hInst = 0;
D3D9Client *g_client = 0;
D3D9Catalog<D3D9Mesh*>			 *MeshCatalog;
D3D9Catalog<LPDIRECT3DTEXTURE9>	 *TileCatalog;
D3D9Catalog<LPD3D9CLIENTSURFACE> *SurfaceCatalog;

DWORD uCurrentMesh = 0;
vObject *pCurrentVisual = 0;
_D3D9Stats D3D9Stats;

#ifdef _NVAPI_H
 StereoHandle pStereoHandle = 0;
#endif

bool bFreeze = false;
bool bFreezeEnable = false;

// Module local constellation marker storage
static GraphicsClient::LABELSPEC *g_cm_list = NULL;
static DWORD g_cm_list_count = 0;

// Debuging Brush-, Pen- and Font-accounting
std::set<Font *> g_fonts;
std::set<Pen *> g_pens;
std::set<Brush *> g_brushes;

extern list<gcGUIApp *> g_gcGUIAppList;

extern "C" {
	_declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
}

// ==============================================================
// API interface
// ==============================================================

// ==============================================================
// Initialise module

DLLCLBK void InitModule(HINSTANCE hDLL)
{

#ifdef _DEBUG
	_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
	// _CrtSetBreakAlloc(8351);

	assert(sizeof(FVECTOR4) == 16);
	assert(sizeof(FVECTOR4::data) == 16);

	assert(sizeof(FVECTOR4::rgb) == 12);
	assert(sizeof(FVECTOR4::xyz) == 12);

	auto dut = FVECTOR4(1.2, 3.4, 5.6, 7.8);
	assert(dut.data[0] == dut.r);
	assert(dut.data[1] == dut.g);
	assert(dut.data[2] == dut.b);
	assert(dut.data[3] == dut.a);

	assert(dut.data[0] == dut.x);
	assert(dut.data[1] == dut.y);
	assert(dut.data[2] == dut.z);
	assert(dut.data[3] == dut.w);

	assert(dut.rgb.x == dut.xyz.x);
	assert(dut.rgb.y == dut.xyz.y);
	assert(dut.rgb.z == dut.xyz.z);

	assert(dut.a == dut.w);

	// Check that 'a' and 'w' are placed behind 'rgb' rsp. 'xyz' and unaffected
	dut.rgb = 0;
	assert(dut.a == dut.w);
#endif

	D3D9InitLog("Modules/D3D9Client/D3D9ClientLog.html");

	if (!D3DXCheckVersion(D3D_SDK_VERSION, D3DX_SDK_VERSION)) {
		MissingRuntimeError();
		return;
	}

#ifdef _NVAPI_H
	if (NvAPI_Initialize()==NVAPI_OK) {
		LogAlw("[nVidia API Initialized]");
		bNVAPI = true;
	}
	else LogAlw("[nVidia API Not Available]");
#else
	LogAlw("[Not Compiled With nVidia API]");
#endif

	Config			= new D3D9Config();
	TileCatalog		= new D3D9Catalog<LPDIRECT3DTEXTURE9>();
	MeshCatalog		= new D3D9Catalog<D3D9Mesh*>;
	SurfaceCatalog	= new D3D9Catalog<LPD3D9CLIENTSURFACE>();

	DebugControls::Create();
	AtmoControls::Create();

	g_hInst = hDLL;
	g_client = new D3D9Client(hDLL);

	if (oapiRegisterGraphicsClient(g_client)==false) {
		delete g_client;
		g_client = 0;
	}
}

// ==============================================================
// Clean up module

DLLCLBK void ExitModule(HINSTANCE hDLL)
{
	LogAlw("--------------ExitModule------------");

	delete TileCatalog;
	delete MeshCatalog;
	delete SurfaceCatalog;
	delete Config;

	DebugControls::Release();
	AtmoControls::Release();

	if (g_client) {
		oapiUnregisterGraphicsClient(g_client);
		g_client = 0;
	}

#ifdef _NVAPI_H
	if (bNVAPI) if (NvAPI_Unload()==NVAPI_OK) LogAlw("[nVidia API Unloaded]");
#endif

	LogAlw("Log Closed");
	D3D9CloseLog();
}

DLLCLBK gcGUIBase * gcGetGUICore()
{
	return dynamic_cast<gcGUIBase *>(g_pWM);
}


DLLCLBK gcCore * gcGetCoreAPI()
{
	return dynamic_cast<gcCore *>(g_client);
}


// ==============================================================
// Face's Terrain Flattening section
// ==============================================================

struct FlatShape
{
	double Lat;
	double Lng;
	double Dim1;
	double Dim2;
	double Cos;
	double Sin;
	double Falloff;
	int Height;
	int Type;
};

struct TilePixelPosition
{
	int iLat;
	int iLng;
	int X;
	int Y;
	bool operator==(const TilePixelPosition& other) const
	{
		return iLat == other.iLat && iLng == other.iLng && X == other.X && Y == other.Y;
	}
};

std::unordered_map<OBJHANDLE, std::unordered_map<int, std::unordered_map<int, std::unordered_map<int, std::vector<FlatShape*>>>>> g_ElevationFlatteningShapes;
std::unordered_map<OBJHANDLE, std::vector<FlatShape*>> g_ShapeStore;
std::unordered_map<OBJHANDLE, bool> g_ShapesLoaded;

std::vector<std::string> EnumerateDirectory(std::string directory, std::string filter)
{
	std::vector<std::string> result;
	std::string search_path = directory + "\\" + filter;
	WIN32_FIND_DATA fd;
	HANDLE hFind = ::FindFirstFile(search_path.c_str(), &fd);
	if (hFind != INVALID_HANDLE_VALUE)
	{
		do
		{
			if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) result.push_back(fd.cFileName);
		} while (::FindNextFile(hFind, &fd));
		::FindClose(hFind);
	}
	return result;
}

void GetTilePixelPosition(int lvl, double lat, double lng, TilePixelPosition &pos)
{
	//Normalize input
	if (lat < -90) lat = -90;
	if (lat > 90) lat = 90;
	while (lng < -180)
	{
		lng += 360;
	}
	while (lng > 180)
	{
		lng -= 360;
	}

	//Calculate tile position and pixel position
	double bands = pow((double)2, lvl);
	double width = 180 / bands;
	pos.iLat = (int)((-lat + 90) / width);
	pos.iLng = (int)((lng + 180) / width);
	double step = width / 256;
	double latmin = 90 - width*(pos.iLat + 1);
	double lngmin = -180 + width*pos.iLng;
	pos.X = (int)((lng - lngmin) / width * 256);
	pos.Y = (int)(-(lat - latmin) / width * 256 + 256);
}

void ProcessPlanetFlats(OBJHANDLE hPlanet)
{
	char name[MAX_PATH];
	char fname[MAX_PATH];
	oapiGetObjectName(hPlanet, name, ARRAYSIZE(name) - 6);
	sprintf_s(fname, ARRAYSIZE(fname), "%s\\Flat", name);
	g_client->TexturePath(fname, name);
	auto files = EnumerateDirectory(name, "*.flt");
	// Load all planet shapes
	for (auto file : files)
	{
		auto radius = oapiGetSize(hPlanet);
		sprintf_s(fname, ARRAYSIZE(fname), "%s\\%s", name, file.c_str());
		auto f = fopen(fname, "r");
		if (f != 0)
		{
			while (!feof(f))
			{
				int height, dim1, dim2, falloff, read;
				double lat, lng, phi;
				if ((read = fscanf(f, "%s %d %lf %lf %d %d %lf %d", fname, &height, &lng, &lat, &dim1, &dim2, &phi, &falloff)) < 5) continue; // Skip incomplete lines
				if (fname[0] == '/' && fname[1] == '/') continue; // Skip commented lines
				_strlwr(fname);
				if (read < 6)	dim2 = dim1; // Fallback for one dimension only
				if (read < 7)	phi = 0;     // Fallback for no angle given
				if (read < 8)	falloff = 0; // Fallback for no falloff given
				auto decdeg = radius*cos(lat*RAD) * 2 * PI;
				decdeg = 360 / decdeg;
				if (strcmp(fname, "ellipse") == 0)
				{
					g_ShapeStore[hPlanet].push_back(new FlatShape{ lat ,lng , (double)dim1*decdeg, (double)dim2*decdeg, cos(-phi*RAD),sin(-phi*RAD), (double)falloff / 100, height, 1 });
				}
				else if (strcmp(fname, "rect") == 0)
				{
					g_ShapeStore[hPlanet].push_back(new FlatShape{ lat ,lng , (double)dim1*decdeg / 2, (double)dim2*decdeg / 2, cos(-phi*RAD),sin(-phi*RAD), (double)falloff / 100, height, 4 });
				}
			}
			fclose(f);
		}
	}
	// Build up planet map only if at least one shape was added
	if (g_ShapeStore.find(hPlanet) != g_ShapeStore.end()) for (auto shape : g_ShapeStore[hPlanet])
	{
		double offset = sqrt(shape->Dim1*shape->Dim1 + shape->Dim2*shape->Dim2);
		double latmin = shape->Lat - offset;
		double latmax = shape->Lat + offset;
		double lngmin = shape->Lng - offset;
		double lngmax = shape->Lng + offset;
		TilePixelPosition leftUpper, rightUpper, leftLower, rightLower;
		for (auto lvl = 13; lvl >= 0; lvl--)
		{
			int latBands = 1 << lvl;
			int lngBands = latBands * 2;

			// Get tile pixel position of all corner points
			GetTilePixelPosition(lvl, latmax, lngmin, leftUpper);
			GetTilePixelPosition(lvl, latmax, lngmax, rightUpper);
			GetTilePixelPosition(lvl, latmin, lngmin, leftLower);
			// Since we are symmetrically, we can deduce the last corner directly
			rightLower.iLat = leftLower.iLat;
			rightLower.iLng = rightUpper.iLng;
			rightLower.X = rightUpper.X;
			rightLower.Y = leftLower.Y;
			// Check for subpixel visibility
			if (leftUpper == rightUpper || leftLower == rightLower || leftUpper == leftLower || rightUpper == rightLower) break;
			// Shift if longitude edge is crossed
			if (leftUpper.iLng > rightUpper.iLng)
			{
				leftUpper.iLng -= lngBands;
				leftLower.iLng -= lngBands;
			}
			// Check for edge pixel to compensate overlapping in elevation tiles
			if (leftUpper.X == 0)
			{
				leftUpper.iLng--;
				leftLower.iLng--;
			}
			if (rightUpper.X == 255)
			{
				rightUpper.iLng++;
				rightLower.iLng++;
			}
			if (leftUpper.Y == 0 && leftUpper.iLat>0)
			{
				leftUpper.iLat--;
				rightUpper.iLat--;
			}
			if (leftLower.Y == 255 && leftLower.iLat<latBands)
			{
				leftLower.iLat++;
				rightLower.iLat++;
			}
			// Sweep over the tile set to put in shape paths
			for (auto ilat = leftUpper.iLat; ilat <= leftLower.iLat; ilat++)
				for (auto ilng = leftUpper.iLng; ilng <= rightUpper.iLng; ilng++)
				{
					// Compensate for longitude edge switch
					g_ElevationFlatteningShapes[hPlanet][lvl][ilat][ilng < 0 ? lngBands + ilng : ilng].push_back(shape);
				}
		}
	}
	g_ShapesLoaded[hPlanet] = true;
}

template <typename Type>
bool FilterElevation(OBJHANDLE hPlanet, int lvl, int ilat, int ilng, double elev_res, Type *elev)
{
	if (!elev) return false;
	if (g_ShapesLoaded.find(hPlanet) == g_ShapesLoaded.end()) ProcessPlanetFlats(hPlanet);
	if (g_ElevationFlatteningShapes.find(hPlanet) == g_ElevationFlatteningShapes.end()) return false; // Nothing for planet
	auto planetShapes = g_ElevationFlatteningShapes[hPlanet];
	if (planetShapes.find(lvl) == planetShapes.end()) return false; // Nothing at this level
	auto levelShapes = planetShapes[lvl];
	if (levelShapes.find(ilat) == levelShapes.end()) return false; // Nothing at this latitude
	auto latShapes = levelShapes[ilat];
	if (latShapes.find(ilng) == latShapes.end()) return false; // Nothing at this longitude
	auto shapes = latShapes[ilng];
	double bands = pow((double)2, lvl);
	double width = 180 / bands;
	double step = width / 256;
	double latmin = 90 - width*(ilat + 1) - step * 1.5;
	double lngmin = -180 + width*ilng - step * 1.5;;
	for (auto i = 0; i < TILE_ELEVSTRIDE*TILE_ELEVSTRIDE; i++)
	{
		for (auto shape : shapes)
		{
			double y = latmin + (i / 259)*step - shape->Lat;
			double x = lngmin + (i % 259)*step - shape->Lng;
			double x1 = (x * shape->Cos - y * shape->Sin) / shape->Dim1;
			double y1 = (x * shape->Sin + y * shape->Cos) / shape->Dim2;
			for (auto pot = 0; pot < shape->Type; pot++)
			{
				x1 *= x1;
				y1 *= y1;
			}
			double dist = x1 + y1;
			if (dist <= 1.0)
			{
				// Do the math in INT16 basis even for "float" output
				INT16 elevation = INT16((double)shape->Height / elev_res);
				if (shape->Falloff > 0)
				{
					for (auto pot = 0; pot < shape->Type; pot++)
					{
						dist = sqrt(dist);
					}
					if (dist >= 1 - shape->Falloff)
					{
						double alpha = (dist - 1 + shape->Falloff) / shape->Falloff;
						elevation = INT16(elev[i] * alpha + (double)shape->Height / elev_res * (1 - alpha));
					}
				}
				// Convert to float or keep as an INT16 depending on case
				elev[i] = Type(elevation);
			}
		}
	}
	return true;
}


bool FilterElevationPhysics(OBJHANDLE hPlanet, int lvl, int ilat, int ilng, double elev_res, INT16 *elev)
{
	if (!Config->bFlatsEnabled) return false;
	char name[64];
	oapiGetObjectName(hPlanet, name, 64);
	auto result = FilterElevation<INT16>(hPlanet, lvl, ilat, ilng, elev_res, elev);
	if (result)
		LogClr("Coral", "FilterElevation[Physics][%s]: Level=%d, ilat=%d, ilng=%d", name, lvl, ilat, ilng);
	return result;
}


void FilterElevationGraphics(OBJHANDLE hPlanet, int lvl, int ilat, int ilng, float *elev)
{
	if (!Config->bFlatsEnabled) return;
	char name[64];
	oapiGetObjectName(hPlanet, name, 64);
	if (FilterElevation<float>(hPlanet, lvl, ilat, ilng, 1.0, elev))
		LogClr("Coral", "FilterElevation[Graphics][%s]: Level=%d, ilat=%d, ilng=%d", name, lvl, ilat, ilng);
}





// ==============================================================
// D3D9Client class implementation
// ==============================================================

D3D9Client::D3D9Client (HINSTANCE hInstance) :
	gcCore(),
	GraphicsClient(hInstance),
	vtab(NULL),
	scenarioName("(none selected)"),
	pLoadLabel(""),
	pLoadItem(""),
	pDevice     (NULL),
	pDefaultTex (NULL),
	pScatterTest(NULL),
	pFramework  (NULL),
	pItemsSkp   (NULL),
	texmgr      (NULL),
	hLblFont1   (NULL),
	hLblFont2   (NULL),
	hMainThread (NULL),
	pCaps       (NULL),
	pWM			(NULL),
	parser    (),
	hRenderWnd(),
	scene     (),
	meshmgr   (),
	bControlPanel (false),
	bScatterUpdate(false),
	bFullscreen   (false),
	bAAEnabled    (false),
	bFailed       (false),
	bRunning      (false),
	bHalt         (false),
	bVertexTex    (false),
	bVSync        (false),
	bRendering	  (false),
	bGDIClear	  (true),
	viewW         (0),
	viewH         (0),
	viewBPP       (0),
	frame_timer   (0),
	loadd_x       (0),
	loadd_y       (0),
	loadd_w       (0),
	loadd_h       (0),
	LabelPos      (0)

{
}

// ==============================================================

D3D9Client::~D3D9Client()
{
	LogAlw("D3D9Client destructor called");
	SAFE_DELETE(vtab);

	// Free constellation names memory (if allocted)
	if (g_cm_list) {
		for (DWORD n = 0; n < g_cm_list_count; ++n) {
			delete[] g_cm_list[n].label[0];
			delete[] g_cm_list[n].label[1];
		}
		delete[] g_cm_list;
	}

	//
	// Orbiter seems not to release all resources :(
	//

	// --- Fonts
	if (g_fonts.size()) {
		LogWrn("%u un-released fonts!", g_fonts.size());
		for (auto it = g_fonts.begin(); it != g_fonts.end(); ) {
			clbkReleaseFont(*it++);
		}
		g_fonts.clear();
	}
	// --- Brushes
	if (g_brushes.size()) {
		LogWrn("%u un-released brushes!", g_brushes.size());
		for (auto it = g_brushes.begin(); it != g_brushes.end(); ) {
			clbkReleaseBrush(*it++);
		}
		g_brushes.clear();
	}
	// --- Pens
	if (g_pens.size()) {
		LogWrn("%u un-released pens!", g_pens.size());
		for (auto it = g_pens.begin(); it != g_pens.end(); ) {
			clbkReleasePen(*it++);
		}
		g_pens.clear();
	}
}


// ==============================================================
// 
bool D3D9Client::ChkDev(const char *fnc) const
{
	if (pDevice) return false;
	LogErr("Call [%s] Failed. D3D9 Graphics services off-line", fnc);
	return true;
}


// ==============================================================
// Overridden
//
const void *D3D9Client::GetConfigParam (DWORD paramtype) const
{
	return (paramtype >= CFGPRM_SHOWBODYFORCEVECTORSFLAG)
		 ? (paramtype >= CFGPRM_GETSELECTEDMESH)
		 ? DebugControls::GetConfigParam(paramtype)
		 : OapiExtension::GetConfigParam(paramtype)
		 : GraphicsClient::GetConfigParam(paramtype);
}


// ==============================================================
// This is called only once when the launchpad will appear
// This callback will initialize the Video tab only
//
bool D3D9Client::clbkInitialise()
{
	_TRACE;
	LogAlw("================ clbkInitialise ===============");
	LogAlw("Orbiter Version = %d",oapiGetOrbiterVersion());

	// Perform default setup
	if (GraphicsClient::clbkInitialise()==false) return false;
	//Create the Launchpad video tab interface
	vtab = new VideoTab(this, ModuleInstance(), OrbiterInstance(), LaunchpadVideoTab());

	return true;
}


// ==============================================================
// This is called when a simulation session will begin
//
HWND D3D9Client::clbkCreateRenderWindow()
{
	_TRACE;

	LogAlw("================ clbkCreateRenderWindow ===============");

	Config->bFlatsEnabled = (Config->bFlats != 0);

	// Disable flattening with "cubic interpolation" it's not going to work.
	if ((*(int*)GetConfigParam(CFGPRM_ELEVATIONMODE)) == 2) {
		Config->bFlatsEnabled = false;
	}

	uEnableLog		 = Config->DebugLvl;
	pSplashScreen    = NULL;
	pBackBuffer      = NULL;
	pTextScreen      = NULL;
	hRenderWnd       = NULL;
	pDefaultTex		 = NULL;
	hLblFont1		 = NULL;
	hLblFont2		 = NULL;
	bControlPanel    = false;
	bFullscreen      = false;
	bFailed			 = false;
	bRunning		 = false;
	bHalt			 = false;
	bVertexTex		 = false;
	viewW = viewH    = 0;
	viewBPP          = 0;
	frame_timer		 = 0;
	scene            = NULL;
	meshmgr          = NULL;
	texmgr           = NULL;
	pFramework       = NULL;
	pDevice			 = NULL;
	parser			 = NULL;
	pBltGrpTgt		 = NULL;	// Let's set this NULL here, constructor is called only once. Not when exiting and restarting a simulation.
	pNoiseTex		 = NULL;
	surfBltTgt		 = NULL;	// This variable is not used, set it to NULL anyway
	hMainThread		 = GetCurrentThread();

	memset(&D3D9Stats, 0, sizeof(D3D9Stats));

	D3DXMatrixIdentity(&ident);

	oapiDebugString()[0] = '\0';

	TileCatalog->Clear();
	MeshCatalog->Clear();
	SurfaceCatalog->Clear();

	hRenderWnd = GraphicsClient::clbkCreateRenderWindow();

	LogAlw("Window Handle = %s",_PTR(hRenderWnd));
	SetWindowText(hRenderWnd, "[D3D9Client]");

	LogOk("Starting to initialize device and 3D environment...");

	pFramework = new CD3DFramework9();

	if (pFramework->GetDirect3D()==NULL) return NULL;

	WriteLog("[DirectX 9 Initialized]");

	HRESULT hr = pFramework->Initialize(hRenderWnd, GetVideoData());

	if (hr!=S_OK) {
		LogErr("ERROR: Failed to initialize 3D Framework");
		return NULL;
	}

	RECT rect;
	GetClientRect(hRenderWnd, &rect);
	HDC hWnd = GetDC(hRenderWnd);
	HBRUSH hBr = CreateSolidBrush(RGB(0,0,0));
	FillRect(hWnd, &rect, hBr);
	DeleteObject(hBr);
	ReleaseDC(hRenderWnd, hWnd);
	ValidateRect(hRenderWnd, NULL);	// avoids white flash after splash screen

	pCaps = pFramework->GetCaps();

	WriteLog("[3DDevice Initialized]");

	pDevice		= pFramework->GetD3DDevice();
	viewW		= pFramework->GetWidth();
	viewH		= pFramework->GetHeight();
	bFullscreen = (pFramework->IsFullscreen() == TRUE);
	bAAEnabled  = (pFramework->IsAAEnabled() == TRUE);
	viewBPP		= 32;
	bVertexTex  = (pFramework->HasVertexTextureSup() == TRUE);
	bVSync		= (pFramework->GetVSync() == TRUE);

	char fld[] = "D3D9Client";

	HR(D3DXCreateTextureFromFileA(pDevice, "Textures/D3D9Noise.dds", &pNoiseTex));

	D3D9ClientSurface::D3D9TechInit(this, pDevice, fld);

	HR(pDevice->GetRenderTarget(0, &pBackBuffer));
	HR(pDevice->GetDepthStencilSurface(&pDepthStencil));

	LogAlw("Render Target = %s", _PTR(pBackBuffer));
	LogAlw("DepthStencil = %s", _PTR(pDepthStencil));

	meshmgr		= new MeshManager(this);
	texmgr	    = new TextureManager(this);

	// Bring Sketchpad Online
	D3D9PadFont::D3D9TechInit(pDevice);
	D3D9PadPen::D3D9TechInit(pDevice);
	D3D9PadBrush::D3D9TechInit(pDevice);
	D3D9Text::D3D9TechInit(this, pDevice);
	D3D9Pad::D3D9TechInit(this, pDevice);

	deffont = (oapi::Font*) new D3D9PadFont(20, true, "fixed");
	defpen  = (oapi::Pen*)  new D3D9PadPen(1, 1, 0x00FF00);

	pDefaultTex = SURFACE(clbkLoadTexture("Null.dds"));
	if (pDefaultTex==NULL) LogErr("Null.dds not found");

	int x=0;
	if (viewW>1282) x=4;

	hLblFont1 = CreateFont(24+x, 0, 0, 0, 700, false, false, 0, 0, 3, 2, 1, 49, "Courier New");
	hLblFont2 = CreateFont(18+x, 0, 0, 0, 700, false, false, 0, 0, 3, 2, 1, 49, "Courier New");

	SplashScreen();  // Warning D3D9ClientSurface is not yet fully initialized here

	ShowWindow(hRenderWnd, SW_SHOW);

	OutputLoadStatus("Building Shader Programs...",0);

	D3D9Effect::D3D9TechInit(this, pDevice, fld);

	// Device-specific initialisations

	TileManager::GlobalInit(this);
	TileManager2Base::GlobalInit(this);
	PlanetRenderer::GlobalInit(this);
	RingManager::GlobalInit(this);
	HazeManager::GlobalInit(this);
	HazeManager2::GlobalInit(this);
	D3D9ParticleStream::GlobalInit(this);
	CSphereManager::GlobalInit(this);
	vStar::GlobalInit(this);
	vObject::GlobalInit(this);
	vVessel::GlobalInit(this);
	OapiExtension::GlobalInit(*Config);

	OutputLoadStatus("SceneTech.fx",1);
	Scene::D3D9TechInit(pDevice, fld);

	// Create scene instance
	scene = new Scene(this, viewW, viewH);

	WriteLog("[D3D9Client Initialized]");
	LogOk("...3D environment initialised");

#ifdef _NVAPI_H
	if (bNVAPI) {
		NvU8 bEnabled = 0;
		NvAPI_Status nvStereo = NvAPI_Stereo_IsEnabled(&bEnabled);

		if (nvStereo!=NVAPI_OK) {
			if (nvStereo==NVAPI_STEREO_NOT_INITIALIZED) LogWrn("Stereo API not initialized");
			if (nvStereo==NVAPI_API_NOT_INITIALIZED) LogErr("nVidia API not initialized");
			if (nvStereo==NVAPI_ERROR) LogErr("nVidia API ERROR");
		}

		if (bEnabled) {
			LogAlw("[nVidia Stereo mode is Enabled]");
			if (NvAPI_Stereo_CreateHandleFromIUnknown(pDevice, &pStereoHandle)!=NVAPI_OK) {
				LogErr("Failed to get StereoHandle");
			}
			else {
				if (pStereoHandle) {
					if (NvAPI_Stereo_SetConvergence(pStereoHandle, float(Config->Convergence))!=NVAPI_OK) {
						LogErr("SetConvergence Failed");
					}
					if (NvAPI_Stereo_SetSeparation(pStereoHandle, float(Config->Separation))!=NVAPI_OK) {
						LogErr("SetSeparation Failed");
					}
				}
			}
		}
		else LogAlw("[nVidia Stereo mode is Disabled]");
	}
#endif

	// Create status queries -----------------------------------------
	//
	if (pDevice->CreateQuery(D3DQUERYTYPE_OCCLUSION, NULL) == S_OK) LogAlw("D3DQUERYTYPE_OCCLUSION is supported by device");
	else LogAlw("D3DQUERYTYPE_OCCLUSION not supported by device");

	if (pDevice->CreateQuery(D3DQUERYTYPE_PIPELINETIMINGS, NULL)==S_OK) LogAlw("D3DQUERYTYPE_PIPELINETIMINGS is supported by device");
	else LogAlw("D3DQUERYTYPE_PIPELINETIMINGS not supported by device");

	if (pDevice->CreateQuery(D3DQUERYTYPE_BANDWIDTHTIMINGS, NULL) == S_OK) LogAlw("D3DQUERYTYPE_BANDWIDTHTIMINGS is supported by device");
	else LogAlw("D3DQUERYTYPE_BANDWIDTHTIMINGS not supported by device");

	if (pDevice->CreateQuery(D3DQUERYTYPE_PIXELTIMINGS, NULL) == S_OK) LogAlw("D3DQUERYTYPE_PIXELTIMINGS is supported by device");
	else LogAlw("D3DQUERYTYPE_PIXELTIMINGS not supported by device");

	return hRenderWnd;
}


// ==============================================================
// This is called when the simulation is ready to go but the clock
// is not yet ticking
//
void D3D9Client::clbkPostCreation()
{
	_TRACE;
	LogAlw("================ clbkPostCreation ===============");

	if (parser) return;

	parser = new FileParser(scenarioName);
	parser->LogContent();

	if (scene) scene->Initialise();

	// Create Window Manager -----------------------------------------
	//
	if (Config->gcGUIMode != 0) {
		pWM = new WindowManager(hRenderWnd, ModuleInstance(), !(GetVideoData()->fullscreen));
		if (pWM->IsOK() == false) SAFE_DELETE(pWM);
	}

	bRunning = true;

	LogAlw("=============== Loading Completed and Visuals Created ================");

#ifdef _DEBUG
	SketchPadTest();
#endif

	WriteLog("[Scene Initialized]");
}

// ==============================================================
// Perform some routine tests with sketchpad
//
void D3D9Client::SketchPadTest()
{
	SURFHANDLE hSrc = clbkLoadSurface("D3D9/SketchpadTest.dds", OAPISURFACE_TEXTURE);
	SURFHANDLE hTgt = clbkLoadSurface("D3D9/SketchpadTest.dds", OAPISURFACE_RENDERTARGET);

	if (!hSrc || !hTgt) return;

	oapiSetSurfaceColourKey(hSrc, 0xFFFFFFFF);

	Sketchpad3 *pSkp = static_cast<Sketchpad3 *>(oapiGetSketchpad(hTgt));

	RECT r = { 17, 1, 31, 15 };
	RECT t = { 1, 33, 31, 63 };
	RECT q = { 0, 16, 16, 32 };
	RECT w = { 49, 1, 63, 15 };

	pSkp->CopyRect(hSrc, &r, 1, 1);
	pSkp->ColorKey(hSrc, &r, 33, 1);
	pSkp->SetBlendState(SKPBS_ALPHABLEND | SKPBS_FILTER_POINT);
	pSkp->StretchRect(hSrc, &r, &t);
	pSkp->SetBlendState();
	pSkp->StretchRect(hSrc, &r, &w);
	pSkp->RotateRect(hSrc, &q, 24, 24, float(PI05));
	pSkp->RotateRect(hSrc, &q, 40, 24, float(PI));
	pSkp->RotateRect(hSrc, &q, 56, 24, float(-PI05));

	RECT tr = { 33, 49, 47, 63 };
	pSkp->ColorFill(FVECTOR4((DWORD)0xFFFF00FF), &tr);

	pSkp->QuickPen(0xFF000000);
	pSkp->QuickBrush(0x80FFFF00);
	pSkp->Rectangle(49, 33, 63, 47); // 1px margin
	pSkp->Ellipse(49, 49, 63, 63);	// 1px margin

									// No Pen, Brush Only
	pSkp->SetPen(NULL);
	pSkp->QuickBrush(0xFF00FF00);
	pSkp->Rectangle(65, 33, 79, 47); // 1px tlm 1px brm
	pSkp->Ellipse(65, 49, 79, 63);	// 1px tlm 1px brm

	pSkp->QuickPen(0xFFFFFFFF);
	pSkp->MoveTo(65, 1);
	pSkp->LineTo(65, 14);
	pSkp->LineTo(78, 14);
	pSkp->LineTo(78, 1);
	pSkp->LineTo(65, 1);

	RECT cr = { 33, 33, 47, 47 };
	pSkp->ClipRect(&cr);
	pSkp->ColorFill(FVECTOR4((DWORD)0xFFFFFF00), NULL);
	pSkp->ClipRect();

	oapiReleaseSketchpad(pSkp);

	clbkSaveSurfaceToImage(hTgt, "SketchpadOutput", ImageFileFormat::IMAGE_PNG);

	oapiReleaseTexture(hSrc);
	oapiReleaseTexture(hTgt);

	 
	// Run Different Kind of Tests
	// 

	hTgt = oapiCreateSurfaceEx(768, 512, OAPISURFACE_RENDERTARGET);

	if (!hTgt) return;

	gcCore *pCore = gcGetCoreInterface();

	if (!pCore) return;

	float a = 0.0f;
	float s = float(PI2 / 6.0);

	gcCore::TriangleVtx Vtx[8];
	oapi::FVECTOR2 Pol[6];

	for (int i = 0; i < 6; i++) {
		Pol[i].x = cos(a);
		Pol[i].y = sin(a);
		Vtx[i + 1].pos.x = Pol[i].x;
		Vtx[i + 1].pos.y = Pol[i].y;
		a += s;
	}

	//				 AABBGGRR
	Vtx[1].color = 0xFFFF0000;
	Vtx[2].color = 0xFFFFFF00;
	Vtx[3].color = 0xFF00FF00;
	Vtx[4].color = 0xFF00FFFF;
	Vtx[5].color = 0xFF0000FF;
	Vtx[6].color = 0xFFFF00FF;

	// Center vertex
	Vtx[0].pos.x = 0.0f;
	Vtx[0].pos.y = 0.0f;
	Vtx[0].color = 0xFFFFFFFF;    // White
	Vtx[7] = Vtx[1];

	HPOLY hColors = pCore->CreateTriangles(NULL, Vtx, 8, PF_FAN);
	HPOLY hOutline = pCore->CreatePoly(NULL, Pol, 6, PF_CONNECT);
	HPOLY hOutline2 = pCore->CreatePoly(NULL, Pol, 6);

	Vtx[0].color = 0xFFFF0000;    
	Vtx[1].color = 0xFFFFFF00;	  
	Vtx[2].color = 0xFFF0FF00;   
	Vtx[3].color = 0xFF00FFFF;	
	Vtx[4].color = 0xFF0000FF;    
	Vtx[5].color = 0xFFFF00FF;

	Vtx[0].pos = FVECTOR2(-1, 0);
	Vtx[1].pos = FVECTOR2(-1, 1);
	Vtx[2].pos = FVECTOR2(0, 0);
	Vtx[3].pos = FVECTOR2(0, 1);
	Vtx[4].pos = FVECTOR2(1, 0);
	Vtx[5].pos = FVECTOR2(1, 1);

	HPOLY hStrip = pCore->CreateTriangles(NULL, Vtx, 6, PF_STRIP);


	Vtx[0].color = 0xFF00FF00;    // Green
	Vtx[1].color = 0xFF00FF00;	  // Green	
	Vtx[2].color = 0xFFFF00FF;    // Mangenta
	Vtx[3].color = 0xFFFF00FF;	  // Mangenta
	Vtx[4].color = 0xFF0000FF;    // Blue
	Vtx[5].color = 0xFF0000FF;	  // Blue

	Vtx[0].pos = FVECTOR2(-1, 1);
	Vtx[1].pos = FVECTOR2(-1, 0);
	Vtx[2].pos = FVECTOR2(0, 1);
	Vtx[3].pos = FVECTOR2(0, 0);
	Vtx[4].pos = FVECTOR2(1, 1);
	Vtx[5].pos = FVECTOR2(1, 0);

	HPOLY hStrip2 = pCore->CreateTriangles(NULL, Vtx, 6, PF_STRIP);


	IVECTOR2 pos0 = { 128, 128 };
	IVECTOR2 pos1 = { 128, 384 };
	IVECTOR2 pos2 = { 384, 128 };
	IVECTOR2 pos3 = { 640, 128 };
	IVECTOR2 pos4 = { 384, 384 };

	pSkp = static_cast<Sketchpad3 *>(oapiGetSketchpad(hTgt));

	pSkp->ColorFill(FVECTOR4((DWORD)0xFFFFFFFF), NULL);

	pSkp->QuickBrush(0xA0000088);
	pSkp->QuickPen(0xA0000000, 3.0f);
	pSkp->PushWorldTransform();

	pSkp->SetWorldScaleTransform2D(&FVECTOR2(100.0f, 100.0f), &pos0);
	pSkp->DrawPoly(hColors);
	pSkp->DrawPoly(hOutline);

	pSkp->SetWorldScaleTransform2D(&FVECTOR2(100.0f, 100.0f), &pos1);
	pSkp->DrawPoly(hOutline2);

	pSkp->SetWorldScaleTransform2D(&FVECTOR2(100.0f, 100.0f), &pos2);
	pSkp->DrawPoly(hStrip);

	pSkp->SetWorldScaleTransform2D(&FVECTOR2(100.0f, 100.0f), &pos3);
	pSkp->DrawPoly(hStrip2);

	pSkp->SetWorldScaleTransform2D(&FVECTOR2(100.0f, 100.0f), &pos4);
	pSkp->QuickPen(0xFF000000, 25.0f);
	pSkp->DrawPoly(hOutline);

	pSkp->PopWorldTransform();
	
	oapiReleaseSketchpad(pSkp);


	pCore->DeletePoly(hColors); // Must release Sketchpad before releasing any sketchpad resources
	pCore->DeletePoly(hOutline);
	pCore->DeletePoly(hOutline2);
	pCore->DeletePoly(hStrip);
	pCore->DeletePoly(hStrip2);

	clbkSaveSurfaceToImage(hTgt, "SketchpadOutput2", ImageFileFormat::IMAGE_PNG);

	oapiReleaseTexture(hTgt);
}




// ==============================================================
// Called when simulation session is about to be closed
//
void D3D9Client::clbkCloseSession(bool fastclose)
{

	LogAlw("================ clbkCloseSession ===============");

	//	Post shutdown signals for gcGUI applications
	//
	for each (gcGUIApp* pApp in g_gcGUIAppList)	pApp->clbkShutdown();


	//	Post shutdown signals for user applications
	//
	if (IsGenericProcEnabled(GENERICPROC_SHUTDOWN)) MakeGenericProcCall(GENERICPROC_SHUTDOWN, 0, NULL);


	// Check the status of RenderTarget Stack ------------------------------------------------
	//
	if (RenderStack.empty() == false) {
		LogErr("RenderStack contains %d items:", RenderStack.size());
		while (!RenderStack.empty()) {
			LogErr("RenderTarget=%s, DepthStencil=%s", _PTR(RenderStack.front().pColor), _PTR(RenderStack.front().pDepthStencil));
			RenderStack.pop_front();
		}
	}

	// Disable rendering and some other systems
	//
	bRunning = false;


	// Face's Cleanup ShapeStore -----------------------
	//
	for (auto store : g_ShapeStore)
	{
		for (auto shape : store.second)
		{
			delete shape;
		}
	}
	g_ShapeStore.clear();
	g_ElevationFlatteningShapes.clear();
	g_ShapesLoaded.clear();

	// At first, shutdown tile loaders -------------------------------------------------------
	//
	if (TileBuffer::ShutDown()==false) LogErr("Failed to Shutdown TileBuffer()");
	if (TileManager2Base::ShutDown()==false) LogErr("Failed to Shutdown TileManager2Base()");

	// Close dialog if Open and disconnect a visual form debug controls
	DebugControls::Close();

	// Disconnect textures from pipeline (Unlikely nesseccary)
	D3D9Effect::ShutDown();

	// DEBUG: List all textures connected to meshes
	/* DWORD cnt = MeshCatalog->CountEntries();
	for (DWORD i=0;i<cnt;i++) {
		D3D9Mesh *x = (D3D9Mesh*)MeshCatalog->Get(i);
		if (x) x->DumpTextures();
	} */
	//GraphicsClient::clbkCloseSession(fastclose);

	SAFE_DELETE(pWM);
	SAFE_DELETE(parser);
	LogAlw("================= Deleting Scene ================");
	Scene::GlobalExit();
	SAFE_DELETE(scene);
	LogAlw("============== Deleting Mesh Manager ============");
	SAFE_DELETE(meshmgr);
	WriteLog("[Session Closed. Scene deleted.]");
	
}

// ==============================================================

void D3D9Client::clbkDestroyRenderWindow (bool fastclose)
{
	_TRACE;
	oapiWriteLog("D3D9: [Destroy Render Window Called]");
	LogAlw("============= clbkDestroyRenderWindow ===========");

#ifdef _NVAPI_H
	if (bNVAPI) {
		if (pStereoHandle) {
			if (NvAPI_Stereo_DestroyHandle(pStereoHandle)!=NVAPI_OK) {
				LogErr("Failed to destroy stereo handle");
			}
		}
	}
#endif

	
	LogAlw("=========== Clearing Texture Repository =========");
	SAFE_DELETE(texmgr);

	LogAlw("===== Calling GlobalExit() for sub-systems ======");
	HazeManager::GlobalExit();
	HazeManager2::GlobalExit();
	TileManager::GlobalExit();
	TileManager2Base::GlobalExit();
	PlanetRenderer::GlobalExit();
	D3D9ParticleStream::GlobalExit();
	CSphereManager::GlobalExit();
	vStar::GlobalExit();
	vVessel::GlobalExit();
	vObject::GlobalExit();

	SAFE_DELETE(defpen);
	SAFE_DELETE(deffont);

	DeleteObject(hLblFont1);
	DeleteObject(hLblFont2);

	D3D9Pad::GlobalExit();
	D3D9Text::GlobalExit();
	D3D9Effect::GlobalExit();
	D3D9ClientSurface::GlobalExit();

	SAFE_RELEASE(pSplashScreen);	// Splash screen related
	SAFE_RELEASE(pTextScreen);		// Splash screen related
	SAFE_RELEASE(pBackBuffer);
	SAFE_RELEASE(pDepthStencil);

	LPD3D9CLIENTSURFACE pBBuf = GetBackBufferHandle();

	SAFE_DELETE(pBBuf);
	SAFE_DELETE(pDefaultTex);
	SAFE_RELEASE(pNoiseTex);

	LogAlw("============ Checking Object Catalogs ===========");

	// Check surface catalog --------------------------------------------------------------------------------------
	//
	auto it = SurfaceCatalog->begin();
	if (it != SurfaceCatalog->end())
	{
		LogErr("UnDeleted Surface(s) Detected");
		while (it != SurfaceCatalog->end())
		{
			LogWrn("Surface %s (%s) (%u,%u)", _PTR(*it), (*it)->GetName(), (*it)->GetWidth(), (*it)->GetHeight());
			delete *it;
			it = SurfaceCatalog->begin();
		}
	}

	// Check tile catalog --------------------------------------------------------------------------------------
	//
	size_t nt = TileCatalog->CountEntries();
	if (nt) LogErr("SurfaceTile catalog contains %lu unreleased entries", nt);

	SurfaceCatalog->Clear();
	MeshCatalog->Clear();
	TileCatalog->Clear();

	pFramework->DestroyObjects();

	SAFE_DELETE(pFramework);

	// Close Render Window -----------------------------------------
	GraphicsClient::clbkDestroyRenderWindow(fastclose);

	hRenderWnd		 = NULL;
	pDevice			 = NULL;
	bFailed			 = false;
	viewW = viewH    = 0;
	viewBPP          = 0;

}

// ==============================================================

void D3D9Client::clbkSurfaceDeleted(LPD3D9CLIENTSURFACE hSurf)
{
	LPDIRECT3DSURFACE9 pSurf = hSurf->GetSurface();
	if (pSurf == NULL) return;

	for each (RenderTgtData data in RenderStack)
	{
		if (data.pColor == pSurf) {
			LogErr("Deleting a surface located in render stack %s", _PTR(hSurf));
		}
	}
}

// ==============================================================

void D3D9Client::PushSketchpad(SURFHANDLE surf, D3D9Pad *pSkp)
{
	if (surf) {
		LPDIRECT3DSURFACE9 pTgt = SURFACE(surf)->GetSurface();
		LPDIRECT3DSURFACE9 pDep = SURFACE(surf)->GetDepthStencil();
		PushRenderTarget(pTgt, pDep, RENDERPASS_SKETCHPAD);
		RenderStack.front().pSkp = pSkp;
	}
}

// ==============================================================

void D3D9Client::PushSketchpadNative(LPDIRECT3DSURFACE9 pColor, LPDIRECT3DSURFACE9 pDepth, D3D9Pad *pSkp)
{
	if (pColor) {
		PushRenderTarget(pColor, pDepth, RENDERPASS_SKETCHPAD);
		RenderStack.front().pSkp = pSkp;
	}
}

// ==============================================================

void D3D9Client::PushRenderTarget(LPDIRECT3DSURFACE9 pColor, LPDIRECT3DSURFACE9 pDepthStencil, int code)
{
	static char *labels[] = { "NULL", "MAIN", "ENV", "CUSTOMCAM", "SHADOWMAP", "PICK", "SKETCHPAD", "OVERLAY" };

	RenderTgtData data;
	data.pColor = pColor;
	data.pDepthStencil = pDepthStencil;
	data.pSkp = NULL;
	data.code = code;

	if (pColor) {
		D3DSURFACE_DESC desc;
		pColor->GetDesc(&desc);
		D3DVIEWPORT9 vp = { 0, 0, desc.Width, desc.Height, 0.0f, 1.0f };
		pDevice->SetViewport(&vp);
	}

	pDevice->SetRenderTarget(0, pColor);
	pDevice->SetDepthStencilSurface(pDepthStencil);

	RenderStack.push_front(data);
	LogDbg("Plum", "PUSH:RenderStack[%lu]={%s, %s} %s", RenderStack.size(), _PTR(data.pColor), _PTR(data.pDepthStencil), labels[data.code]);
}

// ==============================================================

void D3D9Client::AlterRenderTarget(LPDIRECT3DSURFACE9 pColor, LPDIRECT3DSURFACE9 pDepthStencil)
{
	D3DSURFACE_DESC desc;
	pColor->GetDesc(&desc);
	D3DVIEWPORT9 vp = { 0, 0, desc.Width, desc.Height, 0.0f, 1.0f };

	pDevice->SetViewport(&vp);
	pDevice->SetRenderTarget(0, pColor);
	pDevice->SetDepthStencilSurface(pDepthStencil);
}

// ==============================================================

void D3D9Client::PopRenderTargets()
{
	static char *labels[] = { "NULL", "MAIN", "ENV", "CUSTOMCAM", "SHADOWMAP", "PICK", "SKETCHPAD", "OVERLAY" };

	assert(RenderStack.empty() == false);

	RenderStack.pop_front();

	if (RenderStack.empty()) {
		HackFriendlyHack();
		LogDbg("Orange", "POP: Last one out ------------------------------------");
		return;
	}

	RenderTgtData data = RenderStack.front();

	if (data.pColor) {
		D3DSURFACE_DESC desc;
		data.pColor->GetDesc(&desc);
		D3DVIEWPORT9 vp = { 0, 0, desc.Width, desc.Height, 0.0f, 1.0f };

		pDevice->SetViewport(&vp);
		pDevice->SetRenderTarget(0, data.pColor);
		pDevice->SetDepthStencilSurface(data.pDepthStencil);
	}

	LogDbg("Plum", "POP:RenderStack[%lu]={%s, %s, %s} %s", RenderStack.size(), _PTR(data.pColor), _PTR(data.pDepthStencil), _PTR(data.pSkp), labels[data.code]);
}

// ==============================================================

void D3D9Client::HackFriendlyHack()
{
	// Try to make the application more hackable by setting the D3D Device in 'more' expected state.

	D3DSURFACE_DESC desc;
	GetBackBuffer()->GetDesc(&desc);
	D3DVIEWPORT9 vp = { 0, 0, desc.Width, desc.Height, 0.0f, 1.0f };
	pDevice->SetViewport(&vp);
	pDevice->SetRenderTarget(0, GetBackBuffer());
	pDevice->SetDepthStencilSurface(GetDepthStencil());
}

// ==============================================================

LPDIRECT3DSURFACE9 D3D9Client::GetTopDepthStencil()
{
	if (RenderStack.empty()) return NULL;
	return RenderStack.front().pDepthStencil;
}

// ==============================================================

LPDIRECT3DSURFACE9 D3D9Client::GetTopRenderTarget()
{
	if (RenderStack.empty()) return NULL;
	return RenderStack.front().pColor;
}

// ==============================================================

D3D9Pad *D3D9Client::GetTopInterface() const
{
	if (RenderStack.empty()) return NULL;
	return RenderStack.front().pSkp;
}


// ==============================================================

void D3D9Client::clbkUpdate(bool running)
{
	_TRACE;
	double tot_update = D3D9GetTime();
	if (bFailed==false && bRunning) scene->Update();
	D3D9SetTime(D3D9Stats.Timer.Update, tot_update);
}

// ==============================================================

double frame_time = 0.0;
double scene_time = 0.0;

void D3D9Client::clbkRenderScene()
{
	_TRACE;

	if (pDevice==NULL || scene==NULL) return;
	if (bFailed) return;
	if (!bRunning) return;

	if (pWM) pWM->Animate();

	if (Config->PresentLocation == 1) PresentScene();

	scene_time = D3D9GetTime();

	if (pDevice->TestCooperativeLevel()!=S_OK) {
		bFailed=true;
		MessageBoxA(pFramework->GetRenderWindow(),"Connection to Direct3DDevice is lost\nExit the simulation with Ctrl+Q and restart.\n\nAlt-Tabing not supported in a true fullscreen mode.\nDialog windows won't work with multi-sampling in a true fullscreen mode.","D3D9Client: Lost Device",0);
		return;
	}

	if (bHalt) {
		pDevice->BeginScene();
		RECT rect2 = _RECT(0, viewH - 60, viewW, viewH - 20);
		pFramework->GetLargeFont()->DrawTextA(0, "Critical error has occured", 26, &rect2, DT_CENTER | DT_TOP, D3DCOLOR_XRGB(255, 0, 0));
		rect2.left-=4; rect2.top-=4;
		pFramework->GetLargeFont()->DrawTextA(0, "Critical error has occured", 26, &rect2, DT_CENTER | DT_TOP, D3DCOLOR_XRGB(255, 255, 255));
		pDevice->EndScene();
		return;
	}

	UINT mem = pDevice->GetAvailableTextureMem()>>20;
	if (mem<32) TileBuffer::HoldThread(true);

	scene->RenderMainScene();		// Render the main scene

	VESSEL *hVes = oapiGetFocusInterface();

	if (hVes && Config->LabelDisplayFlags)
	{
		char Label[7] = "";
		if (Config->LabelDisplayFlags & D3D9Config::LABEL_DISPLAY_RECORD && hVes->Recording()) strcpy_s(Label, 7, "Record");
		if (Config->LabelDisplayFlags & D3D9Config::LABEL_DISPLAY_REPLAY && hVes->Playback()) strcpy_s(Label, 7, "Replay");

		if (Label[0]!=0) {
			pDevice->BeginScene();
			RECT rect2 = _RECT(0, viewH - 60, viewW, viewH - 20);
			pFramework->GetLargeFont()->DrawTextA(0, Label, 6, &rect2, DT_CENTER | DT_TOP, D3DCOLOR_XRGB(0, 0, 0));
			rect2.left-=4; rect2.top-=4;
			pFramework->GetLargeFont()->DrawTextA(0, Label, 6, &rect2, DT_CENTER | DT_TOP, D3DCOLOR_XRGB(255, 255, 255));
			pDevice->EndScene();
		}
	}

	D3D9SetTime(D3D9Stats.Timer.Scene, scene_time);


	if (bControlPanel) RenderControlPanel();

	// Compute total frame time
	D3D9SetTime(D3D9Stats.Timer.FrameTotal, frame_time);
	frame_time = D3D9GetTime();

	memset(&D3D9Stats.Old, 0, sizeof(D3D9Stats.Old));
	memset(&D3D9Stats.Surf, 0, sizeof(D3D9Stats.Surf));
}

// ==============================================================

void D3D9Client::clbkTimeJump(double simt, double simdt, double mjd)
{
	_TRACE;
	GraphicsClient::clbkTimeJump (simt, simdt, mjd);
}

// ==============================================================

void D3D9Client::PresentScene()
{
	double time = D3D9GetTime();

	if (bFullscreen == false) {
		RenderWithPopupWindows();
		pDevice->Present(0, 0, 0, 0);
	}
	else {
		if (!RenderWithPopupWindows()) pDevice->Present(0, 0, 0, 0);
	}

	D3D9SetTime(D3D9Stats.Timer.Display, time);
}

// ==============================================================

double framer_rater_limit = 0.0;

bool D3D9Client::clbkDisplayFrame()
{
	_TRACE;
//	static int iRefrState = 0;
	double time = D3D9GetTime();

	if (!bRunning) {
		RECT txt = _RECT( loadd_x, loadd_y, loadd_x+loadd_w, loadd_y+loadd_h );
		pDevice->StretchRect(pSplashScreen, NULL, pBackBuffer, NULL, D3DTEXF_POINT);
		pDevice->StretchRect(pTextScreen, NULL, pBackBuffer, &txt, D3DTEXF_POINT);
	}

	if (Config->PresentLocation == 0) PresentScene();

	double frmt = (1000000.0/Config->FrameRate) - (time - framer_rater_limit);

	framer_rater_limit = time;

	if (Config->EnableLimiter && Config->FrameRate>0 && bVSync==false) {
		if (frmt>0) frame_timer++;
		else        frame_timer--;
		if (frame_timer>40) frame_timer=40;
		Sleep(frame_timer);
	}

	return true;
}

// ==============================================================

void D3D9Client::clbkPreOpenPopup ()
{
	_TRACE;
	GetDevice()->SetDialogBoxMode(true);
}

// =======================================================================

static DWORD g_lastPopupWindowCount = 0;
static void FixOutOfScreenPositions (const HWND *hWnd, DWORD count)
{
	// Only check if a popup window is *added*
	if (count > g_lastPopupWindowCount)
	{
		for (DWORD i=0; i<count; ++i)
		{
			RECT rect;
			GetWindowRect(hWnd[i], &rect);

			int x = -1, y, w, h; // x != -1 indicates "position change needed"
			if (rect.left < 0) {
				x = 0;
				y = rect.top;
			}
			if (rect.top  < 0) {
				x = rect.left;
				y = 0;
			}

			// For the rest we need monitor information...
			HMONITOR monitor = MonitorFromWindow(hWnd[i], MONITOR_DEFAULTTONEAREST);
			MONITORINFO info;
			info.cbSize = sizeof(MONITORINFO);
			GetMonitorInfo(monitor, &info);

			int monitorWidth = info.rcMonitor.right - info.rcMonitor.left; // info.rcWork....
			int monitorHeight = info.rcMonitor.bottom - info.rcMonitor.top;

			if (rect.right > monitorWidth) {
				x = monitorWidth - (rect.right - rect.left);
				y = rect.top;
			}
			if (rect.bottom > monitorHeight) {
				x = rect.left;
				y = monitorHeight - (rect.bottom - rect.top);
			}

			if (x != -1) {
				w = rect.right - rect.left,
				h = rect.bottom - rect.top;
				MoveWindow(hWnd[i], x, y, w, h, FALSE);
			}
		}

	}
	g_lastPopupWindowCount = count;
}

// =======================================================================

bool D3D9Client::RenderWithPopupWindows()
{
	_TRACE;

	const HWND *hPopupWnd;
	DWORD count = GetPopupList(&hPopupWnd);

	if (bFullscreen) {
		if (count) GetDevice()->SetDialogBoxMode(true);
		else       GetDevice()->SetDialogBoxMode(false);
	}

	// Let the OapiExtension manager know about this..
	OapiExtension::HandlePopupWindows(hPopupWnd, count);

	FixOutOfScreenPositions(hPopupWnd, count);

	if (!bFullscreen) {
		for (DWORD i=0;i<count;i++) {
			DWORD val = GetWindowLongA(hPopupWnd[i], GWL_STYLE);
			if ((val&WS_SYSMENU)==0) {
				SetWindowLongA(hPopupWnd[i], GWL_STYLE, val|WS_SYSMENU);
			}
		}
	}

	return false;
}

#pragma region Particle stream functions

// =======================================================================
// Particle stream functions
// ==============================================================

ParticleStream *D3D9Client::clbkCreateParticleStream(PARTICLESTREAMSPEC *pss)
{
	LogErr("UnImplemented Feature Used clbkCreateParticleStream");
	return NULL;
}

// =======================================================================

ParticleStream *D3D9Client::clbkCreateExhaustStream(PARTICLESTREAMSPEC *pss,
	OBJHANDLE hVessel, const double *lvl, const VECTOR3 *ref, const VECTOR3 *dir)
{
	_TRACE;
	ExhaustStream *es = new ExhaustStream (this, hVessel, lvl, ref, dir, pss);
	scene->AddParticleStream (es);
	return es;
}

// =======================================================================

ParticleStream *D3D9Client::clbkCreateExhaustStream(PARTICLESTREAMSPEC *pss,
	OBJHANDLE hVessel, const double *lvl, const VECTOR3 &ref, const VECTOR3 &dir)
{
	_TRACE;
	ExhaustStream *es = new ExhaustStream (this, hVessel, lvl, ref, dir, pss);
	scene->AddParticleStream (es);
	return es;
}

// ======================================================================

ParticleStream *D3D9Client::clbkCreateReentryStream (PARTICLESTREAMSPEC *pss,
	OBJHANDLE hVessel)
{
	_TRACE;
	ReentryStream *rs = new ReentryStream (this, hVessel, pss);
	scene->AddParticleStream (rs);
	return rs;
}

#pragma endregion

// ==============================================================

ScreenAnnotation* D3D9Client::clbkCreateAnnotation()
{
	_TRACE;
	return GraphicsClient::clbkCreateAnnotation();
}

#pragma region Mesh functions

// ==============================================================

void D3D9Client::clbkStoreMeshPersistent(MESHHANDLE hMesh, const char *fname)
{
	_TRACE;
	if (ChkDev(__FUNCTION__)) return;

	if (fname) {
		LogAlw("Storing a mesh %s (%s)", _PTR(hMesh), fname);
		if (hMesh==NULL) LogErr("D3D9Client::clbkStoreMeshPersistent(%s) hMesh is NULL",fname);
	}
	else {
		LogAlw("Storing a mesh %s", _PTR(hMesh));
		if (hMesh==NULL) LogErr("D3D9Client::clbkStoreMeshPersistent() hMesh is NULL");
	}

	if (hMesh==NULL) return;

	int idx = meshmgr->StoreMesh(hMesh, fname);

	if (idx>=0) {
		if (fname) LogWrn("MeshGroup(%d) in a mesh %s is larger than 1km",idx,fname);
		else	   LogWrn("MeshGroup(%d) in a mesh %s is larger than 1km",idx,_PTR(hMesh));
	}
}

// ==============================================================

DEVMESHHANDLE D3D9Client::GetDevMesh(MESHHANDLE hMesh)
{
	const D3D9Mesh *pDevMesh = meshmgr->GetMesh(hMesh);
	if (!pDevMesh) {
		meshmgr->StoreMesh(hMesh, "GetDevMesh()");
		pDevMesh = meshmgr->GetMesh(hMesh);
	}

	// Create a new Instance from a template
	return DEVMESHHANDLE(new D3D9Mesh(hMesh, *pDevMesh));
}

// ==============================================================

bool D3D9Client::clbkSetMeshTexture(DEVMESHHANDLE hMesh, DWORD texidx, SURFHANDLE surf)
{
	_TRACE;
	if (hMesh && surf) return ((D3D9Mesh*)hMesh)->SetTexture(texidx, SURFACE(surf));
	return false;
}

// ==============================================================

int D3D9Client::clbkSetMeshMaterial(DEVMESHHANDLE hMesh, DWORD matidx, const MATERIAL *mat)
{
	_TRACE;
	D3D9Mesh *mesh = (D3D9Mesh*)hMesh;
	DWORD nmat = mesh->GetMaterialCount();
	if (matidx >= nmat) return 4; // "index out of range"
	D3D9MatExt meshmat;
	//mesh->GetMaterial(&meshmat, matidx);
	CreateMatExt((const D3DMATERIAL9 *)mat, &meshmat);
	mesh->SetMaterial(&meshmat, matidx);
	return 0;
}

// ==============================================================

int D3D9Client::clbkMeshMaterial (DEVMESHHANDLE hMesh, DWORD matidx, MATERIAL *mat)
{
	_TRACE;
	D3D9Mesh *mesh = (D3D9Mesh*)hMesh;
	DWORD nmat = mesh->GetMaterialCount();
	if (matidx >= nmat) return 4; // "index out of range"
	const D3D9MatExt *meshmat = mesh->GetMaterial(matidx);
	if (meshmat) GetMatExt(meshmat, (D3DMATERIAL9 *)mat);
	return 0;
}

// ==============================================================

bool D3D9Client::clbkSetMeshProperty(DEVMESHHANDLE hMesh, DWORD prop, DWORD value)
{
	_TRACE;
	D3D9Mesh *mesh = (D3D9Mesh*)hMesh;
	switch (prop) {
		case MESHPROPERTY_MODULATEMATALPHA:
			mesh->EnableMatAlpha(value!=0);
			return true;
	}
	return false;
}

// ==============================================================
// Returns a dev-mesh for a visual

MESHHANDLE D3D9Client::clbkGetMesh(VISHANDLE vis, UINT idx)
{
	_TRACE;
	if (vis==NULL) {
		LogErr("NULL visual in clbkGetMesh(NULL,%u)",idx);
		return NULL;
	}
	MESHHANDLE hMesh = ((vObject*)vis)->GetMesh(idx);
	if (hMesh==NULL) LogWrn("clbkGetMesh() returns NULL");
	return hMesh;
}

// =======================================================================

int D3D9Client::clbkEditMeshGroup(DEVMESHHANDLE hMesh, DWORD grpidx, GROUPEDITSPEC *ges)
{
	_TRACE;
	return ((D3D9Mesh*)hMesh)->EditGroup(grpidx, ges);
}

int D3D9Client::clbkGetMeshGroup (DEVMESHHANDLE hMesh, DWORD grpidx, GROUPREQUESTSPEC *grs)
{
	_TRACE;
	return ((D3D9Mesh*)hMesh)->GetGroup (grpidx, grs);
}

#pragma endregion

// ==============================================================

void D3D9Client::clbkNewVessel(OBJHANDLE hVessel)
{
	_TRACE;
	if (scene) scene->NewVessel(hVessel);
}

// ==============================================================

void D3D9Client::clbkDeleteVessel(OBJHANDLE hVessel)
{
	if (scene) scene->DeleteVessel(hVessel);
}


// ==============================================================
// copy video options from the video tab

void D3D9Client::clbkRefreshVideoData()
{
	_TRACE;
	if (vtab) vtab->UpdateConfigData();
}

// ==============================================================

bool D3D9Client::clbkUseLaunchpadVideoTab() const
{
	_TRACE;
	return true;
}

// ==============================================================
// Fullscreen mode flag

bool D3D9Client::clbkFullscreenMode() const
{
	_TRACE;
	return bFullscreen;
}

// ==============================================================
// return the dimensions of the render viewport

void D3D9Client::clbkGetViewportSize(DWORD *width, DWORD *height) const
{
	_TRACE;
	*width = viewW, *height = viewH;
}

// ==============================================================
// Returns a specific render parameter

bool D3D9Client::clbkGetRenderParam(DWORD prm, DWORD *value) const
{
	_TRACE;
	switch (prm) {
		case RP_COLOURDEPTH:
			*value = viewBPP;
			return true;

		case RP_ZBUFFERDEPTH:
			*value = GetFramework()->GetZBufferBitDepth();
			return true;

		case RP_STENCILDEPTH:
			*value = GetFramework()->GetStencilBitDepth();
			return true;

		case RP_MAXLIGHTS:
			*value = MAX_SCENE_LIGHTS;
			return true;

		case RP_REQUIRETEXPOW2:
			*value = 0;
			return true;
	}
	return false;
}

// ==============================================================
// Responds to visual events

int D3D9Client::clbkVisEvent(OBJHANDLE hObj, VISHANDLE vis, DWORD msg, DWORD_PTR context)
{
	_TRACE;
	VisObject *vo = (VisObject*)vis;
	vo->clbkEvent(msg, context);
	if (DebugControls::IsActive()) {
		if (msg==EVENT_VESSEL_INSMESH || msg==EVENT_VESSEL_DELMESH) {
			if (DebugControls::GetVisual()==vo) DebugControls::UpdateVisual();
		}
	}
	return 1;
}


// ==============================================================
//

void D3D9Client::EmergencyShutdown()
{
	bool b = oapiSaveScenario("D3D9ClientRescue","Contains the current simulation state before shutdown due to an unexpected error");
	if (b) LogAlw("ORBITER SCENARIO SAVED SUCCESSFULLY (D3D9ClientRescue.scn)");
	else   LogErr("FAILED TO SAVE A SCENARIO");
	bHalt = true;
}


// ==============================================================
//
void D3D9Client::PickTerrain(DWORD uMsg, int xpos, int ypos)
{
	bool bUD = (uMsg == WM_LBUTTONUP || uMsg == WM_RBUTTONUP || uMsg == WM_LBUTTONDOWN || uMsg == WM_RBUTTONDOWN);
	bool bPrs = IsGenericProcEnabled(GENERICPROC_PICK_TERRAIN) && bUD;
	bool bHov = IsGenericProcEnabled(GENERICPROC_HOVER_TERRAIN) && (uMsg == WM_MOUSEMOVE || uMsg == WM_MOUSEWHEEL);

	if (bPrs || bHov) {
		PickGround pg = ScanScreen(xpos, ypos);
		pg.msg = uMsg;
		if (bPrs) MakeGenericProcCall(GENERICPROC_PICK_TERRAIN, sizeof(PickGround), &pg);
		if (bHov) MakeGenericProcCall(GENERICPROC_HOVER_TERRAIN, sizeof(PickGround), &pg);
	}
}


// ==============================================================
// Message handler for render window

LRESULT D3D9Client::RenderWndProc (HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	static bool bTrackMouse = false;
	static short xpos=0, ypos=0;

	D3D9Pick pick;

	if (hRenderWnd!=hWnd && uMsg!= WM_NCDESTROY) {
		LogErr("Invalid Window !! RenderWndProc() called after calling clbkDestroyRenderWindow() uMsg=0x%X", uMsg);
		return 0;
	}

	if (bRunning && DebugControls::IsActive()) {
		// Must update camera to correspond MAIN_SCENE due to Pick() function,
		// because env-maps have altered camera settings
		// GetScene()->UpdateCameraFromOrbiter(RENDERPASS_PICKSCENE);
		// Obsolete: since moving env/cam stuff in pre-scene
	}

	if (pWM) if (pWM->MainWindowProc(hWnd, uMsg, wParam, lParam)) return 0;


	switch (uMsg)
	{
		case WM_MOUSELEAVE:
		{
			if (bTrackMouse && bRunning) GraphicsClient::RenderWndProc (hWnd, WM_LBUTTONUP, 0, 0);
			return 0;
		}

		case WM_MBUTTONDOWN:
		{
			break;
		}

		case WM_RBUTTONUP:
		case WM_RBUTTONDOWN:
		{
			int xp = GET_X_LPARAM(lParam);
			int yp = GET_Y_LPARAM(lParam);
			PickTerrain(uMsg, xp, yp);
			break;
		}


		case WM_LBUTTONDOWN:
		{
			bTrackMouse = true;
			xpos = GET_X_LPARAM(lParam);
			ypos = GET_Y_LPARAM(lParam);

			TRACKMOUSEEVENT te; te.cbSize = sizeof(TRACKMOUSEEVENT); te.dwFlags = TME_LEAVE; te.hwndTrack = hRenderWnd;
			TrackMouseEvent(&te);

			bool bShift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
			bool bCtrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
			bool bPckVsl = IsGenericProcEnabled(GENERICPROC_PICK_VESSEL);

			if (DebugControls::IsActive() || bPckVsl || (bShift && bCtrl)) {
				pick = GetScene()->PickScene(xpos, ypos);
				if (bPckVsl) {
					PickData out;
					out.hVessel = pick.vObj->GetObjectA();
					out.mesh = MESHHANDLE(pick.pMesh);
					out.group = pick.group;
					out.pos = _FV(pick.pos);
					out.normal = _FV(pick.normal);
					out.dist = pick.dist;
					MakeGenericProcCall(GENERICPROC_PICK_VESSEL, sizeof(PickData), &out);
				}
			}

			PickTerrain(uMsg, xpos, ypos);

			// No Debug Controls
			if (bShift && bCtrl && !DebugControls::IsActive() && !oapiCameraInternal()) {

				if (!pick.pMesh) break;

				OBJHANDLE hObj = pick.vObj->Object();
				if (oapiGetObjectType(hObj) == OBJTP_VESSEL) {
					oapiSetFocusObject(hObj);
				}

				break;
			}

			// With Debug Controls
			if (DebugControls::IsActive()) {

				DWORD flags = *(DWORD*)GetConfigParam(CFGPRM_GETDEBUGFLAGS);

				if (flags&DBG_FLAGS_PICK) {

					if (!pick.pMesh) break;

					if (bShift && bCtrl) {
						OBJHANDLE hObj = pick.vObj->Object();
						if (oapiGetObjectType(hObj)==OBJTP_VESSEL) {
							oapiSetFocusObject(hObj);
							break;
						}
					}
					else if (pick.group>=0) {
						DebugControls::SetVisual(pick.vObj);
						DebugControls::SelectMesh(pick.pMesh);
						DebugControls::SelectGroup(pick.group);
						DebugControls::SetGroupHighlight(true);
						DebugControls::SetPickPos(pick.pos);
					}
				}
			}

			break;
		}

		case WM_LBUTTONUP:
		{
			int xp = GET_X_LPARAM(lParam);
			int yp = GET_Y_LPARAM(lParam);

			PickTerrain(uMsg, xp, yp);

			if (DebugControls::IsActive()) {
				DWORD flags = *(DWORD*)GetConfigParam(CFGPRM_GETDEBUGFLAGS);
				if (flags&DBG_FLAGS_PICK) {
					DebugControls::SetGroupHighlight(false);
				}
			}
			bTrackMouse = false;
			break;
		}

		case WM_KEYDOWN:
		{
			if (DebugControls::IsActive()) {
				if (wParam == 'F') {
					if (bFreeze) bFreezeEnable = bFreeze = false;
					else bFreezeEnable = true;
				}
			}
			bool bShift = (GetAsyncKeyState(VK_SHIFT) & 0x8000)!=0;
			bool bCtrl  = (GetAsyncKeyState(VK_CONTROL) & 0x8000)!=0;
			if (wParam == 'C' && bShift && bCtrl) bControlPanel = !bControlPanel;
			if (wParam == 'N' && bShift && bCtrl) Config->bCloudNormals = !Config->bCloudNormals;
			break;
		}

		case WM_MOUSEWHEEL:
		{
			if (DebugControls::IsActive()) {
				short d = GET_WHEEL_DELTA_WPARAM(wParam);
				if (d<-1) d=-1;
				if (d>1) d=1;
				double speed = *(double *)GetConfigParam(CFGPRM_GETCAMERASPEED);
				speed *= (DebugControls::GetVisualSize()/100.0);
				if (scene->CameraPan(_V(0,0,double(d))*2.0, speed)) return 0;
			}

			PickTerrain(uMsg, xpos, ypos);
			break;
		}

		case WM_MOUSEMOVE:

			if (DebugControls::IsActive())
			{

				double x = double(GET_X_LPARAM(lParam) - xpos);
				double y = double(GET_Y_LPARAM(lParam) - ypos);
				xpos = GET_X_LPARAM(lParam);
				ypos = GET_Y_LPARAM(lParam);

				if (bTrackMouse) {
					double speed = *(double *)GetConfigParam(CFGPRM_GETCAMERASPEED);
					speed *= (DebugControls::GetVisualSize() / 100.0);
					if (scene->CameraPan(_V(-x, y, 0)*0.05, speed)) return 0;
				}
			}

			xpos = GET_X_LPARAM(lParam);
			ypos = GET_Y_LPARAM(lParam);

			PickTerrain(uMsg, xpos, ypos);

			break;

		case WM_MOVE:
			// If in windowed mode, move the Framework's window
			break;

		case WM_SYSCOMMAND:
			switch (wParam) {
				case SC_KEYMENU:
					// trap Alt system keys
					return 1;
				case SC_MOVE:
				case SC_SIZE:
				case SC_MAXIMIZE:
				case SC_MONITORPOWER:
					// Prevent moving/sizing and power loss in fullscreen mode
					if (bFullscreen) return 1;
					break;
			}
			break;

		case WM_SYSKEYUP:
			if (bFullscreen) return 0;  // trap Alt-key
			break;
	}

	if (!bRunning && uMsg>=0x0200 && uMsg<=0x020E) return 0;
	return GraphicsClient::RenderWndProc (hWnd, uMsg, wParam, lParam);
}


// ==============================================================
// Message handler for Launchpad "video" tab

INT_PTR D3D9Client::LaunchpadVideoWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	_TRACE;
	if (vtab) return vtab->WndProc(hWnd, uMsg, wParam, lParam);
	else return false;
}

// =======================================================================

void D3D9Client::clbkRender2DPanel (SURFHANDLE *hSurf, MESHHANDLE hMesh, MATRIX3 *T, float alpha, bool additive)
{
	_TRACE;

	SURFHANDLE surf = NULL;
	DWORD ngrp = oapiMeshGroupCount(hMesh);

	if (ngrp==0) return;

	float sx = 1.0f/(float)(T->m11),  dx = (float)(T->m13);
	float sy = 1.0f/(float)(T->m22),  dy = (float)(T->m23);
	float vw = (float)viewW;
	float vh = (float)viewH;

	D3DXMATRIX mVP;
	D3DXMatrixOrthoOffCenterRH(&mVP, (0.0f-dx)*sx, (vw-dx)*sx, (vh-dy)*sy, (0.0f-dy)*sy, -100.0f, 100.0f);
	D3D9Effect::SetViewProjMatrix(&mVP);

	for (DWORD i=0;i<ngrp;i++) {

		float scale = 1.0f;

		MESHGROUP *gr = oapiMeshGroup(hMesh, i);

		if (gr->UsrFlag & 2) continue; // skip this group

		DWORD TexIdx = gr->TexIdx;

		if (TexIdx >= TEXIDX_MFD0) {
			int mfdidx = TexIdx - TEXIDX_MFD0;
			surf = GetMFDSurface(mfdidx);
			if (!surf) surf = (SURFHANDLE)pDefaultTex;
		} else if (hSurf) {
			surf = hSurf[TexIdx];
		}
		else surf = oapiGetTextureHandle (hMesh, gr->TexIdx+1);

		for (unsigned int k=0;k<gr->nVtx;k++) gr->Vtx[k].z = 0.0f;

		D3D9Effect::Render2DPanel(gr, SURFACE(surf), &ident, alpha, scale, additive);
	}
}

// =======================================================================

void D3D9Client::clbkRender2DPanel (SURFHANDLE *hSurf, MESHHANDLE hMesh, MATRIX3 *T, bool additive)
{
	_TRACE;
	clbkRender2DPanel (hSurf, hMesh, T, 1.0f, additive);
}

// =======================================================================

DWORD D3D9Client::clbkGetDeviceColour (BYTE r, BYTE g, BYTE b)
{
	_TRACE;
	return ((DWORD)r << 16) + ((DWORD)g << 8) + (DWORD)b;
}



#pragma region Surface, Blitting and Filling Functions



// =======================================================================
// Surface functions
// =======================================================================

bool D3D9Client::clbkSaveSurfaceToImage(SURFHANDLE  surf,  const char *fname, ImageFileFormat  fmt, float quality)
{
	_TRACE;
	if (ChkDev(__FUNCTION__)) return false;

	if (surf==NULL) surf = pFramework->GetBackBufferHandle();

	LPDIRECT3DSURFACE9 pRTG = NULL;
	LPDIRECT3DSURFACE9 pSystem = NULL;
	LPDIRECT3DSURFACE9 pSurf = SURFACE(surf)->pSurf;

	if (pSurf==NULL) return false;

	bool bRet = false;
	ImageData ID;
	D3DSURFACE_DESC desc;
	D3DLOCKED_RECT pRect;

	SURFACE(surf)->GetDesc(&desc);

	if (desc.Pool!=D3DPOOL_SYSTEMMEM) {

		HR(pDevice->CreateRenderTarget(desc.Width, desc.Height, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, false, &pRTG, NULL));
		HR(pDevice->CreateOffscreenPlainSurface(desc.Width, desc.Height, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &pSystem, NULL));
		HR(pDevice->StretchRect(pSurf, NULL, pRTG, NULL, D3DTEXF_NONE));
		HR(pDevice->GetRenderTargetData(pRTG, pSystem));

		if (pSystem->LockRect(&pRect, NULL, 0)==S_OK) {

			ID.bpp = 24;
			ID.height = desc.Height;
			ID.width = desc.Width;
			ID.stride = ((ID.width * ID.bpp + 31) & ~31) >> 3;
			ID.bufsize = ID.stride * ID.height;

			BYTE *tgt = ID.data = new BYTE[ID.bufsize];
			BYTE *src = (BYTE *)pRect.pBits;

			for (DWORD k=0;k<desc.Height;k++) {
				for (DWORD i=0;i<desc.Width;i++) {
					tgt[0+i*3] = src[0+i*4];
					tgt[1+i*3] = src[1+i*4];
					tgt[2+i*3] = src[2+i*4];
				}
				tgt += ID.stride;
				src += pRect.Pitch;
			}

			bRet = WriteImageDataToFile(ID, fname, fmt, quality);

			delete []ID.data;
			pSystem->UnlockRect();
		}

		pRTG->Release();
		pSystem->Release();
		return bRet;
	}

	if (pSurf->LockRect(&pRect, NULL, D3DLOCK_READONLY)==S_OK) {

		ID.bpp = 24;
		ID.height = desc.Height;
		ID.width = desc.Width;
		ID.stride = ((ID.width * ID.bpp + 31) & ~31) >> 3;
		ID.bufsize = ID.stride * ID.height;

		BYTE *tgt = ID.data = new BYTE[ID.bufsize];
		BYTE *src = (BYTE *)pRect.pBits;

		for (DWORD k=0;k<desc.Height;k++) {
			for (DWORD i=0;i<desc.Width;i++) {
				tgt[0+i*3] = src[0+i*4];
				tgt[1+i*3] = src[1+i*4];
				tgt[2+i*3] = src[2+i*4];
			}
			tgt += ID.stride;
			src += pRect.Pitch;
		}

		bRet = WriteImageDataToFile(ID, fname, fmt, quality);

		delete []ID.data;
		pSurf->UnlockRect();
		return bRet;
	}

	return false;
}

// ==============================================================

SURFHANDLE D3D9Client::clbkLoadTexture(const char *fname, DWORD flags)
{
	_TRACE;
	if (ChkDev(__FUNCTION__)) return NULL;

	LPD3D9CLIENTSURFACE pTex = NULL;

	char path[MAX_PATH];

	if (TexturePath(fname, path)==false) {
		LogWrn("Texture %s not found.", fname);
		return NULL;
	}

	if (flags & 8) {
		if (texmgr->GetTexture(fname, &pTex, flags)==false) return NULL;
	}
	else {
		if (texmgr->LoadTexture(fname, &pTex, flags)!=S_OK) return NULL;
	}

	return pTex;
}

// ==============================================================

SURFHANDLE D3D9Client::clbkLoadSurface (const char *fname, DWORD attrib)
{
	_TRACE;
	if (ChkDev(__FUNCTION__)) return NULL;

	DWORD flags = 0;

	// Process flag conflicts and issues ---------------------------------------------------------------------------------
	//
	flags = OAPISURFACE_RENDERTARGET|OAPISURFACE_SYSMEM;

	if ((attrib&flags)==flags) {
		LogErr("clbkLoadSurface() Can not combine OAPISURFACE_RENDERTARGET | OAPISURFACE_SYSMEM");
		attrib-=OAPISURFACE_SYSMEM;
	}

	if (attrib==OAPISURFACE_SKETCHPAD) {
		attrib |= OAPISURFACE_RENDERTARGET;
		attrib &= ~(OAPISURFACE_SYSMEM|OAPISURFACE_GDI);
	}

	D3D9ClientSurface *surf = new D3D9ClientSurface(pDevice, fname);
	if (surf->LoadSurface(fname, attrib)) return surf;
	delete surf;
	return NULL;
}

// ==============================================================

HBITMAP D3D9Client::gcReadImageFromFile(const char *_path)
{
	char path[MAX_PATH];
	sprintf_s(path, sizeof(path), "%s\\%s", OapiExtension::GetTextureDir(), _path);
	return ReadImageFromFile(path);
}

// ==============================================================

void D3D9Client::clbkReleaseTexture(SURFHANDLE hTex)
{
	_TRACE;
	if (ChkDev(__FUNCTION__)) return;

	if (texmgr->IsInRepository(hTex)) return;	// Do not release surfaces stored in repository

	if (SURFACE(hTex)->Release()) {
		for (auto it = MeshCatalog->cbegin(); it != MeshCatalog->cend(); ++it) {
			if (*it && (*it)->HasTexture(hTex)) {
				LogErr( "Something is attempting to delete a texture (%s) that is currently used by a mesh. Attempt rejected to prevent a CTD",
						(*it)->GetName() );
				return;
			}
		}
		delete SURFACE(hTex);
	}
}

// ==============================================================

SURFHANDLE D3D9Client::clbkCreateSurfaceEx(DWORD w, DWORD h, DWORD attrib)
{
	_TRACE;
	if (ChkDev(__FUNCTION__)) return NULL;

#ifdef _DEBUG
	LogAttribs(attrib, w, h, "CreateSrfEx");
#endif // _DEBUG

	if (w == 0 || h == 0) return NULL;	// Inline engine returns NULL for a zero surface

	const DWORD noalpha_sketchpad_gdi = OAPISURFACE_NOALPHA | OAPISURFACE_SKETCHPAD | OAPISURFACE_GDI;
	const DWORD gdi_texture = OAPISURFACE_GDI | OAPISURFACE_TEXTURE;

	if (attrib == noalpha_sketchpad_gdi) attrib = gdi_texture;


	// VC MFD mipmaps hack ---
	bool bSize = false;
	if (w==h) if (w == 256 || w == 512 || w == 1024) bSize = true;
	if ((attrib&OAPISURFACE_TEXTURE) && !(attrib&OAPISURFACE_GDI) && bSize) attrib |= OAPISURFACE_MIPMAPS;
	// --- end hack ---


	D3D9ClientSurface *surf = new D3D9ClientSurface(pDevice, "clbkCreateSurfaceEx");
	surf->CreateSurface(w, h, attrib);
	return surf;
}


// =======================================================================

SURFHANDLE D3D9Client::clbkCreateSurface(DWORD w, DWORD h, SURFHANDLE hTemplate)
{
	_TRACE;
	if (ChkDev(__FUNCTION__)) return NULL;

	if (w == 0 || h == 0) return NULL;	// Inline engine returns NULL for a zero surface
	D3D9ClientSurface *surf = new D3D9ClientSurface(pDevice, "clbkCreateSurface");
	surf->MakeEmptySurfaceEx(w, h);
	return surf;
}

// =======================================================================

SURFHANDLE D3D9Client::clbkCreateSurface(HBITMAP hBmp)
{
	_TRACE;
	if (ChkDev(__FUNCTION__)) return NULL;

	SURFHANDLE hSurf = GraphicsClient::clbkCreateSurface(hBmp);
	return hSurf;
}

// =======================================================================

SURFHANDLE D3D9Client::clbkCreateTexture(DWORD w, DWORD h)
{
	_TRACE;
	if (ChkDev(__FUNCTION__)) return NULL;

	if (w == 0 || h == 0) return NULL;	// Inline engine returns NULL for a zero surface
	D3D9ClientSurface *pSurf = new D3D9ClientSurface(pDevice, "clbkCreateTexture");
	// DO NOT USE ALPHA
	pSurf->MakeEmptyTextureEx(w, h);
	return (SURFHANDLE)pSurf;
}

// =======================================================================

void D3D9Client::clbkIncrSurfaceRef(SURFHANDLE surf)
{
	_TRACE;
	if (surf==NULL) { LogErr("D3D9Client::clbkIncrSurfaceRef() Input Surface is NULL");	return; }
	SURFACE(surf)->IncRef();
}

// =======================================================================

bool D3D9Client::clbkReleaseSurface(SURFHANDLE surf)
{
	_TRACE;
	if (ChkDev(__FUNCTION__)) return NULL;

	if (surf==NULL) { LogErr("D3D9Client::clbkReleaseSurface() Input Surface is NULL");	return false; }

	if (texmgr->IsInRepository(surf)) return false;	// Do not release surfaces stored in repository

	bool bRel = SURFACE(surf)->Release();

	if (bRel) {

		for (auto it = MeshCatalog->cbegin(); it != MeshCatalog->cend(); ++it) {
			if (*it && (*it)->HasTexture(surf)) {
				LogErr( "Orbiter is attempting to delete a texture (%s) that is currently used by a mesh. Attempt rejected to prevent a CTD",
						(*it)->GetName() );
				return true;
			}
		}
		delete SURFACE(surf);
	}
	return bRel;
}

// =======================================================================

bool D3D9Client::clbkGetSurfaceSize(SURFHANDLE surf, DWORD *w, DWORD *h)
{
	_TRACE;
	if (surf==NULL) surf = pFramework->GetBackBufferHandle();
	*w = SURFACE(surf)->GetWidth();
	*h = SURFACE(surf)->GetHeight();
	return true;
}

// =======================================================================

bool D3D9Client::clbkSetSurfaceColourKey(SURFHANDLE surf, DWORD ckey)
{
	_TRACE;
	if (surf==NULL) { LogErr("Surface is NULL"); return false; }
	SURFACE(surf)->SetColorKey(ckey);
	return true;
}



// =======================================================================
// Blitting functions
// =======================================================================

int D3D9Client::clbkBeginBltGroup(SURFHANDLE tgt)
{
	_TRACE;
	if (pBltGrpTgt) return -1;

	if (tgt==RENDERTGT_NONE) {
		pBltGrpTgt = NULL;
		return -2;
	}

	if (tgt==RENDERTGT_MAINWINDOW) pBltGrpTgt = pFramework->GetBackBufferHandle();
	else						   pBltGrpTgt = tgt;

	if (SURFACE(pBltGrpTgt)->IsRenderTarget()==false) {
		pBltGrpTgt = NULL;
		return -3;
	}

	return 0;
}

int D3D9Client::clbkEndBltGroup()
{
	_TRACE;
	if (pBltGrpTgt==NULL) return -2;
	SURFACE(pBltGrpTgt)->EndBlitGroup(); // Flush queue and release GPU
	pBltGrpTgt = NULL;
	return 0;
}


bool D3D9Client::CheckBltGroup(SURFHANDLE src, SURFHANDLE tgt) const
{
	if (tgt==pBltGrpTgt) {
		if (SURFACE(src)->pTex!=NULL) {
			if (SURFACE(src)->ColorKey) { // Use blit groups only for color keyed source surfaces
				if (SURFACE(pBltGrpTgt)->BeginBlitGroup()==S_OK) return true;
			}
		}
	}
	// Flush queue, release GPU and disable blit group
	SURFACE(pBltGrpTgt)->EndBlitGroup();
	pBltGrpTgt = NULL;
	return false;
}



bool D3D9Client::clbkBlt(SURFHANDLE tgt, DWORD tgtx, DWORD tgty, SURFHANDLE src, DWORD flag) const
{
	_TRACE;

	double time = D3D9GetTime();

	if (src==NULL) { LogErr("D3D9Client::clbkBlt() Source surface is NULL"); return false; }
	if (tgt==NULL) tgt = pFramework->GetBackBufferHandle();

	int w = SURFACE(src)->GetWidth();
	int h = SURFACE(src)->GetHeight();

	RECT rs = { 0, 0, w, h };
	RECT rt = _RECT( tgtx, tgty, tgtx+w, tgty+h );

	if (pBltGrpTgt) {
		if (CheckBltGroup(src,tgt)) SURFACE(pBltGrpTgt)->AddQueue(SURFACE(src), &rs, &rt);
		else 						SURFACE(tgt)->CopyRect(SURFACE(src), &rs, &rt, flag);
	}
	else SURFACE(tgt)->CopyRect(SURFACE(src), &rs, &rt, flag);

	D3D9SetTime(D3D9Stats.Timer.BlitTime, time);

	return true;
}

// =======================================================================

bool D3D9Client::clbkBlt(SURFHANDLE tgt, DWORD tgtx, DWORD tgty, SURFHANDLE src, DWORD srcx, DWORD srcy, DWORD w, DWORD h, DWORD flag) const
{
	_TRACE;

	double time = D3D9GetTime();

	if (src==NULL) { LogErr("D3D9Client::clbkBlt() Source surface is NULL"); return false; }
	if (tgt==NULL) tgt = pFramework->GetBackBufferHandle();

	RECT rs = _RECT( srcx, srcy, srcx+w, srcy+h );
	RECT rt = _RECT( tgtx, tgty, tgtx+w, tgty+h );

	if (pBltGrpTgt) {
		if (CheckBltGroup(src,tgt)) SURFACE(pBltGrpTgt)->AddQueue(SURFACE(src), &rs, &rt);
		else 						SURFACE(tgt)->CopyRect(SURFACE(src), &rs, &rt, flag);
	}
	else SURFACE(tgt)->CopyRect(SURFACE(src), &rs, &rt, flag);

	D3D9SetTime(D3D9Stats.Timer.BlitTime, time);

	return true;
}

// =======================================================================

bool D3D9Client::clbkScaleBlt (SURFHANDLE tgt, DWORD tgtx, DWORD tgty, DWORD tgtw, DWORD tgth,
                               SURFHANDLE src, DWORD srcx, DWORD srcy, DWORD srcw, DWORD srch, DWORD flag) const
{
	_TRACE;
	
	double time = D3D9GetTime();

	if (src==NULL) { LogErr("D3D9Client::clbkScaleBlt() Source surface is NULL"); return false; }
	if (tgt==NULL) tgt = pFramework->GetBackBufferHandle();

	RECT rs = _RECT( srcx, srcy, srcx+srcw, srcy+srch );
	RECT rt = _RECT( tgtx, tgty, tgtx+tgtw, tgty+tgth );

	if (pBltGrpTgt) {
		if (CheckBltGroup(src,tgt)) SURFACE(pBltGrpTgt)->AddQueue(SURFACE(src), &rs, &rt);
		else 						SURFACE(tgt)->CopyRect(SURFACE(src), &rs, &rt, flag);
	}
	else SURFACE(tgt)->CopyRect(SURFACE(src), &rs, &rt, flag);

	D3D9SetTime(D3D9Stats.Timer.BlitTime, time);

	return true;
}

// =======================================================================

bool D3D9Client::clbkCopyBitmap(SURFHANDLE pdds, HBITMAP hbm, int x, int y, int dx, int dy)
{
	_TRACE;
	return GraphicsClient::clbkCopyBitmap(pdds, hbm, x,y,dx,dy);;
}

// =======================================================================

bool D3D9Client::clbkFillSurface(SURFHANDLE tgt, DWORD col) const
{
	_TRACE;
	if (tgt==NULL) tgt = pFramework->GetBackBufferHandle();
	bool ret = SURFACE(tgt)->Clear(col);
	return ret;
}

// =======================================================================

bool D3D9Client::clbkFillSurface(SURFHANDLE tgt, DWORD tgtx, DWORD tgty, DWORD w, DWORD h, DWORD col) const
{
	_TRACE;
	if (tgt==NULL) tgt = pFramework->GetBackBufferHandle();
	RECT r = _RECT(tgtx, tgty, tgtx+w, tgty+h);
	bool ret = SURFACE(tgt)->Fill(&r, col);
	return ret;
}

#pragma endregion

// =======================================================================
// Constellation name functions
// =======================================================================

DWORD D3D9Client::GetConstellationMarkers(const LABELSPEC **cm_list) const
{
	if ( !g_cm_list ) {
		#pragma pack(1)
		// File entry struct
		typedef struct {
			double lng;    ///< longitude
			double lat;    ///< latitude
			char   abr[3]; ///< abbreviation (short name)
			long   len;    ///< length of 'fullname'
		} ConstellEntry;
		#pragma pack()

		FILE* file = NULL;
		const size_t e_size = sizeof(ConstellEntry);
		const float sphere_r = 1e6f; // the actual render distance for the celestial sphere
		                             // is irrelevant, since it is rendered without z-buffer,
		                             // but it must be within the frustum limits - check this
		                             // in case the near and far planes are dynamically changed!

		if ( 0 != fopen_s(&file, ".\\Constell2.bin", "rb") || file == NULL ) {
			LogErr("Could not open 'Constell2.bin'");
			return 0;
		}

		ConstellEntry f_entry;
		LABELSPEC *p_out;

		// Get number of labels from file
		while ( !feof(file) && (1 == fread(&f_entry, e_size, 1 , file)) ) {
			++g_cm_list_count;
			fseek(file, f_entry.len, SEEK_CUR);
		}

		rewind(file);

		g_cm_list = new LABELSPEC[g_cm_list_count]();

		for (p_out = g_cm_list; !feof(file); ++p_out) {
			if ( 1 == fread(&f_entry, e_size, 1 , file)) {
				p_out->label[0] = new char[f_entry.len+1]();
				p_out->label[1] = new char[4]();
				// position
				double xz = sphere_r * cos(f_entry.lat);
				p_out->pos.x = xz * cos(f_entry.lng);
				p_out->pos.z = xz * sin(f_entry.lng);
				p_out->pos.y = sphere_r * sin(f_entry.lat);
				// fullname
				fread(p_out->label[0], sizeof(char), f_entry.len, file);
				// shortname
				p_out->label[1][0] = f_entry.abr[0];
				p_out->label[1][1] = f_entry.abr[1];
				p_out->label[1][2] = f_entry.abr[2];
			}
		}

		fclose(file);
	}

	*cm_list = g_cm_list;

	return g_cm_list_count;
}

// =======================================================================
// GDI functions
// =======================================================================

HDC D3D9Client::clbkGetSurfaceDC(SURFHANDLE surf)
{
	_TRACE;
	if (ChkDev(__FUNCTION__)) return NULL;

	if (surf == NULL) {
		if (Config->GDIOverlay) {
			LPDIRECT3DSURFACE9 pGDI = GetScene()->GetBuffer(GBUF_GDI);
			HDC hDC;
			if (pGDI) if (pGDI->GetDC(&hDC) == S_OK) {
				if (bGDIClear) {
					bGDIClear = false;
					DWORD color = 0xF08040; // BGR "Color Key" value for transparency
					HBRUSH hBrush = CreateSolidBrush((COLORREF)color);
					RECT r = _RECT( 0, 0, viewW, viewH );
					FillRect(hDC, &r, hBrush);
					DeleteObject(hBrush);
				}
				return hDC;
			}
		}
		return NULL;
	}
	HDC hDC = SURFACE(surf)->GetDC();
	return hDC;
}

// =======================================================================

void D3D9Client::clbkReleaseSurfaceDC(SURFHANDLE surf, HDC hDC)
{
	_TRACE;
	if (ChkDev(__FUNCTION__)) return;

	if (hDC == NULL) { LogErr("D3D9Client::clbkReleaseSurfaceDC() Input hDC is NULL"); return; }
	if (surf == NULL) {
		if (Config->GDIOverlay) {
			LPDIRECT3DSURFACE9 pGDI = GetScene()->GetBuffer(GBUF_GDI);
			if (pGDI) pGDI->ReleaseDC(hDC);
		}
		return;
	}
	SURFACE(surf)->ReleaseDC(hDC);
}

// =======================================================================

bool D3D9Client::clbkFilterElevation(OBJHANDLE hPlanet, int ilat, int ilng, int lvl, double elev_res, INT16* elev)
{
	_TRACE;
	return FilterElevationPhysics(hPlanet, lvl, ilat, ilng, elev_res, elev);
}

// =======================================================================

bool D3D9Client::clbkSplashLoadMsg (const char *msg, int line)
{
	_TRACE;
	return OutputLoadStatus (msg, line);
}

// =======================================================================

LPD3D9CLIENTSURFACE D3D9Client::GetDefaultTexture() const
{
	return pDefaultTex;
}

// =======================================================================

HWND D3D9Client::GetWindow()
{
	return pFramework->GetRenderWindow();
}

// =======================================================================

LPD3D9CLIENTSURFACE D3D9Client::GetBackBufferHandle() const
{
	_TRACE;
	return SURFACE(pFramework->GetBackBufferHandle());
}

// =======================================================================

void D3D9Client::MakeRenderProcCall(Sketchpad *pSkp, DWORD id, LPD3DXMATRIX pV, LPD3DXMATRIX pP)
{
	for (auto it = RenderProcs.cbegin(); it != RenderProcs.cend(); ++it) {
		if (it->id == id) {
			D3D9Pad *pSkp2 = (D3D9Pad *)pSkp;
			pSkp2->LoadDefaults();
			if (id == RENDERPROC_EXTERIOR || id == RENDERPROC_PLANETARIUM) {
				pSkp2->SetViewMode(Sketchpad2::USER);
			}
			pSkp2->SetViewMatrix((FMATRIX4 *)pV);
			pSkp2->SetProjectionMatrix((FMATRIX4 *)pP);
			it->proc(pSkp, it->pParam);
		}
	}
}

// =======================================================================

void D3D9Client::MakeGenericProcCall(DWORD id, int iUser, void *pUser) const
{
	for (auto it = GenericProcs.cbegin(); it != GenericProcs.cend(); ++it) {
		if (it->id == id) it->proc(iUser, pUser, it->pParam);
	}
}

// =======================================================================

bool D3D9Client::RegisterRenderProc(__gcRenderProc proc, DWORD id, void *pParam)
{
	if (id)	{ // register (add)
		RenderProcData data = { proc, pParam, id };
		RenderProcs.push_back(data);
		return true;
	}
	else { // unregister, mark as unused (remove later)
		for (auto it = RenderProcs.begin(); it != RenderProcs.end(); ++it) {
			if (it->proc == proc) {
				it->id = 0;
				it->pParam = NULL;
				it->proc = NULL;
				return true;
			}
		}
	}
	return false;
}

// =======================================================================

bool D3D9Client::RegisterGenericProc(__gcGenericProc proc, DWORD id, void *pParam)
{
	if (id) { // register (add)
		GenericProcData data = { proc, pParam, id };
		GenericProcs.push_back(data);
		return true;
	}
	else { // unregister, mark as unused (remove later)
		for (auto it = GenericProcs.begin(); it != GenericProcs.end(); ++it) {
			if (it->proc == proc) {
				it->id = 0;
				it->pParam = NULL;
				it->proc = NULL;
				return true;
			}
		}
	}
	return false;
}

// =======================================================================

bool D3D9Client::IsGenericProcEnabled(DWORD id) const
{
	for each (auto val in GenericProcs) if (val.id == id) return true;
	return false;
}

// =======================================================================

void D3D9Client::WriteLog(const char *msg) const
{
	_TRACE;
	char cbuf[256];
	sprintf_s(cbuf, 256, "D3D9: %s", msg);
	oapiWriteLog(cbuf);
}

// =======================================================================

bool D3D9Client::OutputLoadStatus(const char *txt, int line)
{

	if (bRunning) return false;

	if (line == 1) strcpy_s(pLoadItem, 127, txt); else
	if (line == 0) strcpy_s(pLoadLabel, 127, txt), pLoadItem[0] = '\0'; // New top line => clear 2nd line

	if (pTextScreen) {

		if (pDevice->TestCooperativeLevel()!=S_OK) {
			LogErr("TestCooperativeLevel() Failed");
			return false;
		}

		RECT txt = _RECT( loadd_x, loadd_y, loadd_x+loadd_w, loadd_y+loadd_h );

		pDevice->StretchRect(pSplashScreen, &txt, pTextScreen, NULL, D3DTEXF_POINT);

		HDC hDC;
		HR(pTextScreen->GetDC(&hDC));

		HFONT hO = (HFONT)SelectObject(hDC, hLblFont1);
		SetTextColor(hDC, 0xE0A0A0);
		SetBkMode(hDC,TRANSPARENT);
		SetTextAlign(hDC, TA_LEFT|TA_TOP);

		TextOut(hDC, 2, 2, pLoadLabel, lstrlen(pLoadLabel));

		SelectObject(hDC, hLblFont2);
		TextOut(hDC, 2, 36, pLoadItem, lstrlen(pLoadItem));

		HPEN pen = CreatePen(PS_SOLID,1,0xE0A0A0);
		HPEN po = (HPEN)SelectObject(hDC, pen);

		MoveToEx(hDC, 0, 32, NULL);
		LineTo(hDC, loadd_w, 32);

		SelectObject(hDC, po);
		SelectObject(hDC, hO);
		DeleteObject(pen);

		HR(pTextScreen->ReleaseDC(hDC));
		HR(pDevice->StretchRect(pSplashScreen, NULL, pBackBuffer, NULL, D3DTEXF_POINT));
		HR(pDevice->StretchRect(pTextScreen, NULL, pBackBuffer, &txt, D3DTEXF_POINT));

		IDirect3DSwapChain9 *pSwap;

		if (pDevice->GetSwapChain(0, &pSwap)==S_OK) {
			pSwap->Present(0, 0, 0, 0, D3DPRESENT_DONOTWAIT);
			pSwap->Release();
			return true;
		}

		// Prevent "Not Responding" during loading
		MSG msg;
		while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessage(&msg);
	}
	return false;
}

// =======================================================================

void D3D9Client::SplashScreen()
{

	loadd_x = 279*viewW/1280;
	loadd_y = 545*viewH/800;
	loadd_w = viewW/3;
	loadd_h = 80;

	RECT rS;

	GetWindowRect(hRenderWnd, &rS);

	LogAlw("Splash Window Size = [%u, %u]", rS.right - rS.left, rS.bottom - rS.top);
	LogAlw("Splash Window LeftTop = [%d, %d]", rS.left, rS.top);

	HR(pDevice->TestCooperativeLevel());
	HR(pDevice->Clear(0, NULL, D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER|D3DCLEAR_STENCIL, 0x0, 1.0f, 0L));
	HR(pDevice->CreateOffscreenPlainSurface(loadd_w, loadd_h, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &pTextScreen, NULL));
	HR(pDevice->CreateOffscreenPlainSurface(viewW, viewH, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &pSplashScreen, NULL));

	D3DXIMAGE_INFO Info;

	HMODULE hOrbiter =  GetModuleHandleA("orbiter.exe");
	HRSRC hRes = FindResourceA(hOrbiter, MAKEINTRESOURCEA(292), "IMAGE");
	HGLOBAL hImage = LoadResource(hOrbiter, hRes);
	LPVOID pData = LockResource(hImage);
	DWORD size = SizeofResource(hOrbiter, hRes);

	// Splash screen image is 1920 x 1200 pixel
	double scale = min(viewW / 1920.0, viewH / 1200.0);
	double _w = (1920.0 * scale);
	double _h = (1200.0 * scale);
	double _l = abs(viewW - _w)/2.0;
	double _t = abs(viewH - _h)/2.0;
	RECT imgRect = {
		static_cast<LONG>( round(_l) ),
		static_cast<LONG>( round(_t) ),
		static_cast<LONG>( round(_w + _l) ),
		static_cast<LONG>( round(_h + _t) )
	};
	HR(pDevice->ColorFill(pSplashScreen, NULL, D3DCOLOR_XRGB(0, 0, 0)));
	HR(D3DXLoadSurfaceFromFileInMemory(pSplashScreen, NULL, &imgRect, pData, size, NULL, D3DX_FILTER_LINEAR, 0, &Info));

	HDC hDC;
	HR(pSplashScreen->GetDC(&hDC));

	LOGFONTA fnt; memset((void *)&fnt, 0, sizeof(LOGFONT));

	fnt.lfHeight		 = 18;
	fnt.lfWeight		 = 700;
	fnt.lfCharSet		 = ANSI_CHARSET;
	fnt.lfOutPrecision	 = OUT_DEFAULT_PRECIS;
	fnt.lfClipPrecision	 = CLIP_DEFAULT_PRECIS;
	fnt.lfQuality		 = ANTIALIASED_QUALITY;
	fnt.lfPitchAndFamily = DEFAULT_PITCH;
	strcpy_s(fnt.lfFaceName, "Courier New");

	HFONT hF = CreateFontIndirect(&fnt);

	HFONT hO = (HFONT)SelectObject(hDC, hF);
	SetTextColor(hDC, 0xE0A0A0);
	SetBkMode(hDC,TRANSPARENT);

	char *months[]={"???","Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec","???"};

	DWORD d = oapiGetOrbiterVersion();
	DWORD y = d/10000; d-=y*10000;
	DWORD m = d/100; d-=m*100;
	if (m>12) m=0;

#ifdef _DEBUG
	char dataA[]={"D3D9Client Beta R30.7 (Debug Build) [" __DATE__ "]"};
#else
	char dataA[]={"D3D9Client Beta R30.7 [" __DATE__ "]"};
#endif

	char dataB[128]; sprintf_s(dataB,128,"Build %s %lu 20%lu [%u]", months[m], d, y, oapiGetOrbiterVersion());
	char dataD[] = { "Warning: Config folder not present in /Modules/Server/. Please create symbolic link." };
	char dataE[] = { "Note: Cubic Interpolation is use... Consider using linear for better elevation matching" };
	char dataF[] = { "Note: Terrain flattening offline due to cubic interpolation" };

	int xc = viewW*750/1280;
	int yc = viewH*545/800;

	TextOut(hDC, xc, yc + 0*20, "ORBITER Space Flight Simulator",30);
	TextOut(hDC, xc, yc + 1*20, dataB, lstrlen(dataB));
	TextOut(hDC, xc, yc + 2*20, dataA, lstrlen(dataA));

	DWORD VPOS = viewH - 50;
	DWORD LSPACE = 20;

	SetTextAlign(hDC, TA_CENTER);
	DWORD cattrib = GetFileAttributes("Modules/Server/Config");

	if ((cattrib&0x10)==0 || cattrib==INVALID_FILE_ATTRIBUTES) {
		TextOut(hDC, viewW/2, VPOS, dataD, lstrlen(dataD));
		VPOS -= LSPACE;
	}

	if ((*(int*)GetConfigParam(CFGPRM_ELEVATIONMODE)) == 2) {
		TextOut(hDC, viewW / 2, VPOS, dataE, lstrlen(dataE));
		VPOS -= LSPACE;
	}

	if ((*(int*)GetConfigParam(CFGPRM_ELEVATIONMODE)) == 2) {
		TextOut(hDC, viewW / 2, VPOS, dataE, lstrlen(dataE));
		VPOS -= LSPACE;
	}

	if (Config->bFlats && !Config->bFlatsEnabled) {
		TextOut(hDC, viewW / 2, VPOS, dataF, lstrlen(dataF));
		VPOS -= LSPACE;
	}


	SelectObject(hDC, hO);
	DeleteObject(hF);

	HR(pSplashScreen->ReleaseDC(hDC));


	RECT src = _RECT( loadd_x, loadd_y, loadd_x+loadd_w, loadd_y+loadd_h );
	pDevice->StretchRect(pSplashScreen, &src, pTextScreen, NULL, D3DTEXF_POINT);
	pDevice->StretchRect(pSplashScreen, NULL, pBackBuffer, NULL, D3DTEXF_POINT);
	pDevice->Present(0, 0, 0, 0);
}

// =======================================================================

HRESULT D3D9Client::BeginScene()
{
	bRendering = false;
	HRESULT hr = pDevice->BeginScene();
	if (hr == S_OK) bRendering = true;
	return hr;
}

// =======================================================================

void D3D9Client::EndScene()
{
	pDevice->EndScene();
	bRendering = false;
}

// =======================================================================

#pragma region Drawing_(Sketchpad)_Interface


double sketching_time;

// =======================================================================
// 2D Drawing Interface
//
oapi::Sketchpad *D3D9Client::clbkGetSketchpad(SURFHANDLE surf)
{
	if (ChkDev(__FUNCTION__)) return NULL;

	if (GetCurrentThread() != hMainThread) {
		_wassert(L"Sketchpad called from a worker thread !", _CRT_WIDE(__FILE__), __LINE__);
		return NULL;
	}

	sketching_time = D3D9GetTime();

	if (surf == NULL) surf = GetBackBufferHandle();

	if (SURFACE(surf)->IsRenderTarget()) {

		// Get Pooled Sketchpad
		D3D9Pad *pPad = SURFACE(surf)->GetD3D9Pad();

		// Get Current interface if any
		D3D9Pad *pCur = GetTopInterface();

		// Do we have an existing SketchPad interface in use
		if (pCur) {
			if (pCur == pPad) _wassert(L"Sketchpad already exists for this surface", _CRT_WIDE(__FILE__), __LINE__);
			pCur->EndDrawing();	// Put the current one in hold
			LogDbg("Red", "Switching to another sketchpad in a middle");
		}

		// Push a new Sketchpad onto a stack
		PushSketchpad(surf, pPad);

		pPad->BeginDrawing();
		pPad->LoadDefaults();

		return pPad;
	}
	else {
		HDC hDC = SURFACE(surf)->GetDC();
		if (hDC) return new GDIPad(surf, hDC);
	}

	return NULL;
}

// =======================================================================

void D3D9Client::clbkReleaseSketchpad(oapi::Sketchpad *sp)
{
	_TRACE;
	if (ChkDev(__FUNCTION__)) return;

	if (!sp) return;

	SURFHANDLE hSrf = sp->GetSurface();

	if (SURFACE(hSrf)->IsRenderTarget()) {

		D3D9Pad *pPad = ((D3D9Pad*)sp);

		assert(!pPad->IsNative());

		if (GetTopInterface() != pPad) _wassert(L"Sketchpad release failed. Not a top one.", _CRT_WIDE(__FILE__), __LINE__);

		pPad->EndDrawing();

		PopRenderTargets();

		// Do we have an old interface ?
		D3D9Pad *pOld = GetTopInterface();
		if (pOld) {
			pOld->BeginDrawing();	// Continue with the old one
			LogDbg("Red", "Continue Previous Sketchpad");
		}
	}
	else {
		GDIPad *pGDI = (GDIPad *)sp;
		SURFACE(hSrf)->ReleaseDC(pGDI->GetDC());
		delete pGDI;
	}

	sketching_time = D3D9GetTime() - sketching_time;
}

// =======================================================================

oapi::Sketchpad *D3D9Client::GetSketchpadNative(HSURFNATIVE hNat, HSURFNATIVE hDep)
{
	if (ChkDev(__FUNCTION__)) return NULL;

	D3DSURFACE_DESC	desc;
	LPDIRECT3DSURFACE9 pSurf = NULL;
	LPDIRECT3DTEXTURE9 pTex = NULL;

	if (GetCurrentThread() != hMainThread) {
		_wassert(L"Sketchpad called from a worker thread !", _CRT_WIDE(__FILE__), __LINE__);
		return NULL;
	}

	if (hNat == NULL) hNat = pBackBuffer;

	LPDIRECT3DRESOURCE9 pResource = static_cast<LPDIRECT3DRESOURCE9>(hNat);

	if (pResource->GetType() == D3DRTYPE_SURFACE) {
		pSurf = static_cast<LPDIRECT3DSURFACE9>(hNat);
		pSurf->GetDesc(&desc);
	}

	if (pResource->GetType() == D3DRTYPE_TEXTURE) {
		pTex = static_cast<LPDIRECT3DTEXTURE9>(hNat);
		pTex->GetLevelDesc(0, &desc);
		pTex->GetSurfaceLevel(0, &pSurf);
	}

	if (desc.Usage & D3DUSAGE_RENDERTARGET)
	{
		// Get Pooled Sketchpad
		D3D9Pad *pPad = new D3D9Pad("GetSketchpadNative", hNat);

		// Get Current interface if any
		D3D9Pad *pCur = GetTopInterface();

		// Do we have an existing SketchPad interface in use
		if (pCur) {
			if (pCur->GetSurface() == hNat) _wassert(L"Sketchpad already exists for this surface", _CRT_WIDE(__FILE__), __LINE__);
			pCur->EndDrawing();	// Put the current one in hold
		}

		// Push a new Sketchpad onto a stack
		PushSketchpadNative(pSurf, static_cast<LPDIRECT3DSURFACE9>(hDep), pPad);

		pPad->BeginDrawing();
		pPad->LoadDefaults();

		return pPad;
	}
	else {
		if (pTex) pSurf->Release();
	}

	return NULL;
}

// =======================================================================

void D3D9Client::ReleaseSketchpadNative(Sketchpad *sp)
{
	if (ChkDev(__FUNCTION__)) return;
	if (!sp) return;

	D3D9Pad *pPad = static_cast<D3D9Pad*>(sp);

	assert(pPad->IsNative());

	if (GetTopInterface() != pPad) _wassert(L"Sketchpad release failed. Not a top one.", _CRT_WIDE(__FILE__), __LINE__);

	pPad->EndDrawing();

	LPDIRECT3DRESOURCE9 pResource = static_cast<LPDIRECT3DRESOURCE9>(pPad->GetSurface());

	// Must release top render target if acquired for a texture due to "pTex->GetSurfaceLevel(0, &pSurf)"
	if (pResource->GetType() == D3DRTYPE_TEXTURE) GetTopRenderTarget()->Release();

	PopRenderTargets();

	// Do we have an old interface ?
	D3D9Pad *pOld = GetTopInterface();
	if (pOld) pOld->BeginDrawing();	// Continue with the old one

	delete pPad;
}

// =======================================================================

Font *D3D9Client::clbkCreateFont(int height, bool prop, const char *face, Font::Style style, int orientation) const
{
	_TRACE;
	if (ChkDev(__FUNCTION__)) return NULL;
	return *g_fonts.insert(new D3D9PadFont(height, prop, face, style, orientation)).first;
}

// =======================================================================

void D3D9Client::clbkReleaseFont(Font *font) const
{
	_TRACE;
	if (!g_fonts.count(font)) return;
	g_fonts.erase(font);
	delete ((D3D9PadFont*)font);
}

// =======================================================================

Pen *D3D9Client::clbkCreatePen(int style, int width, DWORD col) const
{
	_TRACE;
	return *g_pens.insert(new D3D9PadPen(style, width, col)).first;
}

// =======================================================================

void D3D9Client::clbkReleasePen(Pen *pen) const
{
	_TRACE;
	if (!g_pens.count(pen)) return;
	g_pens.erase(pen);
	delete ((D3D9PadPen*)pen);
}

// =======================================================================

Brush *D3D9Client::clbkCreateBrush(DWORD col) const
{
	_TRACE;
	return *g_brushes.insert(new D3D9PadBrush(col)).first;
}

// =======================================================================

void D3D9Client::clbkReleaseBrush(Brush *brush) const
{
	_TRACE;
	if (!g_brushes.count(brush)) return;
	g_brushes.erase(brush);
	delete ((D3D9PadBrush*)brush);
}

#pragma endregion


// ======================================================================
// class VisObject

VisObject::VisObject(OBJHANDLE hObj) : hObj(hObj)
{
	_TRACE;
}

// =======================================================================

VisObject::~VisObject ()
{
}
