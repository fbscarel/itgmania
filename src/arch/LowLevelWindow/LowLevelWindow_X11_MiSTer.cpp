/* LowLevelWindow_X11_MiSTer.cpp - X11 window driver with GroovyMiSTer CRT output.
 *
 * This extends the standard X11 window driver to stream video frames to a MiSTer
 * FPGA for CRT display output.
 */

#include "global.h"
#include "LowLevelWindow_X11_MiSTer.h"
#include "PrefsManager.h"
#include "RageLog.h"
#include "RageDisplay.h"

#include <GL/gl.h>
#include <GL/glext.h>
#include <cstring>

// GL_BGR is part of GL_EXT_bgra, define if not available
#ifndef GL_BGR
#define GL_BGR 0x80E0
#endif

LowLevelWindow_X11_MiSTer::LowLevelWindow_X11_MiSTer()
	: LowLevelWindow_X11()
	, m_groovy(std::make_unique<GroovyMister>())
	, m_hasPreviousFrame(false)
	, m_frameNumber(0)
	, m_captureWidth(0)
	, m_captureHeight(0)
	, m_interlacedFB(true)  // Default to interlace=1, will be overridden by preferences
	, m_currentField(0)
	, m_localDisplayEnabled(true)
	, m_misterInitialized(false)
	, m_compressionMode(GroovyLZ4Mode::LZ4)
	, m_overscanPercent(0)
	, m_deflickerMode(0)
{
	m_hasPreviousField[0] = false;
	m_hasPreviousField[1] = false;
	std::memset(&m_lastBlitStatus, 0, sizeof(m_lastBlitStatus));
	LOG->Info("LowLevelWindow_X11_MiSTer: Initializing MiSTer output driver");
}

LowLevelWindow_X11_MiSTer::~LowLevelWindow_X11_MiSTer()
{
	DisconnectFromMiSTer();
}

bool LowLevelWindow_X11_MiSTer::ConnectToMiSTer(const std::string& ip, uint16_t port)
{
	if (m_groovy->IsConnected())
		DisconnectFromMiSTer();

	if (!m_groovy->Connect(ip, port))
	{
		LOG->Warn("LowLevelWindow_X11_MiSTer: Failed to connect to MiSTer at %s:%d",
		          ip.c_str(), port);
		return false;
	}

	LOG->Info("LowLevelWindow_X11_MiSTer: Connected to MiSTer at %s:%d",
	          ip.c_str(), port);
	return true;
}

void LowLevelWindow_X11_MiSTer::DisconnectFromMiSTer()
{
	if (m_groovy->IsConnected())
	{
		m_groovy->Disconnect();
		m_misterInitialized = false;
		LOG->Info("LowLevelWindow_X11_MiSTer: Disconnected from MiSTer");
	}
}

bool LowLevelWindow_X11_MiSTer::IsMiSTerConnected() const
{
	return m_groovy->IsConnected();
}

void LowLevelWindow_X11_MiSTer::SetCompressionMode(GroovyLZ4Mode mode)
{
	m_compressionMode = mode;
	if (m_groovy)
		m_groovy->SetCompressionMode(mode);
}

GroovyLZ4Mode LowLevelWindow_X11_MiSTer::GetCompressionMode() const
{
	return m_compressionMode;
}

uint32_t LowLevelWindow_X11_MiSTer::GetDroppedFrames() const
{
	return m_groovy ? m_groovy->GetDroppedFrames() : 0;
}

void LowLevelWindow_X11_MiSTer::InitializeMiSTerFromPreferences()
{
	// Read configuration from preferences
	// These preferences will be added to PrefsManager
	bool misterEnabled = PREFSMAN->m_bMiSTerEnable.Get();
	RString misterIP = PREFSMAN->m_sMiSTerIP.Get();
	int compressionMode = PREFSMAN->m_iMiSTerCompression.Get();
	m_localDisplayEnabled = PREFSMAN->m_bMiSTerLocalDisplay.Get();
	m_overscanPercent = PREFSMAN->m_iMiSTerOverscan.Get();
	m_deflickerMode = PREFSMAN->m_iMiSTerDeflicker.Get();
	m_interlacedFB = PREFSMAN->m_bMiSTerInterlacedFB.Get();

	// Clamp overscan to valid range (0-20%)
	if (m_overscanPercent < 0) m_overscanPercent = 0;
	if (m_overscanPercent > 20) m_overscanPercent = 20;

	// Clamp deflicker to valid range (0-2)
	if (m_deflickerMode < 0) m_deflickerMode = 0;
	if (m_deflickerMode > 2) m_deflickerMode = 2;

	if (!misterEnabled)
	{
		LOG->Info("LowLevelWindow_X11_MiSTer: MiSTer output disabled in preferences");
		return;
	}

	LOG->Info("LowLevelWindow_X11_MiSTer: Overscan=%d%%, Deflicker=%d, InterlacedFB=%s",
	          m_overscanPercent, m_deflickerMode, m_interlacedFB ? "true (fields)" : "false (frames)");

	// Set compression mode
	m_compressionMode = static_cast<GroovyLZ4Mode>(compressionMode);

	// Connect to MiSTer
	if (!m_groovy->IsConnected())
	{
		if (!ConnectToMiSTer(misterIP.c_str()))
		{
			LOG->Warn("LowLevelWindow_X11_MiSTer: Could not connect to MiSTer, "
			          "falling back to local display only");
			return;
		}
	}
}

RString LowLevelWindow_X11_MiSTer::TryVideoMode(const VideoModeParams& p, bool& bNewDeviceOut)
{
	// First, set up local X11 window using parent implementation
	RString error = LowLevelWindow_X11::TryVideoMode(p, bNewDeviceOut);
	if (!error.empty())
	{
		return error;
	}

	// Store dimensions for framebuffer capture
	m_captureWidth = p.width;
	m_captureHeight = p.height;

	// Allocate frame buffer (RGB888 = 3 bytes per pixel)
	size_t bufferSize = static_cast<size_t>(m_captureWidth) *
	                    static_cast<size_t>(m_captureHeight) * 3;
	m_frameBuffer.resize(bufferSize);
	m_scaledBuffer.resize(bufferSize);  // Same size for overscan-scaled output
	m_previousFrame.resize(bufferSize); // For frame duplication detection
	m_hasPreviousFrame = false;         // Reset on mode change

	// Allocate field buffer (half height for interlace=1 mode)
	size_t fieldBufferSize = static_cast<size_t>(m_captureWidth) *
	                         static_cast<size_t>(m_captureHeight / 2) * 3;
	m_fieldBuffer.resize(fieldBufferSize);
	m_previousField[0].resize(fieldBufferSize);
	m_previousField[1].resize(fieldBufferSize);
	m_hasPreviousField[0] = false;
	m_hasPreviousField[1] = false;
	m_currentField = 0;

	LOG->Info("LowLevelWindow_X11_MiSTer: Framebuffer allocated for %dx%d (%zu bytes), field buffer %zu bytes",
	          m_captureWidth, m_captureHeight, bufferSize, fieldBufferSize);

	// Initialize MiSTer connection from preferences
	InitializeMiSTerFromPreferences();

	// If connected, initialize streaming session
	if (m_groovy->IsConnected() && !m_misterInitialized)
	{
		// Initialize MiSTer streaming session
		if (!m_groovy->CmdInit(m_compressionMode,
		                       GroovyAudioRate::OFF,     // Audio disabled for now
		                       GroovyAudioChannels::OFF,
		                       GroovyRGBMode::RGB888))
		{
			LOG->Warn("LowLevelWindow_X11_MiSTer: CmdInit failed");
		}
		else
		{
			// Set modeline based on video mode
			GroovyModeline modeline = VideoModeToModeline(p);
			if (!m_groovy->CmdSwitchres(modeline))
			{
				LOG->Warn("LowLevelWindow_X11_MiSTer: CmdSwitchres failed");
			}
			else
			{
				m_misterInitialized = true;
				LOG->Info("LowLevelWindow_X11_MiSTer: MiSTer configured for %dx%d @ %.2f MHz",
				          modeline.hactive, modeline.vactive, modeline.pclock);
			}
		}
	}

	return RString(); // Success
}

void LowLevelWindow_X11_MiSTer::SwapBuffers()
{
	// Capture and send frame to MiSTer if connected
	if (m_groovy->IsConnected() && m_misterInitialized)
	{
		CaptureFramebuffer();

		if (!m_frameBuffer.empty())
		{
			// Apply deflicker filter to reduce interlace flicker on thin lines
			if (m_deflickerMode > 0)
			{
				ApplyDeflickerFilter();
			}

			// Apply overscan scaling if enabled
			const uint8_t* frameBuffer = m_frameBuffer.data();

			if (m_overscanPercent > 0)
			{
				ApplyOverscanScaling();
				frameBuffer = m_scaledBuffer.data();
			}

			bool sent = false;

			// Phase 3: True interlaced mode (interlace=1)
			// Send 240-line fields instead of full 480-line frames = 50% bandwidth reduction
			if (m_interlacedFB && m_captureHeight >= 480)
			{
				// Calculate which field to send based on FPGA status
				int field = CalculateNextField();

				// Extract the appropriate field from the full frame
				ExtractField(frameBuffer, field);

				const uint8_t* fieldData = m_fieldBuffer.data();
				size_t fieldSize = m_fieldBuffer.size();

				// Check for field duplication (per-field, not per-frame)
				bool isDuplicate = m_hasPreviousField[field] &&
				                   fieldSize == m_previousField[field].size() &&
				                   std::memcmp(fieldData, m_previousField[field].data(), fieldSize) == 0;

				if (isDuplicate)
				{
					// Field is identical to previous same-field - send only header
					sent = m_groovy->CmdBlitDuplicate(m_frameNumber, 0);
				}
				else
				{
					// Field is different - send field data with explicit field number
					sent = m_groovy->CmdBlitField(fieldData,
					                              fieldSize,
					                              m_frameNumber,
					                              static_cast<uint8_t>(field),
					                              0); // vSync=0 for automatic

					// Update previous field buffer for next comparison
					if (m_previousField[field].size() != fieldSize)
						m_previousField[field].resize(fieldSize);
					std::memcpy(m_previousField[field].data(), fieldData, fieldSize);
					m_hasPreviousField[field] = true;
				}

				m_currentField = field;
			}
			else
			{
				// Legacy mode: interlace=2 (send full frames, FPGA splits)
				const uint8_t* outputBuffer = frameBuffer;
				size_t outputSize = m_frameBuffer.size();

				// Check for frame duplication (huge bandwidth savings on static screens)
				bool isDuplicate = m_hasPreviousFrame &&
				                   outputSize == m_previousFrame.size() &&
				                   std::memcmp(outputBuffer, m_previousFrame.data(), outputSize) == 0;

				if (isDuplicate)
				{
					// Frame is identical to previous - send only 9-byte header
					sent = m_groovy->CmdBlitDuplicate(m_frameNumber, 0);
				}
				else
				{
					// Frame is different - send full frame data
					sent = m_groovy->CmdBlit(outputBuffer,
					                         outputSize,
					                         m_frameNumber,
					                         0); // vSync=0 for automatic

					// Update previous frame buffer for next comparison
					if (m_previousFrame.size() != outputSize)
						m_previousFrame.resize(outputSize);
					std::memcpy(m_previousFrame.data(), outputBuffer, outputSize);
					m_hasPreviousFrame = true;
				}
			}

			if (sent)
			{
				// Wait for ACK and update status (provides natural frame pacing)
				if (m_groovy->WaitSync(16)) // 16ms timeout (~60fps)
				{
					// Cache status for field calculation on next frame
					m_lastBlitStatus = m_groovy->GetLastStatus();

					// KEY FIX: Sync our frame counter from FPGA feedback to prevent drift
					// This matches GroovyMAME's approach (drawnogpu.cpp line 454):
					//   m_frame = m_blit_status.frame_req + 1;
					// Without this, our frame counter drifts from FPGA's counter,
					// causing the field XOR calculation to flip incorrectly and
					// resulting in vertical flickering.
					if (m_lastBlitStatus.frameEcho > 0)
					{
						m_frameNumber = m_lastBlitStatus.frameEcho + 1;
					}
					else
					{
						m_frameNumber++;
					}
				}
				else
				{
					// WaitSync timed out - increment locally but we may be drifting
					m_frameNumber++;
				}
			}
			else
			{
				m_frameNumber++;
			}
		}
	}

	// Also swap local display if enabled
	if (m_localDisplayEnabled)
	{
		LowLevelWindow_X11::SwapBuffers();
	}
}

void LowLevelWindow_X11_MiSTer::CaptureFramebuffer()
{
	if (m_captureWidth <= 0 || m_captureHeight <= 0)
		return;

	// Ensure buffer is correct size
	size_t expectedSize = static_cast<size_t>(m_captureWidth) *
	                      static_cast<size_t>(m_captureHeight) * 3;
	if (m_frameBuffer.size() != expectedSize)
	{
		m_frameBuffer.resize(expectedSize);
	}

	// Read pixels from OpenGL framebuffer
	// This reads from the back buffer before swap
	// Use GL_BGR because MiSTer expects BGR byte order
	glReadPixels(0, 0, m_captureWidth, m_captureHeight,
	             GL_BGR, GL_UNSIGNED_BYTE, m_frameBuffer.data());

	// OpenGL framebuffer is bottom-up, MiSTer expects top-down
	FlipFramebufferVertical();
}

void LowLevelWindow_X11_MiSTer::FlipFramebufferVertical()
{
	const int rowSize = m_captureWidth * 3;
	std::vector<uint8_t> tempRow(rowSize);

	const int halfHeight = m_captureHeight / 2;
	for (int y = 0; y < halfHeight; y++)
	{
		int topOffset = y * rowSize;
		int bottomOffset = (m_captureHeight - 1 - y) * rowSize;

		// Swap top and bottom rows
		std::memcpy(tempRow.data(), &m_frameBuffer[topOffset], rowSize);
		std::memcpy(&m_frameBuffer[topOffset], &m_frameBuffer[bottomOffset], rowSize);
		std::memcpy(&m_frameBuffer[bottomOffset], tempRow.data(), rowSize);
	}
}

void LowLevelWindow_X11_MiSTer::ApplyDeflickerFilter()
{
	// Apply vertical blur to reduce interlace flicker on thin horizontal lines.
	// This spreads 1-pixel features across multiple scanlines so they appear
	// on both interlace fields, reducing the visible 30Hz flicker.
	//
	// Mode 1 (Light): kernel [1,2,1]/4 = [0.25, 0.5, 0.25] - preserves sharpness
	// Mode 2 (Strong): kernel [1,1,1]/3 = average of 3 lines - maximum smoothing

	if (m_deflickerMode <= 0 || m_captureWidth <= 0 || m_captureHeight < 3)
		return;

	const int rowSize = m_captureWidth * 3;

	// We need a temporary buffer to avoid reading modified values
	std::vector<uint8_t> tempBuffer(m_frameBuffer.size());

	for (int y = 0; y < m_captureHeight; y++)
	{
		// Handle edge rows by clamping
		const int yPrev = (y > 0) ? y - 1 : 0;
		const int yNext = (y < m_captureHeight - 1) ? y + 1 : m_captureHeight - 1;

		const uint8_t* rowPrev = &m_frameBuffer[yPrev * rowSize];
		const uint8_t* rowCurr = &m_frameBuffer[y * rowSize];
		const uint8_t* rowNext = &m_frameBuffer[yNext * rowSize];
		uint8_t* rowOut = &tempBuffer[y * rowSize];

		if (m_deflickerMode == 1)
		{
			// Light: [1,2,1]/4 kernel
			for (int x = 0; x < rowSize; x++)
			{
				int val = static_cast<int>(rowPrev[x]) +
				          static_cast<int>(rowCurr[x]) * 2 +
				          static_cast<int>(rowNext[x]);
				rowOut[x] = static_cast<uint8_t>(val >> 2);  // Divide by 4
			}
		}
		else // m_deflickerMode == 2
		{
			// Strong: [1,1,1]/3 kernel (average)
			for (int x = 0; x < rowSize; x++)
			{
				int val = static_cast<int>(rowPrev[x]) +
				          static_cast<int>(rowCurr[x]) +
				          static_cast<int>(rowNext[x]);
				rowOut[x] = static_cast<uint8_t>(val / 3);
			}
		}
	}

	// Copy result back to frame buffer
	std::memcpy(m_frameBuffer.data(), tempBuffer.data(), m_frameBuffer.size());
}

void LowLevelWindow_X11_MiSTer::ApplyOverscanScaling()
{
	// Scale down the framebuffer content to compensate for CRT overscan.
	// The content is shrunk and centered with black borders.
	// CRT overscan will cut off the black borders, leaving all content visible.

	if (m_overscanPercent <= 0 || m_captureWidth <= 0 || m_captureHeight <= 0)
		return;

	// Calculate scaled dimensions (e.g., 8% overscan = 92% scale = 589x442 for 640x480)
	const float scale = (100.0f - static_cast<float>(m_overscanPercent)) / 100.0f;
	const int scaledW = static_cast<int>(m_captureWidth * scale);
	const int scaledH = static_cast<int>(m_captureHeight * scale);

	// Calculate margins for centering
	const int marginX = (m_captureWidth - scaledW) / 2;
	const int marginY = (m_captureHeight - scaledH) / 2;

	// Clear scaled buffer to black (this creates the borders)
	std::memset(m_scaledBuffer.data(), 0, m_scaledBuffer.size());

	// Scale and center the captured frame into scaled buffer using nearest-neighbor
	const int srcRowSize = m_captureWidth * 3;
	const int dstRowSize = m_captureWidth * 3;

	for (int dy = 0; dy < scaledH; dy++)
	{
		// Map destination Y to source Y
		const int sy = dy * m_captureHeight / scaledH;
		const int dstY = marginY + dy;

		for (int dx = 0; dx < scaledW; dx++)
		{
			// Map destination X to source X
			const int sx = dx * m_captureWidth / scaledW;
			const int dstX = marginX + dx;

			// Copy 3 bytes (BGR) from source to destination
			const int srcOffset = sy * srcRowSize + sx * 3;
			const int dstOffset = dstY * dstRowSize + dstX * 3;

			m_scaledBuffer[dstOffset]     = m_frameBuffer[srcOffset];
			m_scaledBuffer[dstOffset + 1] = m_frameBuffer[srcOffset + 1];
			m_scaledBuffer[dstOffset + 2] = m_frameBuffer[srcOffset + 2];
		}
	}
}

void LowLevelWindow_X11_MiSTer::ExtractField(const uint8_t* frame, int field)
{
	// Extract one field (half the lines) from a full frame for interlace=1 mode.
	// field=0 (even): extract lines 0, 2, 4, 6... (top field)
	// field=1 (odd):  extract lines 1, 3, 5, 7... (bottom field)
	//
	// This implements the same algorithm as GroovyMAME (drawnogpu.cpp lines 403-418):
	//   int lstart = pitch * m_field;         // Start at line 0 or 1
	//   int lstep = pitch * interlace_factor; // Step by 2 lines

	if (m_captureWidth <= 0 || m_captureHeight < 2)
		return;

	const int srcRowSize = m_captureWidth * 3;
	const int fieldHeight = m_captureHeight / 2;

	// Ensure field buffer is correct size
	size_t expectedSize = static_cast<size_t>(srcRowSize) * static_cast<size_t>(fieldHeight);
	if (m_fieldBuffer.size() != expectedSize)
		m_fieldBuffer.resize(expectedSize);

	// Extract every other line starting from 'field'
	// field=0: lines 0,2,4,6... -> dstY 0,1,2,3...
	// field=1: lines 1,3,5,7... -> dstY 0,1,2,3...
	int srcY = field;  // Start at line 0 (even) or 1 (odd)
	for (int dstY = 0; dstY < fieldHeight; dstY++)
	{
		std::memcpy(&m_fieldBuffer[dstY * srcRowSize],
		            &frame[srcY * srcRowSize],
		            srcRowSize);
		srcY += 2;  // Skip to next line of same field
	}
}

int LowLevelWindow_X11_MiSTer::CalculateNextField()
{
	// Determine which field to send based on FPGA status.
	// This implements the GroovyMAME algorithm (drawnogpu.cpp line 400):
	//   m_field = (vgaF1 ? 1 : 0) ^ ((m_frame - fpga.frame) % 2);
	//
	// The logic ensures we send the field that will be displayed next,
	// accounting for frame latency between host and FPGA.

	// If we have no valid status yet, just alternate based on frame number
	if (m_lastBlitStatus.fpgaFrame == 0 && m_lastBlitStatus.frameEcho == 0)
	{
		return m_frameNumber % 2;
	}

	int vgaField = m_lastBlitStatus.vgaField ? 1 : 0;

	// Calculate frame difference, handling potential wraparound
	int frameDiff = static_cast<int>(m_frameNumber) - static_cast<int>(m_lastBlitStatus.fpgaFrame);

	// Sanity check: if frame difference is unreasonably large (drift detected),
	// fall back to simple alternation. This shouldn't happen often if frame
	// counter sync is working, but provides a safety net.
	if (frameDiff < 0 || frameDiff > 10)
	{
		// Counter drift or wraparound - just alternate based on local counter
		return m_frameNumber % 2;
	}

	// XOR current field with frame difference parity
	// This compensates for any lag between when we send and when FPGA displays
	return vgaField ^ (frameDiff % 2);
}

GroovyModeline LowLevelWindow_X11_MiSTer::VideoModeToModeline(const VideoModeParams& p)
{
	// Return appropriate 15kHz CRT modeline based on resolution
	// All modelines use ~15.7kHz horizontal frequency for CRT TV compatibility
	//
	// Interlace modes:
	//   interlace=1: Host sends 240-line fields separately (Phase 3 optimization)
	//   interlace=2: Host sends 480-line frames, FPGA splits to fields (legacy)

	GroovyModeline modeline;

	if (p.width == 640 && p.height == 480)
	{
		// 640x480 interlaced at 15kHz (NTSC TV timing)
		modeline = GroovyModelines::CRT15_640x480i_60;

		// Phase 3: Use interlace=1 for true field separation (50% bandwidth reduction)
		if (m_interlacedFB)
		{
			modeline.interlace = 1;  // Host sends fields
			LOG->Info("LowLevelWindow_X11_MiSTer: Using 640x480i 15kHz modeline (interlace=1, field mode)");
		}
		else
		{
			LOG->Info("LowLevelWindow_X11_MiSTer: Using 640x480i 15kHz modeline (interlace=2, frame mode)");
		}
	}
	else if (p.width == 640 && p.height == 240)
	{
		// 640x240 progressive at 15kHz
		modeline = GroovyModelines::CRT15_640x240_60;
		LOG->Info("LowLevelWindow_X11_MiSTer: Using 640x240p 15kHz modeline");
	}
	else if (p.width == 320 && p.height == 240)
	{
		// 320x240 progressive at 15kHz (arcade standard)
		modeline = GroovyModelines::CRT15_320x240_60;
		LOG->Info("LowLevelWindow_X11_MiSTer: Using 320x240p 15kHz modeline");
	}
	else if (p.width == 720 && p.height == 480)
	{
		// 720x480 interlaced at 15kHz (NTSC DVD)
		modeline = GroovyModelines::CRT15_720x480i_60;

		// Phase 3: Use interlace=1 for true field separation
		if (m_interlacedFB)
		{
			modeline.interlace = 1;
			LOG->Info("LowLevelWindow_X11_MiSTer: Using 720x480i 15kHz modeline (interlace=1, field mode)");
		}
		else
		{
			LOG->Info("LowLevelWindow_X11_MiSTer: Using 720x480i 15kHz modeline (interlace=2, frame mode)");
		}
	}
	else if (p.width == 256 && p.height == 240)
	{
		// 256x240 progressive (NES/SNES)
		modeline = GroovyModelines::CRT15_256x240_60;
		LOG->Info("LowLevelWindow_X11_MiSTer: Using 256x240p 15kHz modeline");
	}
	else
	{
		// Default to 640x480i for 15kHz CRT
		modeline = GroovyModelines::CRT15_640x480i_60;

		if (m_interlacedFB)
		{
			modeline.interlace = 1;
			LOG->Warn("LowLevelWindow_X11_MiSTer: No modeline for %dx%d, using 640x480i 15kHz (interlace=1)",
			          p.width, p.height);
		}
		else
		{
			LOG->Warn("LowLevelWindow_X11_MiSTer: No modeline for %dx%d, using 640x480i 15kHz (interlace=2)",
			          p.width, p.height);
		}
	}

	return modeline;
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
