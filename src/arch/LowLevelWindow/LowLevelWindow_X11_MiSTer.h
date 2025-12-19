/* LowLevelWindow_X11_MiSTer - X11 window driver with GroovyMiSTer CRT output.
 *
 * This class extends LowLevelWindow_X11 to add video streaming to a MiSTer FPGA
 * running the Groovy core, enabling authentic CRT display for rhythm gaming.
 *
 * Features:
 * - Dual output: local X11 display + MiSTer CRT
 * - LZ4 compression for efficient network transfer
 * - Automatic frame capture from OpenGL framebuffer
 * - Configurable modelines for different CRT resolutions
 */

#ifndef LOW_LEVEL_WINDOW_X11_MISTER_H
#define LOW_LEVEL_WINDOW_X11_MISTER_H

#include "LowLevelWindow_X11.h"
#include "GroovyMister/GroovyMister.h"
#include <memory>
#include <vector>

class LowLevelWindow_X11_MiSTer : public LowLevelWindow_X11
{
public:
	LowLevelWindow_X11_MiSTer();
	~LowLevelWindow_X11_MiSTer() override;

	// LowLevelWindow overrides
	RString TryVideoMode(const VideoModeParams& p, bool& bNewDeviceOut) override;
	void SwapBuffers() override;

	// MiSTer-specific connection management
	bool ConnectToMiSTer(const std::string& ip, uint16_t port = GROOVY_DEFAULT_PORT);
	void DisconnectFromMiSTer();
	bool IsMiSTerConnected() const;

	// Configuration
	void SetLocalDisplayEnabled(bool enabled) { m_localDisplayEnabled = enabled; }
	bool IsLocalDisplayEnabled() const { return m_localDisplayEnabled; }

	void SetCompressionMode(GroovyLZ4Mode mode);
	GroovyLZ4Mode GetCompressionMode() const;

	// Statistics
	uint32_t GetFramesSent() const { return m_frameNumber; }
	uint32_t GetDroppedFrames() const;

private:
	// MiSTer protocol handler
	std::unique_ptr<GroovyMister> m_groovy;

	// Frame capture state
	std::vector<uint8_t> m_frameBuffer;
	std::vector<uint8_t> m_scaledBuffer;  // Buffer for overscan-adjusted output
	std::vector<uint8_t> m_previousFrame; // For frame duplication detection
	std::vector<uint8_t> m_fieldBuffer;   // Buffer for single field (240 lines for 480i)
	std::vector<uint8_t> m_previousField[2]; // Previous field data for each field (dup detection)
	bool m_hasPreviousFrame;              // Valid previous frame exists
	bool m_hasPreviousField[2];           // Valid previous field exists for each field
	uint32_t m_frameNumber;
	int m_captureWidth;
	int m_captureHeight;

	// Field/interlace state (Phase 3 optimization)
	bool m_interlacedFB;        // True = interlace=1 mode (host sends fields separately)
	int m_currentField;         // Current field being processed (0=even, 1=odd)
	GroovyStatus m_lastBlitStatus; // Cached status from last successful blit

	// Configuration
	bool m_localDisplayEnabled;
	bool m_misterInitialized;
	GroovyLZ4Mode m_compressionMode;
	int m_overscanPercent;  // CRT overscan compensation (0-20%)
	int m_deflickerMode;    // Interlace deflicker filter (0=off, 1=light, 2=strong)

	// Internal helpers
	void CaptureFramebuffer();
	void FlipFramebufferVertical();
	void ApplyDeflickerFilter();  // Reduce interlace flicker on thin horizontal lines
	void ApplyOverscanScaling();  // Scale down content to compensate for CRT overscan
	void ExtractField(const uint8_t* frame, int field);  // Extract 240 lines from 480-line frame
	int CalculateNextField();     // Determine which field to send based on FPGA status
	GroovyModeline VideoModeToModeline(const VideoModeParams& p);
	void InitializeMiSTerFromPreferences();

	// Disable copying
	LowLevelWindow_X11_MiSTer(const LowLevelWindow_X11_MiSTer&) = delete;
	LowLevelWindow_X11_MiSTer& operator=(const LowLevelWindow_X11_MiSTer&) = delete;
};

#endif // LOW_LEVEL_WINDOW_X11_MISTER_H

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
