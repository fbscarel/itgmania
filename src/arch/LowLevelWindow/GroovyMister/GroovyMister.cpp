/* GroovyMister.cpp - GroovyMiSTer protocol implementation for Linux.
 *
 * This implements the UDP protocol for streaming video frames to a MiSTer FPGA.
 * Uses POSIX sockets for Linux compatibility.
 *
 * Protocol reference: https://github.com/psakhis/Groovy_MiSTer
 */

#include "global.h"
#include "GroovyMister.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <cerrno>
#include <cstring>
#include <algorithm>

#include <lz4.h>
#include <lz4hc.h>

GroovyMister::GroovyMister()
	: m_socket(-1)
	, m_port(GROOVY_DEFAULT_PORT)
	, m_connected(false)
	, m_initialized(false)
	, m_frameCount(0)
	, m_droppedFrames(0)
	, m_lz4Mode(GroovyLZ4Mode::LZ4)
	, m_rgbMode(GroovyRGBMode::RGB888)
	, m_audioRate(GroovyAudioRate::OFF)
	, m_audioChannels(GroovyAudioChannels::OFF)
{
	m_sendBuffer.resize(GROOVY_MTU_SIZE);
	m_compressBuffer.resize(LZ4_compressBound(GROOVY_MAX_FRAME_SIZE));
	m_recvBuffer.resize(64);
	std::memset(&m_lastStatus, 0, sizeof(m_lastStatus));
	std::memset(&m_currentMode, 0, sizeof(m_currentMode));
}

GroovyMister::~GroovyMister()
{
	Disconnect();
}

bool GroovyMister::Connect(const std::string& ip, uint16_t port)
{
	if (m_connected)
		Disconnect();

	// Create UDP socket
	m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (m_socket < 0)
	{
		return false;
	}

	// Set non-blocking for receives (allows polling)
	int flags = fcntl(m_socket, F_GETFL, 0);
	if (flags >= 0)
	{
		fcntl(m_socket, F_SETFL, flags | O_NONBLOCK);
	}

	// Increase send buffer size for better throughput
	int bufSize = 2 * 1024 * 1024; // 2MB
	setsockopt(m_socket, SOL_SOCKET, SO_SNDBUF, &bufSize, sizeof(bufSize));

	// Connect to MiSTer (allows using send() instead of sendto())
	struct sockaddr_in addr;
	std::memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);

	if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1)
	{
		close(m_socket);
		m_socket = -1;
		return false;
	}

	if (connect(m_socket, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0)
	{
		close(m_socket);
		m_socket = -1;
		return false;
	}

	m_ip = ip;
	m_port = port;
	m_connected = true;
	m_initialized = false;
	m_frameCount = 0;
	m_droppedFrames = 0;

	return true;
}

void GroovyMister::Disconnect()
{
	if (m_socket >= 0)
	{
		// Send close command to MiSTer
		uint8_t closeCmd = CMD_CLOSE;
		send(m_socket, &closeCmd, 1, MSG_NOSIGNAL);

		close(m_socket);
		m_socket = -1;
	}
	m_connected = false;
	m_initialized = false;
}

bool GroovyMister::IsConnected() const
{
	return m_connected;
}

bool GroovyMister::CmdInit(GroovyLZ4Mode lz4Mode,
                           GroovyAudioRate audioRate,
                           GroovyAudioChannels audioChannels,
                           GroovyRGBMode rgbMode)
{
	if (!m_connected)
		return false;

	// Build CMD_INIT packet (5 bytes)
	uint8_t packet[5];
	packet[0] = CMD_INIT;
	packet[1] = static_cast<uint8_t>(lz4Mode);
	packet[2] = static_cast<uint8_t>(audioRate);
	packet[3] = static_cast<uint8_t>(audioChannels);
	packet[4] = static_cast<uint8_t>(rgbMode);

	m_lz4Mode = lz4Mode;
	m_rgbMode = rgbMode;
	m_audioRate = audioRate;
	m_audioChannels = audioChannels;

	if (SendPacket(packet, sizeof(packet)))
	{
		m_initialized = true;
		return true;
	}

	return false;
}

bool GroovyMister::CmdSwitchres(const GroovyModeline& modeline)
{
	if (!m_connected)
		return false;

	// Build CMD_SWITCHRES packet (26 bytes)
	uint8_t packet[26];
	packet[0] = CMD_SWITCHRES;

	// Pack modeline data (little-endian)
	std::memcpy(&packet[1], &modeline.pclock, 8);
	std::memcpy(&packet[9], &modeline.hactive, 2);
	std::memcpy(&packet[11], &modeline.hbegin, 2);
	std::memcpy(&packet[13], &modeline.hend, 2);
	std::memcpy(&packet[15], &modeline.htotal, 2);
	std::memcpy(&packet[17], &modeline.vactive, 2);
	std::memcpy(&packet[19], &modeline.vbegin, 2);
	std::memcpy(&packet[21], &modeline.vend, 2);
	std::memcpy(&packet[23], &modeline.vtotal, 2);
	packet[25] = modeline.interlace;

	if (SendPacket(packet, sizeof(packet)))
	{
		m_currentMode = modeline;
		return true;
	}

	return false;
}

bool GroovyMister::CmdBlit(const uint8_t* frameData, size_t frameSize,
                           uint32_t frameNum, uint16_t vSync)
{
	if (!m_connected || !m_initialized)
		return false;

	const uint8_t* dataToSend = frameData;
	size_t sizeToSend = frameSize;
	uint8_t cmd = CMD_BLIT_VSYNC;
	size_t headerSize = 8;

	// Compress if enabled
	if (m_lz4Mode != GroovyLZ4Mode::RAW)
	{
		size_t compSize = CompressFrame(frameData, frameSize,
		                                m_compressBuffer.data(),
		                                m_compressBuffer.size());

		// Only use compressed data if it's smaller
		if (compSize > 0 && compSize < frameSize)
		{
			dataToSend = m_compressBuffer.data();
			sizeToSend = compSize;
			cmd = CMD_BLIT_FIELD_VSYNC;
			headerSize = 12;
		}
	}

	// Build header
	uint8_t header[12];
	header[0] = cmd;
	std::memcpy(&header[1], &frameNum, 4);
	header[5] = 0; // field (0 = progressive)
	std::memcpy(&header[6], &vSync, 2);

	if (cmd == CMD_BLIT_FIELD_VSYNC)
	{
		uint32_t compSize32 = static_cast<uint32_t>(sizeToSend);
		std::memcpy(&header[8], &compSize32, 4);
	}

	// Send header
	if (!SendPacket(header, headerSize))
	{
		m_droppedFrames++;
		return false;
	}

	// Send frame data
	if (!SendFrame(dataToSend, sizeToSend))
	{
		m_droppedFrames++;
		return false;
	}

	m_frameCount = frameNum;
	return true;
}

bool GroovyMister::CmdBlitDuplicate(uint32_t frameNum, uint16_t vSync)
{
	if (!m_connected || !m_initialized)
		return false;

	// Build 9-byte duplicate header for CMD_BLIT_FIELD_VSYNC
	// Format: cmd(1) + frame(4) + field(1) + vsync(2) + dup_flag(1)
	uint8_t header[9];
	header[0] = CMD_BLIT_FIELD_VSYNC;
	std::memcpy(&header[1], &frameNum, 4);
	header[5] = 0;  // field (0 = progressive)
	std::memcpy(&header[6], &vSync, 2);
	header[8] = 0x01;  // frame_dup flag

	if (!SendPacket(header, 9))
	{
		m_droppedFrames++;
		return false;
	}

	m_frameCount = frameNum;
	return true;
}

bool GroovyMister::CmdAudio(const int16_t* samples, size_t sampleCount)
{
	// STUB: Audio streaming expansion point
	// Implementation follows same pattern as CmdBlit
	// Audio data is sent as 16-bit signed PCM
	//
	// Packet format:
	// [0] = CMD_AUDIO (0x04)
	// [1-2] = sample count (uint16_t)
	// [3...] = PCM data
	//
	// Reference: ~/src/GroovyMAME/src/osd/modules/render/drawnogpu.cpp
	// Search for "CMD_AUDIO" or "add_audio_to_recording"

	(void)samples;
	(void)sampleCount;

	if (!m_connected || !m_initialized)
		return false;

	if (m_audioRate == GroovyAudioRate::OFF)
		return true; // Audio disabled, success

	// TODO: Implement audio streaming
	// This requires integration with ITGMania's audio system
	// See implementation plan section 7 for details

	return true;
}

bool GroovyMister::CmdGetStatus()
{
	if (!m_connected)
		return false;

	uint8_t packet[1] = { CMD_GET_STATUS };
	return SendPacket(packet, sizeof(packet));
}

bool GroovyMister::WaitSync(uint32_t timeoutMs)
{
	if (!m_connected)
		return false;

	struct pollfd pfd;
	pfd.fd = m_socket;
	pfd.events = POLLIN;

	int ret = poll(&pfd, 1, static_cast<int>(timeoutMs));
	if (ret > 0 && (pfd.revents & POLLIN))
	{
		return ReceiveAck();
	}

	return false;
}

bool GroovyMister::PollAck()
{
	if (!m_connected)
		return false;

	// Non-blocking check for ACK
	return ReceiveAck();
}

bool GroovyMister::ReceiveAck()
{
	ssize_t received = recv(m_socket, m_recvBuffer.data(), m_recvBuffer.size(), 0);

	if (received >= 13)
	{
		const uint8_t* data = m_recvBuffer.data();

		// Parse status response (13 bytes)
		std::memcpy(&m_lastStatus.frameEcho, data + 0, 4);
		std::memcpy(&m_lastStatus.vCountEcho, data + 4, 2);
		std::memcpy(&m_lastStatus.fpgaFrame, data + 6, 4);
		std::memcpy(&m_lastStatus.fpgaVCount, data + 10, 2);

		uint8_t flags = data[12];
		m_lastStatus.vramReady = (flags & 0x01) != 0;
		m_lastStatus.vramEndFrame = (flags & 0x02) != 0;
		m_lastStatus.vramSynced = (flags & 0x04) != 0;
		m_lastStatus.vgaVblank = (flags & 0x10) != 0;
		m_lastStatus.audioEnabled = (flags & 0x40) != 0;

		return true;
	}

	return false;
}

GroovyStatus GroovyMister::GetLastStatus() const
{
	return m_lastStatus;
}

double GroovyMister::GetAverageLatencyMs() const
{
	// TODO: Track timing statistics for latency measurement
	// This would require storing timestamps and computing averages
	return 0.0;
}

bool GroovyMister::SendPacket(const uint8_t* data, size_t size)
{
	if (m_socket < 0)
		return false;

	ssize_t sent = send(m_socket, data, size, MSG_NOSIGNAL);
	return sent == static_cast<ssize_t>(size);
}

bool GroovyMister::SendFrame(const uint8_t* data, size_t size)
{
	if (m_socket < 0)
		return false;

	// Send frame data in MTU-sized chunks
	size_t offset = 0;
	while (offset < size)
	{
		size_t chunkSize = std::min(static_cast<size_t>(GROOVY_MTU_SIZE), size - offset);
		ssize_t sent = send(m_socket, data + offset, chunkSize, MSG_NOSIGNAL);

		if (sent < 0)
		{
			// Check for EAGAIN/EWOULDBLOCK (would block on non-blocking socket)
			if (errno == EAGAIN || errno == EWOULDBLOCK)
			{
				// Brief pause and retry
				usleep(100);
				continue;
			}
			return false;
		}

		offset += static_cast<size_t>(sent);
	}

	return true;
}

size_t GroovyMister::CompressFrame(const uint8_t* src, size_t srcSize,
                                   uint8_t* dst, size_t dstCapacity)
{
	int compSize;

	if (m_lz4Mode == GroovyLZ4Mode::LZ4_HC)
	{
		// High compression mode (slower but better ratio)
		compSize = LZ4_compress_HC(
			reinterpret_cast<const char*>(src),
			reinterpret_cast<char*>(dst),
			static_cast<int>(srcSize),
			static_cast<int>(dstCapacity),
			LZ4HC_CLEVEL_DEFAULT
		);
	}
	else
	{
		// Standard LZ4 compression (fast)
		compSize = LZ4_compress_default(
			reinterpret_cast<const char*>(src),
			reinterpret_cast<char*>(dst),
			static_cast<int>(srcSize),
			static_cast<int>(dstCapacity)
		);
	}

	return (compSize > 0) ? static_cast<size_t>(compSize) : 0;
}

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
