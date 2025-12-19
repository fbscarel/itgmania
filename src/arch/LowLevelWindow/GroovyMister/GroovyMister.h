/* GroovyMister.h - GroovyMiSTer protocol library for Linux.
 *
 * This class handles UDP communication with a MiSTer FPGA running the Groovy
 * core, streaming video frames for CRT display.
 *
 * Adapted from:
 * - Groovy_MiSTer/api/groovymister.cpp (original Windows implementation)
 * - MiSTerCast/Library/MiSTerCastLib/groovymister.cpp (clean C++ reference)
 *
 * Protocol reference: https://github.com/psakhis/Groovy_MiSTer
 */

#ifndef GROOVY_MISTER_H
#define GROOVY_MISTER_H

#include "GroovyMisterTypes.h"
#include <string>
#include <vector>
#include <atomic>

class GroovyMister
{
public:
	GroovyMister();
	~GroovyMister();

	// Connection management
	bool Connect(const std::string& ip, uint16_t port = GROOVY_DEFAULT_PORT);
	void Disconnect();
	bool IsConnected() const;
	const std::string& GetIP() const { return m_ip; }
	uint16_t GetPort() const { return m_port; }

	// Session initialization (CMD_INIT)
	// Must be called after Connect() before streaming
	bool CmdInit(GroovyLZ4Mode lz4Mode,
	             GroovyAudioRate audioRate,
	             GroovyAudioChannels audioChannels,
	             GroovyRGBMode rgbMode);

	// Video mode configuration (CMD_SWITCHRES)
	// Sets the modeline for the CRT output
	bool CmdSwitchres(const GroovyModeline& modeline);

	// Frame blitting (CMD_BLIT_VSYNC or CMD_BLIT_FIELD_VSYNC)
	// Sends a frame to the FPGA for display
	// frameData: RGB pixel data (format must match rgbMode from CmdInit)
	// frameSize: Size in bytes
	// frameNum: Sequential frame number
	// vSync: Scanline to sync with (0 = automatic)
	bool CmdBlit(const uint8_t* frameData, size_t frameSize,
	             uint32_t frameNum, uint16_t vSync = 0);

	// Audio streaming (CMD_AUDIO) - stub for future expansion
	// samples: 16-bit signed PCM samples
	// sampleCount: Number of samples (not bytes)
	bool CmdAudio(const int16_t* samples, size_t sampleCount);

	// Status request (CMD_GET_STATUS)
	bool CmdGetStatus();

	// Frame synchronization
	// Waits for ACK from FPGA (provides natural frame pacing)
	// Returns true if ACK received within timeout
	bool WaitSync(uint32_t timeoutMs = 16);

	// Polling interface for non-blocking operation
	bool PollAck();

	// Status accessors
	GroovyStatus GetLastStatus() const;
	uint32_t GetCurrentFrame() const { return m_frameCount; }
	const GroovyModeline& GetCurrentModeline() const { return m_currentMode; }

	// Statistics
	double GetAverageLatencyMs() const;
	uint32_t GetDroppedFrames() const { return m_droppedFrames; }

	// Configuration
	void SetCompressionMode(GroovyLZ4Mode mode) { m_lz4Mode = mode; }
	GroovyLZ4Mode GetCompressionMode() const { return m_lz4Mode; }

private:
	// Socket management
	int m_socket;
	std::string m_ip;
	uint16_t m_port;

	// Connection state
	std::atomic<bool> m_connected;
	bool m_initialized;

	// Frame state
	uint32_t m_frameCount;
	uint32_t m_droppedFrames;
	GroovyStatus m_lastStatus;
	GroovyModeline m_currentMode;

	// Configuration
	GroovyLZ4Mode m_lz4Mode;
	GroovyRGBMode m_rgbMode;
	GroovyAudioRate m_audioRate;
	GroovyAudioChannels m_audioChannels;

	// Buffers
	std::vector<uint8_t> m_sendBuffer;
	std::vector<uint8_t> m_compressBuffer;
	std::vector<uint8_t> m_recvBuffer;

	// Internal helpers
	bool SendPacket(const uint8_t* data, size_t size);
	bool SendFrame(const uint8_t* data, size_t size);
	bool ReceiveAck();
	size_t CompressFrame(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t dstCapacity);

	// Prevent copying
	GroovyMister(const GroovyMister&) = delete;
	GroovyMister& operator=(const GroovyMister&) = delete;
};

#endif // GROOVY_MISTER_H

/*
 * (c) 2024 ITGMania Team
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, and/or sell copies of the Software, and to permit persons to
 * whom the Software is furnished to do so, provided that the above
 * copyright notice(s) and this permission notice appear in all copies of
 * the Software and that both the above copyright notice(s) and this
 * permission notice appear in supporting documentation.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF
 * THIRD PARTY RIGHTS. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR HOLDERS
 * INCLUDED IN THIS NOTICE BE LIABLE FOR ANY CLAIM, OR ANY SPECIAL INDIRECT
 * OR CONSEQUENTIAL DAMAGES, OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */
