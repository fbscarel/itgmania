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
#include <chrono>

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
	std::vector<uint8_t> m_progBuffer;    // Buffer for 240p progressive (scaled from 480)
	std::vector<uint8_t> m_previousField[2]; // Previous field data for each field (dup detection)
	bool m_hasPreviousFrame;              // Valid previous frame exists
	bool m_hasPreviousField[2];           // Valid previous field exists for each field
	uint32_t m_frameNumber;
	int m_captureWidth;
	int m_captureHeight;

	// Field/interlace state (Phase 3 optimization)
	bool m_interlacedFB;        // True = interlace=1 mode (host sends fields separately)
	bool m_progressiveScan;     // True = 240p progressive (15kHz, scaled), False = 480i interlaced
	bool m_predictiveSync;      // True = FPGA position-based timing, False = wall-clock timing
	GroovyStatus m_lastBlitStatus; // Cached status from last successful blit

	// Configuration
	bool m_localDisplayEnabled;
	bool m_misterInitialized;
	GroovyLZ4Mode m_compressionMode;
	int m_overscanPercent;  // CRT overscan compensation (0-20%)
	int m_deflickerMode;    // Interlace deflicker filter (0=off, 1=light, 2=strong)

	// Frame timing tracking (GroovyMAME-style adaptive frame delay)
	static const int FRAME_TIME_SAMPLES = 16;
	std::chrono::steady_clock::time_point m_timeEntry;  // When we started processing this frame
	std::chrono::steady_clock::time_point m_timeExit;   // When we finished processing last frame
	double m_frameTimeHistory[FRAME_TIME_SAMPLES];      // Circular buffer of frame times (ms)
	int m_frameTimeIndex;                               // Current index in circular buffer
	int m_frameTimeCount;                               // Number of valid samples
	double m_frameTimeAvg;                              // Rolling average frame time (ms)
	double m_frameTimeJitter;                           // Max deviation between consecutive samples (ms)

	// Frame delay calculation
	double m_period;           // Frame period in ms (16.67 for 60Hz)
	double m_linePeriod;       // Line period in ms
	double m_frameDelay;       // 0.0-1.0 frame delay factor
	double m_fdMargin;         // Safety margin in ms (default 1.5)
	int m_vtotal;              // Total vertical lines in modeline
	int m_vsyncScanline;       // Calculated vsync target scanline

	// Timing statistics for latency analysis
	bool m_timingStatsEnabled;                            // Log timing stats periodically
	static const int TIMING_STATS_INTERVAL = 300;         // Log every N frames (~5 sec at 60Hz)
	int m_timingStatsFrameCount;                          // Frames since last stats log
	double m_waitMsSum;                                   // Sum of waitMs values
	double m_waitMsMin;                                   // Minimum waitMs observed
	double m_waitMsMax;                                   // Maximum waitMs observed
	double m_waitMsSumSq;                                 // Sum of squares for variance
	int m_linesToWaitSum;                                 // Sum of linesToWait values
	int m_linesToWaitMin;                                 // Minimum linesToWait observed
	int m_linesToWaitMax;                                 // Maximum linesToWait observed
	int m_predictiveFrames;                               // Frames using predictive timing
	int m_wallClockFrames;                                // Frames using wall-clock fallback
	int m_lateFrames;                                     // Frames where linesToWait was negative

	// Internal helpers
	void CaptureFramebuffer();
	void FlipFramebufferVertical();
	void ApplyDeflickerFilter();  // Reduce interlace flicker on thin horizontal lines
	void ApplyOverscanScaling();  // Scale down content to compensate for CRT overscan
	void ExtractField(const uint8_t* frame, int field);  // Extract 240 lines from 480-line frame
	void ScaleVerticalHalf(const uint8_t* frame);  // Scale 480→240 for progressive mode (line averaging)
	GroovyModeline VideoModeToModeline(const VideoModeParams& p);
	void InitializeMiSTerFromPreferences();

	// Frame timing helpers (GroovyMAME-style adaptive frame delay)
	void RegisterFrameTime(double frameTimeMs);  // Add sample, recalculate avg/jitter
	void CalculateFrameDelay();                  // Compute frame_delay and vsync_scanline
	bool WaitForVSync();                         // Predictive wait with adaptive timing

	// Timing statistics helpers
	void ResetTimingStats();                     // Reset all timing stats to initial values
	void CollectTimingStats(double waitMs, int linesToWait, bool usedPredictive);  // Record frame timing
	void LogTimingStats();                       // Log accumulated stats and reset

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
