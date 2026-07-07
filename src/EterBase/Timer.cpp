#include "StdAfx.h"
#include "Timer.h"

static LARGE_INTEGER gs_liTickCountPerSec;
static DWORD gs_dwBaseTime=0;
static DWORD gs_dwServerTime=0;
static DWORD gs_dwClientTime=0;
static DWORD gs_dwFrameTime=0;

#pragma comment(lib, "winmm.lib")

BOOL ELTimer_Init()
{	
	/*
	gs_liTickCountPerSec.QuadPart=0;

	if (!QueryPerformanceFrequency(&gs_liTickCountPerSec))
		return 0;

	LARGE_INTEGER liTickCount;
	QueryPerformanceCounter(&liTickCount);
	gs_dwBaseTime= (liTickCount.QuadPart*1000  / gs_liTickCountPerSec.QuadPart);	
	*/
	gs_dwBaseTime = timeGetTime();
	return 1;
}

DWORD ELTimer_GetMSec()
{
	//assert(gs_dwBaseTime!=0 && "ELTimer_Init 를 먼저 실행하세요");
	//LARGE_INTEGER liTickCount;
	//QueryPerformanceCounter(&liTickCount);
	return timeGetTime() - gs_dwBaseTime; //(liTickCount.QuadPart*1000  / gs_liTickCountPerSec.QuadPart)-gs_dwBaseTime;		
}

VOID	ELTimer_SetServerMSec(DWORD dwServerTime)
{
	NANOBEGIN
	if (0 != dwServerTime) // nanomite를 위한 더미 if
	{
		gs_dwServerTime = dwServerTime;
		gs_dwClientTime = CTimer::instance().GetCurrentMillisecond();
	}
	NANOEND
}

DWORD	ELTimer_GetServerMSec()
{
	return CTimer::instance().GetCurrentMillisecond() - gs_dwClientTime + gs_dwServerTime;
	//return ELTimer_GetMSec() - gs_dwClientTime + gs_dwServerTime;
}

DWORD	ELTimer_GetFrameMSec()
{
	return gs_dwFrameTime;
}

DWORD	ELTimer_GetServerFrameMSec()
{
	return ELTimer_GetFrameMSec() - gs_dwClientTime + gs_dwServerTime;
}

VOID	ELTimer_SetFrameMSec()
{
	gs_dwFrameTime = ELTimer_GetMSec();
}

CTimer::CTimer()
{
	ELTimer_Init();

	NANOBEGIN
	if (this) // nanomite를 위한 더미 if
	{
		m_dwCurrentTime = 0;
		m_bUseRealTime = true;
		m_index = 0;
	
		m_dwElapsedTime = 0;

		m_fCurrentTime = 0.0f;
	}
	NANOEND
}

CTimer::~CTimer()
{
}

void CTimer::SetBaseTime()
{
	m_dwCurrentTime = 0;
}

// ELEMENTIA: fixed-step accumulator. Drains real elapsed time into whole ~16.67ms
// game steps. Crash-safe: real elapsed is clamped (post alt-tab / stall) and the
// backlog is capped so we never spiral or emit a huge single game step.
void CTimer::SetFixedStepUnlock(bool bEnable)
{
	if (m_bFixedStepUnlock == bEnable)
		return;

	m_bFixedStepUnlock = bEnable;
	m_dStepAccumMS = 0.0;
	m_dwStepLastRealMS = 0;
	m_bStepFrame = true;
}

void CTimer::Advance()
{
	if (m_bFixedStepUnlock)   // ELEMENTIA: decoupled render / fixed game step
	{
		const double dFixedStep = 1000.0 / 60.0;   // ~16.667ms per game tick

		DWORD dwNow = ELTimer_GetMSec();
		if (m_dwStepLastRealMS == 0)
			m_dwStepLastRealMS = dwNow;

		double dReal = double(dwNow - m_dwStepLastRealMS);
		m_dwStepLastRealMS = dwNow;

		if (dReal > 250.0)          // clamp after a long stall (crash/spiral guard)
			dReal = 250.0;

		m_dStepAccumMS += dReal;

		if (m_dStepAccumMS >= dFixedStep)
		{
			m_dStepAccumMS -= dFixedStep;
			if (m_dStepAccumMS > dFixedStep * 4.0)   // cap backlog
				m_dStepAccumMS = dFixedStep * 4.0;

			// keep the original 16/17ms alternating cadence for parity with vanilla
			++m_index;
			if (m_index == 1)
				m_index = -1;

			DWORD dwStepMS = 16 + (m_index & 1);
			m_dwCurrentTime += dwStepMS;
			m_fCurrentTime = m_dwCurrentTime / 1000.0f;
			m_dwElapsedTime = dwStepMS;
			m_bStepFrame = true;
		}
		else
		{
			m_dwElapsedTime = 0;    // render-only frame: freeze game time
			m_bStepFrame = false;
		}
		return;
	}

	if (!m_bUseRealTime)
	{
		++m_index;

		if (m_index == 1)
			m_index = -1;

		m_dwCurrentTime += 16 + (m_index & 1);
		m_fCurrentTime = m_dwCurrentTime / 1000.0f;
	}
	else
	{
		DWORD currentTime = ELTimer_GetMSec();

		if (m_dwCurrentTime == 0)
			m_dwCurrentTime = currentTime;

		m_dwElapsedTime = currentTime - m_dwCurrentTime;
		m_dwCurrentTime = currentTime;
	}
}

void CTimer::Adjust(int iTimeGap)
{
	m_dwCurrentTime += iTimeGap;
}

float CTimer::GetCurrentSecond()
{
	if (m_bUseRealTime)
		return ELTimer_GetMSec() / 1000.0f;

	return m_fCurrentTime;
}

DWORD CTimer::GetCurrentMillisecond()
{
	if (m_bUseRealTime)
		return ELTimer_GetMSec();

	return m_dwCurrentTime;
}

float CTimer::GetElapsedSecond()
{
	return GetElapsedMilliecond() / 1000.0f;
}

DWORD CTimer::GetElapsedMilliecond()
{
	// ELEMENTIA: in fixed-step-unlock mode m_dwElapsedTime is authoritative
	// (0 on render-only frames, ~16/17ms on game-step frames).
	if (m_bFixedStepUnlock)
		return m_dwElapsedTime;

	if (!m_bUseRealTime)
		return 16 + (m_index & 1);

	return m_dwElapsedTime;
}

void CTimer::UseCustomTime()
{
	m_bUseRealTime = false;
}
