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
#include <thread>
#include <algorithm>

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
	, m_progressiveScan(true)  // Default to progressive, will be overridden by preferences
	, m_localDisplayEnabled(true)
	, m_misterInitialized(false)
	, m_compressionMode(GroovyLZ4Mode::LZ4)
	, m_overscanPercent(0)
	, m_deflickerMode(0)
	// Frame timing initialization
	, m_frameTimeIndex(0)
	, m_frameTimeCount(0)
	, m_frameTimeAvg(0.0)
	, m_frameTimeJitter(0.0)
	, m_period(16.6667)      // 60Hz default
	, m_linePeriod(0.0635)   // ~15.7kHz default
	, m_frameDelay(0.0)
	, m_fdMargin(1.5)        // 1.5ms default safety margin
	, m_vtotal(525)          // NTSC default
	, m_vsyncScanline(0)
{
	m_hasPreviousField[0] = false;
	m_hasPreviousField[1] = false;
	std::memset(&m_lastBlitStatus, 0, sizeof(m_lastBlitStatus));
	std::memset(m_frameTimeHistory, 0, sizeof(m_frameTimeHistory));
	m_timeEntry = std::chrono::steady_clock::now();
	m_timeExit = m_timeEntry;
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
	m_progressiveScan = PREFSMAN->m_bMiSTerProgressive.Get();

	// Progressive mode overrides interlaced framebuffer setting
	// When progressive, we always send full frames (no field extraction)
	if (m_progressiveScan)
	{
		if (m_interlacedFB)
		{
			LOG->Warn("LowLevelWindow_X11_MiSTer: Progressive mode overrides InterlacedFB=true");
		}
		m_interlacedFB = false;
	}

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

	LOG->Info("LowLevelWindow_X11_MiSTer: Overscan=%d%%, Deflicker=%d, Progressive=%s, InterlacedFB=%s",
	          m_overscanPercent, m_deflickerMode,
	          m_progressiveScan ? "true (240p 15kHz)" : "false (480i 15kHz)",
	          m_interlacedFB ? "true (fields)" : "false (frames)");

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
		// ===== FRAME TIMING: Record entry time =====
		auto now = std::chrono::steady_clock::now();
		double frameTimeMs = std::chrono::duration<double, std::milli>(now - m_timeExit).count();
		m_timeEntry = now;

		// Register frame time and update adaptive timing
		RegisterFrameTime(frameTimeMs);
		CalculateFrameDelay();

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

			// Progressive 240p mode: Scale 480→240 for true progressive scan
			// This eliminates ALL interlace artifacts (no field separation at all)
			if (m_progressiveScan && m_captureHeight >= 480)
			{
				// Scale 480 lines to 240 using line averaging
				ScaleVerticalHalf(frameBuffer);

				const uint8_t* progData = m_progBuffer.data();
				size_t progSize = m_progBuffer.size();

				// Check for frame duplication (huge bandwidth savings on static screens)
				bool isDuplicate = m_hasPreviousFrame &&
				                   progSize == m_previousFrame.size() &&
				                   std::memcmp(progData, m_previousFrame.data(), progSize) == 0;

				if (isDuplicate)
				{
					// Frame is identical to previous - send only header
					sent = m_groovy->CmdBlitDuplicate(m_frameNumber,
					                                  static_cast<uint16_t>(m_vsyncScanline));
				}
				else
				{
					// Frame is different - send scaled 240-line frame
					// Progressive mode uses CmdBlit (not CmdBlitField) since there are no fields
					sent = m_groovy->CmdBlit(progData,
					                         progSize,
					                         m_frameNumber,
					                         static_cast<uint16_t>(m_vsyncScanline));

					// Update previous frame buffer for next comparison
					if (m_previousFrame.size() != progSize)
						m_previousFrame.resize(progSize);
					std::memcpy(m_previousFrame.data(), progData, progSize);
					m_hasPreviousFrame = true;
				}
			}
			// Interlaced field mode (interlace=1)
			// Send 240-line fields instead of full 480-line frames = 50% bandwidth reduction
			else if (m_interlacedFB && m_captureHeight >= 480)
			{
				// TEMPORAL INTERLACING FIX:
				// Field type is determined by frame number, not FPGA real-time state.
				// This ensures each frame contributes a different field:
				//   Frame 0 → even lines (field 0)
				//   Frame 1 → odd lines (field 1)
				//   Frame 2 → even lines (field 0)
				//   ...
				// This gives true 60Hz temporal sampling instead of spatial-only interlacing
				// which caused motion choppiness (same field type from multiple consecutive frames).
				// Frame counter is FPGA-synced after WaitSync, so fields stay aligned with display.
				int field = m_frameNumber % 2;

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
					sent = m_groovy->CmdBlitDuplicate(m_frameNumber,
					                                  static_cast<uint16_t>(m_vsyncScanline));
				}
				else
				{
					// Field is different - send field data with explicit field number
					// Use vsync_scanline to tell FPGA when to display (reduces tearing)
					sent = m_groovy->CmdBlitField(fieldData,
					                              fieldSize,
					                              m_frameNumber,
					                              static_cast<uint8_t>(field),
					                              static_cast<uint16_t>(m_vsyncScanline));

					// Update previous field buffer for next comparison
					if (m_previousField[field].size() != fieldSize)
						m_previousField[field].resize(fieldSize);
					std::memcpy(m_previousField[field].data(), fieldData, fieldSize);
					m_hasPreviousField[field] = true;
				}
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
					sent = m_groovy->CmdBlitDuplicate(m_frameNumber,
					                                  static_cast<uint16_t>(m_vsyncScanline));
				}
				else
				{
					// Frame is different - send full frame data
					// Use vsync_scanline to tell FPGA when to display (reduces tearing)
					sent = m_groovy->CmdBlit(outputBuffer,
					                         outputSize,
					                         m_frameNumber,
					                         static_cast<uint16_t>(m_vsyncScanline));

					// Update previous frame buffer for next comparison
					if (m_previousFrame.size() != outputSize)
						m_previousFrame.resize(outputSize);
					std::memcpy(m_previousFrame.data(), outputBuffer, outputSize);
					m_hasPreviousFrame = true;
				}
			}

			if (sent)
			{
				// ===== FRAME TIMING: Predictive wait with adaptive timing =====
				// WaitForVSync() implements GroovyMAME-style frame delay:
				// - Quick poll for ACK
				// - Calculate precise wait time based on FPGA position
				// - Sleep/busy-wait to maximize input capture time
				// - Sync frame counter from FPGA feedback
				if (!WaitForVSync())
				{
					// WaitForVSync handles frame counter sync internally
					// If it failed completely, just increment locally
					m_frameNumber++;
				}
			}
			else
			{
				m_frameNumber++;
			}

			// ===== FRAME TIMING: Record exit time =====
			m_timeExit = std::chrono::steady_clock::now();
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

void LowLevelWindow_X11_MiSTer::ScaleVerticalHalf(const uint8_t* frame)
{
	// Scale 480 lines to 240 lines for progressive 240p mode.
	// Uses line averaging: each output line is the average of two input lines.
	// This preserves thin horizontal lines better than simple decimation.
	//
	// Output line Y = (input line Y*2 + input line Y*2+1) / 2
	//
	// This eliminates interlace combing artifacts because every output frame
	// contains ALL 240 lines derived from the SAME instant in time (no temporal
	// field separation like in interlaced mode).

	if (m_captureWidth <= 0 || m_captureHeight < 2)
		return;

	const int srcRowSize = m_captureWidth * 3;  // RGB888
	const int dstHeight = m_captureHeight / 2;

	// Ensure progressive buffer is correct size
	size_t expectedSize = static_cast<size_t>(srcRowSize) * static_cast<size_t>(dstHeight);
	if (m_progBuffer.size() != expectedSize)
		m_progBuffer.resize(expectedSize);

	// Average pairs of lines
	for (int dstY = 0; dstY < dstHeight; dstY++)
	{
		const uint8_t* srcLine0 = &frame[(dstY * 2) * srcRowSize];
		const uint8_t* srcLine1 = &frame[(dstY * 2 + 1) * srcRowSize];
		uint8_t* dstLine = &m_progBuffer[dstY * srcRowSize];

		// Average each pixel component (R, G, B)
		for (int x = 0; x < srcRowSize; x++)
		{
			dstLine[x] = static_cast<uint8_t>((static_cast<int>(srcLine0[x]) +
			                                   static_cast<int>(srcLine1[x])) / 2);
		}
	}
}

void LowLevelWindow_X11_MiSTer::RegisterFrameTime(double frameTimeMs)
{
	// Based on GroovyMAME drawnogpu.cpp:776-817 (nogpu_register_frametime)
	// Track frame-to-frame time for adaptive frame delay calculation.

	// Discard invalid values (negative or > period)
	if (frameTimeMs <= 0.0 || frameTimeMs > m_period)
	{
		return;
	}

	// Add to circular buffer
	m_frameTimeHistory[m_frameTimeIndex] = frameTimeMs;
	m_frameTimeIndex = (m_frameTimeIndex + 1) % FRAME_TIME_SAMPLES;
	if (m_frameTimeCount < FRAME_TIME_SAMPLES)
	{
		m_frameTimeCount++;
	}

	// Calculate rolling average
	double sum = 0.0;
	for (int i = 0; i < m_frameTimeCount; i++)
	{
		sum += m_frameTimeHistory[i];
	}
	m_frameTimeAvg = sum / m_frameTimeCount;

	// Calculate max jitter (max positive difference between consecutive samples)
	// This represents the worst-case timing variation we need to account for
	double maxDiff = 0.0;
	for (int i = 1; i < m_frameTimeCount; i++)
	{
		double diff = m_frameTimeHistory[i] - m_frameTimeHistory[i - 1];
		if (diff > 0.0 && diff > maxDiff)
		{
			maxDiff = diff;
		}
	}
	m_frameTimeJitter = maxDiff;
}

void LowLevelWindow_X11_MiSTer::CalculateFrameDelay()
{
	// Based on GroovyMAME drawnogpu.cpp:885-901
	// Calculate optimal frame delay factor based on timing stability.
	//
	// Higher frame delay = more time to capture late input = lower latency
	// But too high risks missing deadlines if timing jitters
	//
	// Formula: frame_delay = (period - max(margin, jitter)) / period
	// - If jitter is low, frame_delay approaches 1.0 (wait almost full period)
	// - If jitter is high, frame_delay decreases for safety margin

	double margin = std::max(m_fdMargin, m_frameTimeJitter);
	m_frameDelay = std::max((m_period - margin) / m_period, 0.0);

	// Calculate vsync scanline target
	// For interlaced mode, we want to target the vblank region
	// vtotal=525 for NTSC: Field 0 vblank ~240-262, Field 1 vblank ~502-525
	// Using a high value (near vtotal) works for both fields since the FPGA
	// handles the field timing internally
	if (m_interlacedFB)
	{
		// Target late in the frame (vblank region) - around 90-95% of vtotal
		// This gives maximum time for input while staying in safe territory
		m_vsyncScanline = std::min(static_cast<int>(m_vtotal * 0.95), m_vtotal - 1);
	}
	else
	{
		// Progressive mode: use frame_delay calculation
		m_vsyncScanline = std::min(static_cast<int>(m_vtotal * m_frameDelay + 1), m_vtotal);
	}
}

bool LowLevelWindow_X11_MiSTer::WaitForVSync()
{
	// Simplified frame pacing: wait for ACK, then ensure consistent frame timing.
	//
	// The previous predictive wait based on FPGA scanline position caused
	// massive timing variations (0ms to 16ms) depending on where the FPGA
	// happened to be when we checked. This led to erratic frame pacing and
	// visible judder ("arrow moves, stops, jiggles, moves").
	//
	// New approach: consistent frame pacing based on wall-clock time.
	// The vsync_scanline in the blit command tells the FPGA when to display,
	// so we don't need to precisely time our sends - just maintain steady pace.

	// Wait for ACK with standard timeout
	if (!m_groovy->WaitSync(16))
	{
		return false;
	}

	// Get FPGA status
	m_lastBlitStatus = m_groovy->GetLastStatus();

	// Sync frame counter from FPGA feedback to prevent drift
	// CRITICAL: Must always increment frame counter, even when frameEcho is 0!
	if (m_lastBlitStatus.frameEcho > 0)
	{
		m_frameNumber = m_lastBlitStatus.frameEcho + 1;
	}
	else
	{
		// First frame or FPGA hasn't responded yet - increment locally
		m_frameNumber++;
	}

	// CONSISTENT FRAME PACING:
	// Target completing this frame at exactly one period from when we started.
	// This ensures steady ~60Hz pacing regardless of FPGA position.
	//
	// targetTime = when we should finish this frame and start the next
	// We subtract a small margin (frameTimeAvg) to account for processing time,
	// ensuring the NEXT frame's blit arrives on time.
	double waitMs = m_period - m_frameTimeAvg;

	// Clamp to reasonable range
	if (waitMs < 1.0)
	{
		waitMs = 1.0;  // Minimum 1ms wait to prevent racing
	}
	else if (waitMs > m_period * 1.5)
	{
		waitMs = m_period;  // Don't wait more than 1.5 periods
	}

	auto targetTime = m_timeEntry + std::chrono::microseconds(
		static_cast<int64_t>(waitMs * 1000.0));

	// Wait for target time with hybrid sleep/busy-wait
	while (std::chrono::steady_clock::now() < targetTime)
	{
		auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
			targetTime - std::chrono::steady_clock::now()).count();

		if (remaining > 2)
		{
			// Sleep for most of the wait to reduce CPU usage
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		// else busy-wait for final precision
	}

	return true;
}

GroovyModeline LowLevelWindow_X11_MiSTer::VideoModeToModeline(const VideoModeParams& p)
{
	// Return appropriate 15kHz CRT modeline based on resolution and scan mode
	//
	// Progressive mode (240p at 15kHz):
	//   Scales 480-line content to 240 lines, uses CRT15_640x240_60
	//   interlace=0: True progressive scan, eliminates interlace combing
	//
	// Interlaced mode (480i at 15kHz):
	//   interlace=1: Host sends 240-line fields separately (50% bandwidth)
	//   interlace=2: Host sends 480-line frames, FPGA splits to fields

	GroovyModeline modeline;

	// Progressive 240p mode at 15kHz - eliminates interlace combing artifacts
	// Content is scaled 480→240 before sending, giving true progressive scan
	// This is how arcade games achieved smooth motion on 15kHz CRTs
	if (m_progressiveScan && (p.width == 640 && p.height == 480))
	{
		modeline = GroovyModelines::CRT15_640x240_60;
		// CRT15_640x240_60 has interlace=0 (true progressive at 15kHz)
		LOG->Info("LowLevelWindow_X11_MiSTer: Using 640x240p 15kHz modeline (progressive, scaled from 480)");
	}
	else if (p.width == 640 && p.height == 480)
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
			modeline.interlace = 2;  // Host sends full frames, FPGA extracts fields
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
			modeline.interlace = 2;  // Host sends full frames, FPGA extracts fields
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
			modeline.interlace = 2;  // Host sends full frames, FPGA extracts fields
			LOG->Warn("LowLevelWindow_X11_MiSTer: No modeline for %dx%d, using 640x480i 15kHz (interlace=2)",
			          p.width, p.height);
		}
	}

	// Calculate timing parameters for frame delay optimization
	// Based on GroovyMAME drawnogpu.cpp:659-660
	m_vtotal = modeline.vtotal;

	// Frame period in ms: 1000 / (pclock / (htotal * vtotal)) / interlace_factor
	// pclock is in MHz, so multiply by 1e6 to get Hz
	double pclockHz = modeline.pclock * 1000000.0;
	double frameRate = pclockHz / (static_cast<double>(modeline.htotal) * modeline.vtotal);
	int interlaceFactor = (modeline.interlace != 0) ? 2 : 1;
	m_period = 1000.0 / frameRate / interlaceFactor;

	// Line period in ms: 1000 / hfreq where hfreq = pclock / htotal
	double hfreq = pclockHz / modeline.htotal;
	m_linePeriod = 1000.0 / hfreq;

	LOG->Info("LowLevelWindow_X11_MiSTer: Timing - period=%.3fms, line=%.4fms, vtotal=%d",
	          m_period, m_linePeriod, m_vtotal);

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
