// ==============================================================
// Atmospheric controls implementation
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2014 Jarmo Nikkanen
// ==============================================================

#include "D3D9Client.h"
#include "resource.h"
#include "D3D9Config.h"
#include "AtmoControls.h"
#include "Commctrl.h"
#include "vObject.h"
#include "vPlanet.h"
#include "Mesh.h"
#include "Scene.h"
#include <stdio.h>

using namespace oapi;

extern HINSTANCE g_hInst;
extern D3D9Client *g_client;

// ==============================================================

// Defaut c'tor to init members
ScatterParams::ScatterParams() :
	red      ( 0.650 ),  // 0.400 ... 0.700
	green    ( 0.500 ),  // 0.400 ... 0.700
	blue     ( 0.480 ),  // 0.400 ... 0.700
	rpow	 ( 4.0 ),    // -8.0 ... 8.0
	mpow	 ( 1.0 ),    // -2.0 ... 2.0
	height   ( 8.0 ),    // 4.0 ... 40.0 [km]
	depth    ( 1.0 ),    // 0.0 ... 1.5
	// ----------------------------------------
	expo     ( 0.5 ),    // 0.2 ... 1.5
	balance  ( 0.0 ),    // -0.5 ... 0.5
	// ----------------------------------------
	rin      ( 1.0 ),    // 0.0 ... 3.0
	rout     ( 0.592 ),  // 0.0 ... 4.0
	rphase   ( 0.3395 ), // 0.0 ... 3.5
	// ----------------------------------------
	moffset  ( 1.0 ),    // 0.0 ... 2.0
	mie      ( 0.0869 ), // 0.0 ... 8.0
	mphase   ( 0.9831 ), // 0.85 ... 0.999
	// ----------------------------------------
	aux1	 ( 0.0 ),    // 0.0 ... 2.0
	aux2	 ( 0.0 ),    // 0.0 ... 2.0
	// ----------------------------------------
	mode     ( 0 ),      // [0|1]
	oversat  ( true ),   // [true|false]
	pSunLight ( NULL )
{
}

// ==============================================================

namespace AtmoControls {

	struct sSlider {
		double min, max;
		HWND hWnd;
		int id;
		int dsp;
		int style;
	};


ScatterParams defs;
ScatterParams *param = NULL;

sSlider Slider[ATM_SLIDER_COUNT];

DWORD dwCmd = NULL;
HWND hDlg = NULL;
vPlanet *vObj = NULL;

// ==============================================================

HWND CreateToolTip(int toolID, HWND hDlg, PTSTR pszText)
{
    if (!toolID || !hDlg || !pszText) return NULL;
    
    // Get the window of the tool.
    HWND hwndTool = GetDlgItem(hDlg, toolID);
    // Create the tooltip. g_hInst is the global instance handle.
    HWND hwndTip = CreateWindowEx(NULL, TOOLTIPS_CLASS, NULL, WS_POPUP |TTS_ALWAYSTIP | TTS_BALLOON, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, hDlg, NULL, g_hInst, NULL);
    
    if (!hwndTool || !hwndTip) return NULL;
                                                          
    // Associate the tooltip with the tool.
    TOOLINFO toolInfo = { 0 };
    toolInfo.cbSize = sizeof(toolInfo);
    toolInfo.hwnd = hDlg;
    toolInfo.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    toolInfo.uId = (UINT_PTR)hwndTool;
    toolInfo.lpszText = pszText;
    SendMessage(hwndTip, TTM_ADDTOOL, 0, (LPARAM)&toolInfo);

    return hwndTip;
}

// ==============================================================

void Create()
{
	vObj = NULL;
	hDlg = NULL;

	dwCmd = oapiRegisterCustomCmd("D3D9 Atmospheric Controls", "This dialog allows to control various atmospheric parameters and effects", OpenDlgClbk, NULL);
	
	memset(Slider,0,sizeof(Slider));
	
	// ATTENTION: Order of params must match with slider indexes

	Slider[0].id = IDC_ATM_RED;
	Slider[1].id = IDC_ATM_GREEN;
	Slider[2].id = IDC_ATM_BLUE;
	Slider[3].id = IDC_ATM_RPOW;
	Slider[4].id = IDC_ATM_IN;
	Slider[5].id = IDC_ATM_OUT;
	Slider[6].id = IDC_ATM_RPHASE;
	Slider[7].id = IDC_ATM_MIE;
	Slider[8].id = IDC_ATM_MPHASE;
	Slider[9].id = IDC_ATM_BALANCE;
	Slider[10].id = IDC_ATM_HEIGHT;
	Slider[11].id = IDC_ATM_AUX2;
	Slider[12].id = IDC_ATM_DEPTH;
	Slider[13].id = IDC_ATM_MPOW;
	Slider[14].id = IDC_ATM_EXPO;
	Slider[15].id = IDC_ATM_MOFFSET;
	Slider[16].id = IDC_ATM_AUX1;

	Slider[0].dsp = IDC_ATD_RED;
	Slider[1].dsp = IDC_ATD_GREEN;
	Slider[2].dsp = IDC_ATD_BLUE;
	Slider[3].dsp = IDC_ATD_RPOW;
	Slider[4].dsp = IDC_ATD_IN;
	Slider[5].dsp = IDC_ATD_OUT;
	Slider[6].dsp = IDC_ATD_RPHASE;
	Slider[7].dsp = IDC_ATD_MIE;
	Slider[8].dsp = IDC_ATD_MPHASE;
	Slider[9].dsp = IDC_ATD_BALANCE;
	Slider[10].dsp = IDC_ATD_HEIGHT;
	Slider[11].dsp = IDC_ATD_AUX2;
	Slider[12].dsp = IDC_ATD_DEPTH;
	Slider[13].dsp = IDC_ATD_MPOW;
	Slider[14].dsp = IDC_ATD_EXPO;
	Slider[15].dsp = IDC_ATD_MOFFSET;
	Slider[16].dsp = IDC_ATD_AUX1;
}

// ==============================================================

bool IsActive()
{
	return (hDlg!=NULL);
}

// ==============================================================

void Release()
{
	if (dwCmd) oapiUnregisterCustomCmd(dwCmd);
	dwCmd = NULL;
}

// ==============================================================

void OpenDlgClbk(void *context)
{
	HWND l_hDlg = oapiOpenDialog(g_hInst, IDD_D3D9SCATTER, WndProc);

	if (l_hDlg) hDlg = l_hDlg; // otherwise open already
	else return;

	Scene *scene = g_client->GetScene();
	
	if (scene) {
		OBJHANDLE hBody = scene->GetCameraProxyBody();
		if (hBody) vObj = (vPlanet *) scene->GetVisObject(hBody);
	}
	
	if (vObj) param = vObj->GetAtmoParams();
	else      param = &defs;

	for (int i=0;i<ATM_SLIDER_COUNT;i++) Slider[i].hWnd = GetDlgItem(hDlg, Slider[i].id);

	ConfigSlider(IDC_ATM_RED,      0.400, 0.700);
	ConfigSlider(IDC_ATM_GREEN,    0.400, 0.700);
	ConfigSlider(IDC_ATM_BLUE,     0.400, 0.700);
	ConfigSlider(IDC_ATM_RPOW,     -8.0, 8.0);
	ConfigSlider(IDC_ATM_MPOW,     -2.0, 2.0);
	ConfigSlider(IDC_ATM_HEIGHT,   4.0, 40.0, 1);
	ConfigSlider(IDC_ATM_DEPTH,    0.0, 1.5);
	// -------------------------------------------------------
	ConfigSlider(IDC_ATM_EXPO,	   0.1, 3.0);
	ConfigSlider(IDC_ATM_BALANCE,  -0.3, 0.7);
	// -------------------------------------------------------
	ConfigSlider(IDC_ATM_OUT,      0.0, 2.0);
	ConfigSlider(IDC_ATM_IN,       0.5, 2.0);
	ConfigSlider(IDC_ATM_RPHASE,   0.0, 1.5);
	// -------------------------------------------------------
	ConfigSlider(IDC_ATM_MOFFSET,  0.0, 2.0);
	ConfigSlider(IDC_ATM_MIE,      0.0, 2.0);
	ConfigSlider(IDC_ATM_MPHASE,   0.80, 0.999);
	// -------------------------------------------------------
	ConfigSlider(IDC_ATM_AUX1,	   0.0, 2.0);
	ConfigSlider(IDC_ATM_AUX2,	   0.0, 2.0);
	/*
	CreateToolTip(IDC_ATM_RED,		hDlg, "Wavelength setting for red light (default 0.650)");
	CreateToolTip(IDC_ATM_GREEN,	hDlg, "Wavelength setting for green light (default 0.600)");
	CreateToolTip(IDC_ATM_BLUE,		hDlg, "Wavelength setting for blue light (default 0.480)");
	CreateToolTip(IDC_ATM_WAVE,		hDlg, "Main control for atmospheric color composition (4.0 for the Earth)");
	CreateToolTip(IDC_ATM_HEIGHT,	hDlg, "Atmosphere scale height (7km - 10km for the Earth)");
	CreateToolTip(IDC_ATM_EXPO,		hDlg, "Overall brightness control (i.e. Camera \"exposure\" control)");
	// -------------------------------------------------------
	CreateToolTip(IDC_ATM_OUT,		hDlg, "Overall control for rayleigh scattering (i.e. Haze stickness)");
	CreateToolTip(IDC_ATM_IN,		hDlg, "(FINE) Controls an intensity of in-scattered sunlight (i.e. Haze glow intensity)");
	CreateToolTip(IDC_ATM_RPHASE,	hDlg, "Controls a directional dependency of in-scattered sunlight (Most visible when camera, planet and the sun are aligned)");
	CreateToolTip(IDC_ATM_BALANCE,	hDlg, "(FINE) Controls a color balance of atmospheric haze. (Most effective in sunrise/set)");
	CreateToolTip(IDC_ATM_RSUN,		hDlg, "Optical depth clamp distance (i.e. Horizon haze distance)");
	// -------------------------------------------------------
	CreateToolTip(IDC_ATM_SRFCOLOR,	hDlg, "(FINE) Controls a color composition of sunlight on a planet surface (Configure at sunrise/set)");
	CreateToolTip(IDC_ATM_SUN,		hDlg, "(FINE) Controls an intensity of sunlight on a planet surface");
	// -------------------------------------------------------
	CreateToolTip(IDC_ATM_MIE,		hDlg, "Overall scale factor for mie scattering");
	CreateToolTip(IDC_ATM_MPHASE,	hDlg, "Directional strength of Henyey-Greenstein phase function");
	*/

	SendDlgItemMessageA(hDlg, IDC_ATM_MODE, CB_RESETCONTENT, 0, 0);
	SendDlgItemMessageA(hDlg, IDC_ATM_MODE, CB_ADDSTRING, 0, (LPARAM)"Enabled");
	SendDlgItemMessageA(hDlg, IDC_ATM_MODE, CB_ADDSTRING, 0, (LPARAM)"Disabled");
	SendDlgItemMessageA(hDlg, IDC_ATM_MODE, CB_SETCURSEL, param->mode, 0);

	SendDlgItemMessage(hDlg, IDC_ATM_OVERSAT, BM_SETCHECK, param->oversat ? BST_CHECKED : BST_UNCHECKED, 0);

	UpdateSliders();
}

// ==============================================================

double GetValue(int id)
{
	for (int i=0;i<ATM_SLIDER_COUNT;i++) if (Slider[i].id==id) return param->data[i];
	LogErr("Invalid Slider ID in AtmoControls");
	return 0.0;
}

// ==============================================================

void ConfigSlider(int id, double min, double max, int style)
{
	for (int i=0;i<ATM_SLIDER_COUNT;i++) if (Slider[i].id==id) {
		Slider[i].max = max;
		Slider[i].min = min;
		Slider[i].style = style;
		UpdateSlider(id);
		return;
	}
	LogErr("Invalid Slider ID in AtmoControls");
}

// ==============================================================

void SetSlider(int id, WORD pos)
{
	for (int i=0;i<ATM_SLIDER_COUNT;i++) if (Slider[i].id==id) {
		double x = (1000.0-double(pos))/1000.0;
		param->data[i] = Slider[i].min*(1.0-x) + Slider[i].max*x;
		UpdateSlider(id, false);

		if (id==IDC_ATM_HEIGHT && vObj) vObj->UpdateAtmoConfig(); 
		return;
	}
	LogErr("Invalid Slider ID in AtmoControls");
}

// ==============================================================

void UpdateSliders()
{
	for (int i=0;i<ATM_SLIDER_COUNT;i++) UpdateSlider(Slider[i].id);
}

// ==============================================================

void UpdateSlider(int id, bool bSetPos)
{
	char buf[32];

	if (!param) return;

	for (int i=0;i<ATM_SLIDER_COUNT;i++) if (Slider[i].id==id) {

		double val = param->data[i];
		
		SendDlgItemMessage(hDlg, id, TBM_SETRANGEMAX, 1, 1000);
		SendDlgItemMessage(hDlg, id, TBM_SETRANGEMIN, 1, 0);
		SendDlgItemMessage(hDlg, id, TBM_SETTICFREQ,  1, 0);

		if (bSetPos) {
			double x = (val - Slider[i].min)/(Slider[i].max-Slider[i].min);
			DWORD dpos = 1000 - DWORD(x*1000.0);
			SendDlgItemMessage(hDlg, id, TBM_SETPOS,  1, dpos);
		}

		switch (Slider[i].style) {
			case 0:
				sprintf_s(buf,"%.3lf", val);
				break;
			case 1:
				sprintf_s(buf,"%.1lf k", val);
				break;
			default:
				sprintf_s(buf,"%.3lf", val);
				break;
		}

		SetWindowTextA(GetDlgItem(hDlg, Slider[i].dsp), buf);
		return;
	}
	LogErr("Invalid Slider ID in AtmoControls");
}

// ==============================================================

vPlanet * GetVisual()
{
	return vObj;
}

// ==============================================================

void SetVisual(vObject *vo)
{
	if (!vo) {
		vObj = NULL;
		param = &defs;
		return;
	}

	if (!hDlg || !dwCmd) return;
	
	OBJHANDLE hObj = vo->GetObjectA();

	if (oapiGetObjectType(hObj)!=OBJTP_PLANET) {
		LogErr("Invalid Object Type in AtmoControls");
		vObj = NULL;
		param = &defs;
		return;
	}

	vObj = (vPlanet *)vo;

	if (vObj) param = vObj->GetAtmoParams();
	else	  param = &defs;

	UpdateSliders();
}


// ==============================================================
// Dialog message handler

BOOL CALLBACK WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{

	switch (uMsg) {

	case WM_INITDIALOG:
	{
		vObject *vPl = NULL;
		OBJHANDLE hProxy = g_client->GetScene()->GetCameraProxyBody();
		if (hProxy) vPl = g_client->GetScene()->GetVisObject(hProxy);
		SetVisual(vPl);
		return true;
	}

	case WM_VSCROLL:
	{
		if (LOWORD(wParam)==TB_THUMBTRACK) {
			WORD pos = HIWORD(wParam);
			for (int i=0;i<ATM_SLIDER_COUNT;i++) if (Slider[i].hWnd==HWND(lParam)) {
				SetSlider(Slider[i].id, pos);
				return true;
			}
		}
		return false;
	}

	case WM_COMMAND:

		switch (LOWORD(wParam)) {

			case IDCANCEL:  
			case IDOK:
				oapiCloseDialog(hWnd);
				hDlg = NULL;
				return TRUE;

			case IDC_ATM_LOAD:
				if (vObj) {
					vObj->LoadAtmoConfig();
					UpdateSliders();
					SendDlgItemMessage(hWnd, IDC_ATM_OVERSAT, BM_SETCHECK, param->oversat ? BST_CHECKED : BST_UNCHECKED, 0);
				}
				break;

			case IDC_ATM_SAVE:
				if (vObj) {
					vObj->SaveAtmoConfig();
					/*DWORD dAmbient = *(DWORD*)g_client->GetConfigParam(CFGPRM_AMBIENTLEVEL);
					Scatter *pSct = new Scatter(param, vObj->GetObjectA(), dAmbient);
					SAFE_RELEASE(param->pSunLight);
					pSct->ComputeSunLightColorMap(g_client->GetDevice(), &param->pSunLight, true);
					delete pSct;*/
				}
				break;

			case IDC_ATM_RESET:
				param->red = 0.650; 
				param->green = 0.500; 
				param->blue = 0.480;
				param->rin = 1.0;
				param->balance = 0.0;
				param->rphase = 0.25;
				UpdateSliders();
				break;
	
			case IDC_ATM_OVERSAT:
				param->oversat = (SendDlgItemMessageA(hWnd, IDC_ATM_OVERSAT, BM_GETCHECK, 0, 0)==BST_CHECKED);
				break;

			case IDC_ATM_MODE:
				if (HIWORD(wParam)==CBN_SELCHANGE) {
					DWORD idx = SendDlgItemMessage(hWnd, IDC_ATM_MODE, CB_GETCURSEL, 0, 0);
					if (param) param->mode = idx;	
				}
				break;

			default: 
				//LogErr("LOWORD(%hu), HIWORD(0x%hX)",LOWORD(wParam),HIWORD(wParam));
				break;
		}
		break;
	}

	return oapiDefDialogProc(hWnd, uMsg, wParam, lParam);;
}

} //namespace


		