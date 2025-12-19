/* GroovyMisterTypes.h - Types and constants for GroovyMiSTer protocol.
 *
 * This implements the UDP protocol for streaming video frames to a MiSTer FPGA
 * running the Groovy core, enabling authentic CRT display for rhythm gaming.
 *
 * Protocol reference: https://github.com/psakhis/Groovy_MiSTer
 */

#ifndef GROOVY_MISTER_TYPES_H
#define GROOVY_MISTER_TYPES_H

#include <cstdint>

// Protocol constants
constexpr uint16_t GROOVY_DEFAULT_PORT = 32100;
constexpr int GROOVY_MTU_SIZE = 1472;          // 1500 - 28 (IP+UDP headers)
constexpr int GROOVY_MAX_FRAME_SIZE = 1245312; // 720*576*3 (max resolution)
constexpr int GROOVY_BUFFER_SLICES = 846;

// Protocol commands
constexpr uint8_t CMD_CLOSE = 0x01;
constexpr uint8_t CMD_INIT = 0x02;
constexpr uint8_t CMD_SWITCHRES = 0x03;
constexpr uint8_t CMD_AUDIO = 0x04;
constexpr uint8_t CMD_GET_STATUS = 0x05;
constexpr uint8_t CMD_BLIT_VSYNC = 0x06;
constexpr uint8_t CMD_BLIT_FIELD_VSYNC = 0x07;

// Compression modes for CMD_INIT
enum class GroovyLZ4Mode : uint8_t
{
	RAW = 0,      // No compression
	LZ4 = 1,      // Standard LZ4 compression
	LZ4_HC = 2    // LZ4 High Compression
};

// RGB pixel formats for CMD_INIT
enum class GroovyRGBMode : uint8_t
{
	RGB888 = 0,   // 24-bit RGB (3 bytes per pixel)
	RGBA888 = 1,  // 32-bit RGBA (4 bytes per pixel)
	RGB565 = 2    // 16-bit RGB (2 bytes per pixel)
};

// Audio sample rates for CMD_INIT
enum class GroovyAudioRate : uint8_t
{
	OFF = 0,
	RATE_22050 = 1,
	RATE_44100 = 2,
	RATE_48000 = 3
};

// Audio channel configuration for CMD_INIT
enum class GroovyAudioChannels : uint8_t
{
	OFF = 0,
	MONO = 1,
	STEREO = 2
};

// Status response from FPGA (13 bytes)
struct GroovyStatus
{
	uint32_t frameEcho;     // Frame number echoed back
	uint16_t vCountEcho;    // Scanline at frame reception
	uint32_t fpgaFrame;     // Current FPGA frame counter
	uint16_t fpgaVCount;    // Current FPGA scanline
	bool vramReady;         // Bit 0: VRAM ready for next frame
	bool vramEndFrame;      // Bit 1: End of frame reached
	bool vramSynced;        // Bit 2: Frame sync achieved
	bool vgaFrameskip;      // Bit 3: Framebuffer was used (frameskip occurred)
	bool vgaVblank;         // Bit 4: In vertical blanking
	bool vgaField;          // Bit 5: Current field (0=even, 1=odd) - CRITICAL for interlace=1
	bool audioEnabled;      // Bit 6: Audio streaming active
	bool vramQueue;         // Bit 7: Pixels queued in VRAM
};

// Modeline definition for CMD_SWITCHRES (25 bytes of data after command)
struct GroovyModeline
{
	double pclock;       // Pixel clock in MHz
	uint16_t hactive;    // Horizontal active pixels
	uint16_t hbegin;     // Horizontal sync start
	uint16_t hend;       // Horizontal sync end
	uint16_t htotal;     // Total horizontal pixels
	uint16_t vactive;    // Vertical active lines
	uint16_t vbegin;     // Vertical sync start
	uint16_t vend;       // Vertical sync end
	uint16_t vtotal;     // Total vertical lines
	uint8_t interlace;   // 0=progressive, 1=interlaced
};

// Predefined modelines for 15kHz CRT displays
// Reference: http://wiki.arcadecontrols.com/wiki/Modeline
// Note: interlace=2 means "progressive framebuffer, interlaced output" (MiSTer handles conversion)
namespace GroovyModelines
{
	// 640x480 interlaced @60Hz for 15kHz CRT (NTSC TV timing)
	// Modeline "NTSC 640x480 (60Hz)" 12.336 640 662 720 784 480 488 494 525 interlace
	// interlace=2: we send full 480-line frames, MiSTer converts to interlaced
	constexpr GroovyModeline CRT15_640x480i_60 = {
		12.336, 640, 662, 720, 784, 480, 488, 494, 525, 2  // interlace=2 (progressive FB)
	};

	// 320x240 progressive @60Hz for 15kHz CRT (NTSC arcade)
	// Modeline "320x240 NTSC (60Hz)" 6.700 320 336 367 426 240 244 247 262
	constexpr GroovyModeline CRT15_320x240_60 = {
		6.700, 320, 336, 367, 426, 240, 244, 247, 262, 0  // progressive
	};

	// 640x240 progressive @60Hz (double-width for 15kHz)
	// Modeline "640x240_60,0Hz 15,7KHz" 12.324 640 648 706 784 240 241 244 262
	constexpr GroovyModeline CRT15_640x240_60 = {
		12.324, 640, 648, 706, 784, 240, 241, 244, 262, 0  // progressive
	};

	// 256x240 progressive @60Hz (NES/SNES resolution)
	// Modeline "256x240 NTSC (60Hz)" 5.370 256 269 294 341 240 244 247 262
	constexpr GroovyModeline CRT15_256x240_60 = {
		5.370, 256, 269, 294, 341, 240, 244, 247, 262, 0  // progressive
	};

	// 720x480 interlaced @60Hz (NTSC DVD resolution)
	// Modeline "NTSC 720x480 (60Hz)" 13.846 720 744 809 880 480 488 494 525 interlace
	constexpr GroovyModeline CRT15_720x480i_60 = {
		13.846, 720, 744, 809, 880, 480, 488, 494, 525, 2  // interlace=2 (progressive FB)
	};
}

#endif // GROOVY_MISTER_TYPES_H

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
