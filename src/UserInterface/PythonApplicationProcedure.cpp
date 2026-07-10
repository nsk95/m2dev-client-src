#include "StdAfx.h"
#include "PythonApplication.h"
#include "Eterlib/Camera.h"

#include <winuser.h>

static int gs_nMouseCaptureRef = 0;

void CPythonApplication::SafeSetCapture()
{
	SetCapture(m_hWnd);
	gs_nMouseCaptureRef++;
}

void CPythonApplication::SafeReleaseCapture()
{
	gs_nMouseCaptureRef--;
	if (gs_nMouseCaptureRef==0)
		ReleaseCapture();
}

void CPythonApplication::__SetFullScreenWindow(HWND hWnd, DWORD dwWidth, DWORD dwHeight, DWORD dwBPP)
{
	DEVMODE DevMode;
	DevMode.dmSize = sizeof(DevMode);
	DevMode.dmBitsPerPel = dwBPP;
	DevMode.dmPelsWidth = dwWidth;
	DevMode.dmPelsHeight = dwHeight;
	DevMode.dmFields = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT;

	LONG Error = ChangeDisplaySettings(&DevMode, CDS_FULLSCREEN);
	if(Error == DISP_CHANGE_RESTART)
	{
		ChangeDisplaySettings(0,0);
	}
}

void CPythonApplication::__MinimizeFullScreenWindow(HWND hWnd, DWORD dwWidth, DWORD dwHeight)
{
	ChangeDisplaySettings(0, 0);
	SetWindowPos(hWnd, 0, 0, 0,
				 dwWidth,
				 dwHeight,
				 SWP_SHOWWINDOW);
	ShowWindow(hWnd, SW_MINIMIZE);
}

// ELEMENTIA-RESIZE: central handler for a changed client size (drag-resize end,
// maximize, restore). Resets the D3D9Ex backbuffer to the new client size and
// propagates the size to the UI layer so rendering and mouse hit-testing stay
// consistent. No-ops when nothing changed, the device is fullscreen-exclusive,
// or the window is minimized/degenerate.
void CPythonApplication::__OnWindowSizeChanged()
{
	if (!m_isWindowed || m_isWindowFullScreenEnable)
		return;

	RECT rcWnd;
	GetClientRect(&rcWnd);

	const UINT uWidth  = UINT(rcWnd.right - rcWnd.left);
	const UINT uHeight = UINT(rcWnd.bottom - rcWnd.top);

	if (uWidth == 0 || uHeight == 0)		// minimized / degenerate
		return;

	if (uWidth == m_dwWidth && uHeight == m_dwHeight)
		return;

	// The character shadow map is a D3DPOOL_DEFAULT render target. A D3D9Ex
	// Reset() keeps the resource alive but its contents become undefined, so we
	// recreate it around the reset - the exact pattern the lost-device path in
	// Process() already uses.
	m_pyBackground.ReleaseCharacterShadowTexture();

	if (!m_grpDevice.ResizeBackBuffer(uWidth, uHeight))
	{
		// Reset failed: keep the old logical size; the lost-device handling in
		// Process() will retry restoring the device on the next frame.
		m_pyBackground.CreateCharacterShadowTexture();
		TraceError("__OnWindowSizeChanged: ResizeBackBuffer(%u, %u) failed", uWidth, uHeight);
		return;
	}

	m_pyBackground.CreateCharacterShadowTexture();

	m_dwWidth  = uWidth;
	m_dwHeight = uHeight;

	// Keep the UI layer in sync:
	// - SetResolution: physical client size (basis of the mouse mapping)
	// - SetScreenSize: physical size too; the window manager derives its virtual
	//   UI size from it (divides by the UI scale, see ELEMENTIA-UISCALE there).
	// NOTE: already-created Python windows keep their positions (anchored
	// top-left); only the layer sizes and center calculations use the new size.
	m_kWndMgr.SetResolution(int(uWidth), int(uHeight));
	m_kWndMgr.SetScreenSize(long(uWidth), long(uHeight));
}

LRESULT CPythonApplication::WindowProcedure(HWND hWnd, UINT uiMsg, WPARAM wParam, LPARAM lParam)
{
	const int c_DoubleClickTime = 300;
	const int c_DoubleClickBox = 5;
	static int s_xDownPosition = 0;
	static int s_yDownPosition = 0;	

	switch (uiMsg)
	{
		case WM_ACTIVATEAPP:
			{
				m_isActivateWnd = (wParam == WA_ACTIVE) || (wParam == WA_CLICKACTIVE);

				if (m_isActivateWnd)
				{
					m_SoundEngine.RestoreVolume();

					//////////////////

					if (m_isWindowFullScreenEnable)
					{
						__SetFullScreenWindow(hWnd, m_dwWidth, m_dwHeight, m_pySystem.GetBPP());
					}
				}
				else
				{
					m_SoundEngine.SaveVolume(m_isMinimizedWnd);

					//////////////////

					if (m_isWindowFullScreenEnable)
					{
						__MinimizeFullScreenWindow(hWnd, m_dwWidth, m_dwHeight);
					}

					if (IsUserMovingMainWindow())
					{
						SetUserMovingMainWindow(false);
					}
				}
			}
			break;

		case WM_INPUTLANGCHANGE:
			return CPythonIME::Instance().WMInputLanguage(hWnd, uiMsg, wParam, lParam);
			break;

		case WM_IME_STARTCOMPOSITION:
			return CPythonIME::Instance().WMStartComposition(hWnd, uiMsg, wParam, lParam);
			break;

		case WM_IME_COMPOSITION:
			return CPythonIME::Instance().WMComposition(hWnd, uiMsg, wParam, lParam);
			break;

		case WM_IME_ENDCOMPOSITION:
			return CPythonIME::Instance().WMEndComposition(hWnd, uiMsg, wParam, lParam);
			break;

		case WM_IME_NOTIFY:
			return CPythonIME::Instance().WMNotify(hWnd, uiMsg, wParam, lParam);
			break;

		case WM_IME_SETCONTEXT:
			lParam &= ~(ISC_SHOWUICOMPOSITIONWINDOW | ISC_SHOWUIALLCANDIDATEWINDOW);
			break;

		case WM_CHAR:
			return CPythonIME::Instance().WMChar(hWnd, uiMsg, wParam, lParam);
			break;

		case WM_KEYDOWN:
			if (wParam == VK_ESCAPE && IsUserMovingMainWindow())
				SetUserMovingMainWindow(false);
			OnIMEKeyDown(LOWORD(wParam));
			break;

		case WM_LBUTTONDOWN:
			SafeSetCapture();

			if (ELTimer_GetMSec() - m_dwLButtonDownTime < c_DoubleClickTime &&
				abs(LOWORD(lParam) - s_xDownPosition) < c_DoubleClickBox &&
				abs(HIWORD(lParam) - s_yDownPosition) < c_DoubleClickBox)
			{
				m_dwLButtonDownTime = 0;

				OnMouseLeftButtonDoubleClick(short(LOWORD(lParam)), short(HIWORD(lParam)));
			}
			else
			{
				m_dwLButtonDownTime = ELTimer_GetMSec();

				OnMouseLeftButtonDown(short(LOWORD(lParam)), short(HIWORD(lParam)));
			}

			s_xDownPosition = LOWORD(lParam);
			s_yDownPosition = HIWORD(lParam);

			if (IsUserMovingMainWindow())
				SetUserMovingMainWindow(false);
			return 0;

		case WM_LBUTTONUP:
			m_dwLButtonUpTime = ELTimer_GetMSec();

			if (hWnd == GetCapture())
			{
				SafeReleaseCapture();
				OnMouseLeftButtonUp(short(LOWORD(lParam)), short(HIWORD(lParam)));
			}
			return 0;

		case WM_MBUTTONDOWN:
			SafeSetCapture();

			UI::CWindowManager::Instance().RunMouseMiddleButtonDown(short(LOWORD(lParam)), short(HIWORD(lParam)));
//			OnMouseMiddleButtonDown(short(LOWORD(lParam)), short(HIWORD(lParam)));
			break;

		case WM_MBUTTONUP:
			if (GetCapture() == hWnd)
			{
				SafeReleaseCapture();

				UI::CWindowManager::Instance().RunMouseMiddleButtonUp(short(LOWORD(lParam)), short(HIWORD(lParam)));
//				OnMouseMiddleButtonUp(short(LOWORD(lParam)), short(HIWORD(lParam)));
			}
			break;

		case WM_RBUTTONDOWN:
			SafeSetCapture();
			OnMouseRightButtonDown(short(LOWORD(lParam)), short(HIWORD(lParam)));
			return 0;

		case WM_RBUTTONUP:
			if (hWnd == GetCapture()) 
			{
				SafeReleaseCapture();

				OnMouseRightButtonUp(short(LOWORD(lParam)), short(HIWORD(lParam)));
			}
			return 0;

		case 0x20a:
			if (CPythonApplication::Instance().IsWebPageMode())
			{
				// 웹브라우저 상태일때는 휠 작동 안되도록 처리
			}
			else
			{
				OnMouseWheel(short(HIWORD(wParam)));
			}
			break;

		// ELEMENTIA-RESIZE: reworked size handling.
		// - fixes the vanilla height bug (rcWnd.bottom - rcWnd.left)
		// - during an interactive drag-resize (WM_ENTERSIZEMOVE..WM_EXITSIZEMOVE)
		//   the device reset is deferred to WM_EXITSIZEMOVE so we do not Reset()
		//   the device on every intermediate WM_SIZE tick
		// - maximize/restore (which arrive as WM_SIZE outside a size-move loop)
		//   are applied immediately
		case WM_SIZE:
			switch (wParam)
			{
				case SIZE_RESTORED:
				case SIZE_MAXIMIZED:
					if (!m_isWindowSizing)
						__OnWindowSizeChanged();
					break;
			}

			if (wParam==SIZE_MINIMIZED)
				m_isMinimizedWnd=true;
			else
				m_isMinimizedWnd=false;

			OnSizeChange(short(LOWORD(lParam)), short(HIWORD(lParam)));

			break;

		case WM_ENTERSIZEMOVE:	// ELEMENTIA-RESIZE
			m_isWindowSizing = true;
			break;

		case WM_EXITSIZEMOVE:	// ELEMENTIA-RESIZE
			m_isWindowSizing = false;
			__OnWindowSizeChanged();
			OnSizeChange(short(LOWORD(lParam)), short(HIWORD(lParam)));
			break;

		// ELEMENTIA-RESIZE: keep the client area from being dragged below a
		// playable minimum; tiny/zero backbuffers break the renderer and the UI.
		case WM_GETMINMAXINFO:
			if (m_isWindowed && CPythonSystem::Instance().IsWindowResizeEnabled())
			{
				RECT rcMin = { 0, 0, 640, 480 };
				AdjustWindowRectEx(&rcMin, GetWindowLong(hWnd, GWL_STYLE), FALSE, GetWindowLong(hWnd, GWL_EXSTYLE));

				MINMAXINFO* pMinMax = reinterpret_cast<MINMAXINFO*>(lParam);
				pMinMax->ptMinTrackSize.x = rcMin.right - rcMin.left;
				pMinMax->ptMinTrackSize.y = rcMin.bottom - rcMin.top;
				return 0;
			}
			break;
		case WM_NCLBUTTONDOWN:
			{
				switch (wParam)
				{
				case HTMAXBUTTON:
					// ELEMENTIA-RESIZE: with the resizable frame enabled, let the
					// maximize button do its default job (vanilla swallows it).
					if (m_isWindowed && CPythonSystem::Instance().IsWindowResizeEnabled())
						break;
					return 0;
				case HTSYSMENU:
					return 0;
				case HTMINBUTTON:
					ShowWindow(hWnd, SW_MINIMIZE);
					return 0;
				case HTCLOSE:
					RunPressExitKey();
					return 0;
				case HTCAPTION:
					if (!IsUserMovingMainWindow())
						SetUserMovingMainWindow(true);
		
					return 0;
				}
		
				break;
			}
			
		case WM_NCLBUTTONUP:
			{
				if (IsUserMovingMainWindow())
					SetUserMovingMainWindow(false);
				
				break;
			}
		
		case WM_NCRBUTTONDOWN:
		case WM_NCRBUTTONUP:
		case WM_CONTEXTMENU:
			return 0;
		case WM_SYSCOMMAND:
			if (wParam == SC_KEYMENU)
				return 0;
			break;
		case WM_SYSKEYDOWN:
			switch (LOWORD(wParam))
			{
				case VK_F10:
					break;
			}
			break;

		case WM_SYSKEYUP:
			switch(LOWORD(wParam))
			{
				case 18:
					return FALSE;
					break;
				case VK_F10:
					break;
			}
			break;

		case WM_SETCURSOR:
			if (IsActive())
			{
				if (m_bCursorVisible && CURSOR_MODE_HARDWARE == m_iCursorMode)
				{
					SetCursor((HCURSOR) m_hCurrentCursor);
					return 0;
				}
				else
				{
					SetCursor(NULL);
					return 0;
				}
			}
			break;

		case WM_CLOSE:
#ifdef _DEBUG
			PostQuitMessage(0);
#else	
			RunPressExitKey();
#endif
			return 0;

		case WM_DESTROY:
			return 0;
		default:
			//Tracenf("%x msg %x", timeGetTime(), uiMsg);
			break;
	}	

	return CMSApplication::WindowProcedure(hWnd, uiMsg, wParam, lParam);
}
