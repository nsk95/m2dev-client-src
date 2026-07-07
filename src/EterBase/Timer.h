#pragma once

#include <windows.h>
#include "Singleton.h"

class CTimer : public CSingleton<CTimer>
{
	public:
		CTimer();
		virtual ~CTimer();

		void	Advance();
		void	Adjust(int iTimeGap);
		void	SetBaseTime();

		float	GetCurrentSecond();
		DWORD	GetCurrentMillisecond();

		float	GetElapsedSecond();
		DWORD	GetElapsedMilliecond();

		void	UseCustomTime();

		// ELEMENTIA: FPS-unlock via a proper fixed-step accumulator (StepTimer pattern,
		// metin2.dev topic 34198). When enabled the game logic keeps advancing on a
		// fixed ~60Hz timestep (deterministic, no speed-up) while the render loop is
		// allowed to run faster. On render-only frames GetElapsedMilliecond() returns 0
		// so no game state moves -> gameplay speed stays correct at any framerate.
		void	SetFixedStepUnlock(bool bEnable);   // ELEMENTIA
		bool	IsFixedStepUnlock() const { return m_bFixedStepUnlock; }   // ELEMENTIA
		bool	IsStepFrame() const { return m_bStepFrame; }               // ELEMENTIA (true when a fixed game step advanced this frame)

	protected:
		bool	m_bUseRealTime;
		DWORD	m_dwBaseTime;
		DWORD	m_dwCurrentTime;
		float	m_fCurrentTime;
		DWORD	m_dwElapsedTime;
		int		m_index;

		// ELEMENTIA: fixed-step accumulator state
		bool	m_bFixedStepUnlock = false;
		double	m_dStepAccumMS = 0.0;
		DWORD	m_dwStepLastRealMS = 0;
		bool	m_bStepFrame = true;
};

BOOL	ELTimer_Init();

DWORD	ELTimer_GetMSec();

VOID	ELTimer_SetServerMSec(DWORD dwServerTime);
DWORD	ELTimer_GetServerMSec();

VOID	ELTimer_SetFrameMSec();
DWORD	ELTimer_GetFrameMSec();