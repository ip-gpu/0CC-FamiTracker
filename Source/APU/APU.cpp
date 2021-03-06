/*
** FamiTracker - NES/Famicom sound tracker
** Copyright (C) 2005-2014  Jonathan Liss
**
** 0CC-FamiTracker is (C) 2014-2017 HertzDevil
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful, 
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU 
** Library General Public License for more details.  To obtain a 
** copy of the GNU Library General Public License, write to the Free 
** Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
**
** Any permitted reproduction of these routines, in whole or in part,
** must bear this legend.
*/

#include <algorithm>		// // //
#include <vector>
#include "../stdafx.h"
#include <cstdio>
#include <memory>
#include <cmath>
#include "APU.h"
#include "2A03.h"		// // //
#include "VRC6.h"
#include "MMC5.h"
#include "FDS.h"
#include "N163.h"
#include "VRC7.h"
#include "S5B.h"
#include "../RegisterState.h"		// // //

const int		CAPU::SEQUENCER_FREQUENCY	= 240;		// // //
const uint32_t	CAPU::BASE_FREQ_NTSC		= 1789773;		// 72.667
const uint32_t	CAPU::BASE_FREQ_PAL			= 1662607;
const uint8_t	CAPU::FRAME_RATE_NTSC		= 60;
const uint8_t	CAPU::FRAME_RATE_PAL		= 50;

const uint8_t CAPU::LENGTH_TABLE[] = {
	0x0A, 0xFE, 0x14, 0x02, 0x28, 0x04, 0x50, 0x06,
	0xA0, 0x08, 0x3C, 0x0A, 0x0E, 0x0C, 0x1A, 0x0E,
	0x0C, 0x10, 0x18, 0x12, 0x30, 0x14, 0x60, 0x16,
	0xC0, 0x18, 0x48, 0x1A, 0x10, 0x1C, 0x20, 0x1E
};

static std::vector<CSoundChip*> ExChips;

CAPU::CAPU(IAudioCallback *pCallback) :		// // //
	m_pParent(pCallback),
	m_iFrameCycles(0),
	m_pSoundBuffer(NULL),
	m_pMixer(new CMixer()),
	m_iExternalSoundChip(0),
	m_iCyclesToRun(0),
	m_iSampleRate(44100)		// // //
{
	m_p2A03 = new C2A03(m_pMixer);		// // //
	m_pMMC5 = new CMMC5(m_pMixer);
	m_pVRC6 = new CVRC6(m_pMixer);
	m_pVRC7 = new CVRC7(m_pMixer);
	m_pFDS  = new CFDS(m_pMixer);
	m_pN163 = new CN163(m_pMixer);
	m_pS5B  = new CS5B(m_pMixer);

	m_fLevelVRC7 = 1.0f;

#ifdef LOGGING
	m_pLog = new CFile("apu_log.txt", CFile::modeCreate | CFile::modeWrite);
	m_iFrame = 0;
#endif
}

CAPU::~CAPU()
{
	SAFE_RELEASE(m_p2A03);		// // //
	SAFE_RELEASE(m_pMMC5);
	SAFE_RELEASE(m_pVRC6);
	SAFE_RELEASE(m_pVRC7);
	SAFE_RELEASE(m_pFDS);
	SAFE_RELEASE(m_pN163);
	SAFE_RELEASE(m_pS5B);

	SAFE_RELEASE(m_pMixer);

	SAFE_RELEASE(m_pSoundBuffer);

#ifdef LOGGING
	m_pLog->Close();
	delete m_pLog;
#endif
}

// The main APU emulation
//
// The amount of cycles that will be emulated is added by CAPU::AddCycles
//
void CAPU::Process()
{	
	while (m_iCyclesToRun > 0) {

		uint32_t Time = m_iCyclesToRun;
		Time = std::min(Time, m_iSequencerNext - m_iSequencerClock);		// // //
		Time = std::min(Time, m_iFrameClock);

		for (auto Chip : ExChips)		// // //
			Chip->Process(Time);

		m_iFrameCycles	  += Time;
		m_iSequencerClock += Time;
		m_iFrameClock	  -= Time;
		m_iCyclesToRun	  -= Time;

		if (m_iSequencerClock == m_iSequencerNext)
			StepSequence();		// // //

		if (m_iFrameClock == 0)
			EndFrame();
	}
}

void CAPU::StepSequence()		// // //
{
	if (++m_iSequencerCount == SEQUENCER_FREQUENCY)
		m_iSequencerClock = m_iSequencerCount = 0;
	m_iSequencerNext = (uint64_t)BASE_FREQ_NTSC * (m_iSequencerCount + 1) / SEQUENCER_FREQUENCY;
	m_p2A03->ClockSequence();
	m_pMMC5->ClockSequence();		// // //
}

// End of audio frame, flush the buffer if enough samples has been produced, and start a new frame
void CAPU::EndFrame()
{
	// The APU will always output audio in 32 bit signed format
	
	for (auto Chip : ExChips)		// // //
		Chip->EndFrame();

	int SamplesAvail = m_pMixer->FinishBuffer(m_iFrameCycles);
	int ReadSamples	= m_pMixer->ReadBuffer(SamplesAvail, m_pSoundBuffer, m_bStereoEnabled);
	if (m_pParent)		// // //
		m_pParent->FlushBuffer(m_pSoundBuffer, ReadSamples);
	
	m_iFrameClock /*+*/= m_iFrameCycleCount;
	m_iFrameCycles = 0;

	for (auto &r : ExChips)		// // //
		r->GetRegisterLogger()->Step();
		
#ifdef LOGGING
	++m_iFrame;
#endif
}

void CAPU::Reset()
{
	// Reset APU
	//
	
	m_iSequencerCount	= 0;		// // //
	m_iSequencerClock	= 0;		// // //
	m_iSequencerNext	= BASE_FREQ_NTSC / SEQUENCER_FREQUENCY;
	
	m_iCyclesToRun		= 0;
	m_iFrameCycles		= 0;
	m_iFrameClock		= m_iFrameCycleCount;
	
	m_pMixer->ClearBuffer();
	
	for (auto Chip : ExChips) {		// // //
		Chip->GetRegisterLogger()->Reset();
		Chip->Reset();
	}

#ifdef LOGGING
	m_iFrame = 0;
#endif
}

void CAPU::SetupMixer(int LowCut, int HighCut, int HighDamp, int Volume) const
{
	// New settings
	m_pMixer->UpdateSettings(LowCut, HighCut, HighDamp, float(Volume) / 100.0f);
	m_pVRC7->SetVolume((float(Volume) / 100.0f) * m_fLevelVRC7);
}

// // //
void CAPU::SetCallback(IAudioCallback &pCallback) {
	m_pParent = &pCallback;
}

void CAPU::SetExternalSound(uint8_t Chip)
{
	// Set expansion chip
	m_iExternalSoundChip = Chip;
	m_pMixer->ExternalSound(Chip);

	ExChips.clear();

	ExChips.push_back(m_p2A03);		// // //
	if (Chip & SNDCHIP_VRC6)
		ExChips.push_back(m_pVRC6);
	if (Chip & SNDCHIP_VRC7)
		ExChips.push_back(m_pVRC7);
	if (Chip & SNDCHIP_FDS)
		ExChips.push_back(m_pFDS);
	if (Chip & SNDCHIP_MMC5)
		ExChips.push_back(m_pMMC5);
	if (Chip & SNDCHIP_N163)
		ExChips.push_back(m_pN163);
	if (Chip & SNDCHIP_S5B)
		ExChips.push_back(m_pS5B);

	Reset();
}

void CAPU::ChangeMachineRate(int Machine, int Rate)		// // //
{
	// Allow to change speed on the fly
	//
	
	uint32_t BaseFreq = (Machine == MACHINE_NTSC) ? BASE_FREQ_NTSC : BASE_FREQ_PAL;
	m_p2A03->ChangeMachine(Machine);

	m_pVRC7->SetSampleSpeed(m_iSampleRate, BaseFreq, Rate);
	m_iFrameCycleCount = BaseFreq / Rate;
}

bool CAPU::SetupSound(int SampleRate, int NrChannels, int Machine)		// // //
{
	// Allocate a sound buffer
	//
	// Returns false if a buffer couldn't be allocated
	//
	
	uint32_t BaseFreq = (Machine == MACHINE_NTSC) ? BASE_FREQ_NTSC : BASE_FREQ_PAL;
	uint8_t FrameRate = (Machine == MACHINE_NTSC) ? FRAME_RATE_NTSC : FRAME_RATE_PAL;
	m_iSampleRate = SampleRate;		// // //

	m_iSoundBufferSamples = uint32_t(m_iSampleRate / FRAME_RATE_PAL);	// Samples / frame. Allocate for PAL, since it's more
	m_bStereoEnabled	  = (NrChannels == 2);	
	m_iSoundBufferSize	  = m_iSoundBufferSamples * NrChannels;		// Total amount of samples to allocate
	m_iSampleSizeShift	  = (NrChannels == 2) ? 1 : 0;
	m_iBufferPointer	  = 0;

	if (!m_pMixer->AllocateBuffer(m_iSoundBufferSamples, m_iSampleRate, NrChannels))		// // //
		return false;

	m_pMixer->SetClockRate(BaseFreq);

	SAFE_RELEASE_ARRAY(m_pSoundBuffer);

	m_pSoundBuffer = new int16_t[m_iSoundBufferSize << 1];

	if (m_pSoundBuffer == NULL)
		return false;

	ChangeMachineRate(Machine, FrameRate);		// // //

	return true;
}

void CAPU::AddTime(int32_t Cycles)
{
	if (Cycles < 0)
		return;
	m_iCyclesToRun += Cycles;
}

void CAPU::Write(uint16_t Address, uint8_t Value)
{
	// Data was written to an external sound chip

	Process();
	
	for (auto Chip : ExChips)		// // //
		Chip->Write(Address, Value);

	LogWrite(Address, Value);
}

uint8_t CAPU::Read(uint16_t Address)
{
	// Data read from an external chip
	//

	uint8_t Value(0);
	bool Mapped(false);

	Process();
	
	for (auto Chip : ExChips)		// // //
		if (!Mapped)
			Value = Chip->Read(Address, Mapped);

	if (!Mapped)
		Value = Address >> 8;	// open bus

	return Value;
}

// Expansion for famitracker

int32_t CAPU::GetVol(uint8_t Chan) const	
{
	return m_pMixer->GetChanOutput(Chan);
}

uint8_t CAPU::GetSamplePos() const
{
	return m_p2A03->GetSamplePos();
}

uint8_t CAPU::GetDeltaCounter() const
{
	return m_p2A03->GetDeltaCounter();
}

bool CAPU::DPCMPlaying() const
{
	return m_p2A03->DPCMPlaying();
}

void CAPU::WriteSample(const char *pBuf, int Size)		// // //
{
	m_p2A03->GetSampleMemory()->SetMem(pBuf, Size);
}

void CAPU::ClearSample()		// // //
{
	m_p2A03->GetSampleMemory()->Clear();
}

#ifdef LOGGING
void CAPU::Log()
{
	CString str;
	str.Format("Frame %08i: ", m_iFrame - 1);
	str.Append("2A03 ");
	for (int i = 0; i < 0x14; ++i)
		str.AppendFormat("%02X ", GetReg(SNDCHIP_NONE, i));
	if (m_iExternalSoundChip & SNDCHIP_VRC6) {		// // //
		str.Append("VRC6 ");
		for (int i = 0; i < 0x03; ++i) for (int j = 0; j < 0x03; ++j)
			str.AppendFormat("%02X ", GetReg(SNDCHIP_VRC6, 0x9000 + i * 0x1000 + j));
	}
	if (m_iExternalSoundChip & SNDCHIP_MMC5) {
		str.Append("MMC5 ");
		for (int i = 0; i < 0x08; ++i)
			str.AppendFormat("%02X ", GetReg(SNDCHIP_MMC5, 0x5000 + i));
	}
	if (m_iExternalSoundChip & SNDCHIP_N163) {
		str.Append("N163 ");
		for (int i = 0; i < 0x80; ++i)
			str.AppendFormat("%02X ", GetReg(SNDCHIP_N163, i));
	}
	if (m_iExternalSoundChip & SNDCHIP_FDS) {
		str.Append("FDS ");
		for (int i = 0; i < 0x0B; ++i)
			str.AppendFormat("%02X ", GetReg(SNDCHIP_FDS, 0x4080 + i));
	}
	if (m_iExternalSoundChip & SNDCHIP_VRC7) {
		str.Append("VRC7 ");
		for (int i = 0; i < 0x40; ++i)
			str.AppendFormat("%02X ", GetReg(SNDCHIP_VRC7, i));
	}
	if (m_iExternalSoundChip & SNDCHIP_S5B) {
		str.Append("S5B ");
		for (int i = 0; i < 0x10; ++i)
			str.AppendFormat("%02X ", GetReg(SNDCHIP_S5B, i));
	}
	str.Append("\r\n");
	m_pLog->Write(str, str.GetLength());
}
#endif

void CAPU::SetChipLevel(chip_level_t Chip, float Level)
{
	float fLevel = powf(10, Level / 20.0f);		// Convert dB to linear

	switch (Chip) {
		case CHIP_LEVEL_VRC7:
			m_fLevelVRC7 = fLevel;
			break;
			// // // 050B
		default:
			m_pMixer->SetChipLevel(Chip, fLevel);
	}
}

void CAPU::SetNamcoMixing(bool bLinear)		// // //
{
	m_pMixer->SetNamcoMixing(bLinear);
	m_pN163->SetMixingMethod(bLinear);
}

void CAPU::SetMeterDecayRate(int Type) const		// // // 050B
{
	m_pMixer->SetMeterDecayRate(Type);
}

int CAPU::GetMeterDecayRate() const		// // // 050B
{
	return m_pMixer->GetMeterDecayRate();
}

void CAPU::LogWrite(uint16_t Address, uint8_t Value)
{
	for (auto &r : ExChips)		// // //
		r->Log(Address, Value);
}

uint8_t CAPU::GetReg(int Chip, int Reg) const
{
	if (auto r = GetRegState(Chip, Reg))		// // //
		return r->GetValue();
	return static_cast<uint8_t>(0);
}

double CAPU::GetFreq(int Chip, int Chan) const
{
	const CSoundChip *pChip = nullptr;
	switch (Chip) {
	case SNDCHIP_NONE: pChip = m_p2A03; break;
	case SNDCHIP_VRC6: pChip = m_pVRC6; break;
	case SNDCHIP_VRC7: pChip = m_pVRC7; break;
	case SNDCHIP_FDS:  pChip = m_pFDS; break;
	case SNDCHIP_MMC5: pChip = m_pMMC5; break;
	case SNDCHIP_N163: pChip = m_pN163; break;
	case SNDCHIP_S5B:  pChip = m_pS5B; break;
	default: AfxDebugBreak(); return 0.;
	}
	return pChip->GetFreq(Chan);
}

CRegisterState *CAPU::GetRegState(int Chip, int Reg) const		// // //
{
	const CSoundChip *pChip = nullptr;
	switch (Chip) {
	case SNDCHIP_NONE: pChip = m_p2A03; break;
	case SNDCHIP_VRC6: pChip = m_pVRC6; break;
	case SNDCHIP_VRC7: pChip = m_pVRC7; break;
	case SNDCHIP_FDS:  pChip = m_pFDS; break;
	case SNDCHIP_MMC5: pChip = m_pMMC5; break;
	case SNDCHIP_N163: pChip = m_pN163; break;
	case SNDCHIP_S5B:  pChip = m_pS5B; break;
	default: AfxDebugBreak();
	}

	return pChip->GetRegisterLogger()->GetRegister(Reg);
}
