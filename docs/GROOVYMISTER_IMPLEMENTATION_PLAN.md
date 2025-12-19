# GroovyMiSTer Video Driver Implementation Plan for ITGMania

## Executive Summary

This document provides a complete implementation plan for adding GroovyMiSTer CRT output support to ITGMania. The goal is to stream video frames over UDP to a MiSTer FPGA running the Groovy core, enabling authentic CRT display for rhythm gaming.

**Scope:**
- ✅ Video streaming to MiSTer (primary focus)
- ⏳ Audio streaming (optional expansion, architecture prepared)
- ❌ Input from MiSTer (out of scope)

**Target:** ITGMania release branch at `~/src/itgmania`
**MiSTer IP (test):** 192.168.30.81

---

## 1. Project Architecture Overview

### 1.1 Source Codebases (Reference)

| Codebase | Location | Purpose |
|----------|----------|---------|
| Groovy_MiSTer | `~/src/Groovy_MiSTer` | FPGA core + protocol library |
| GroovyMAME | `~/src/GroovyMAME` | Reference video driver implementation |
| MiSTerCast | `~/src/MiSTerCast` | Clean C++ protocol implementation |
| ITGMania | `~/src/itgmania` | Target application |

### 1.2 Key Reference Files

**Protocol Library (to copy/adapt):**
```
~/src/Groovy_MiSTer/api/groovymister.h      # Class interface
~/src/Groovy_MiSTer/api/groovymister.cpp    # Full protocol (needs Linux adaptation)
```

**Reference Implementations:**
```
~/src/GroovyMAME/src/osd/modules/render/drawnogpu.cpp    # Frame timing, compression
~/src/MiSTerCast/Library/MiSTerCastLib/groovymister.cpp  # Clean C++ reference
~/src/MiSTerCast/Library/MiSTerCastLib/renderer_nogpu.h  # Renderer interface
```

**ITGMania Integration Points:**
```
~/src/itgmania/src/arch/LowLevelWindow/LowLevelWindow.h           # Window abstraction base
~/src/itgmania/src/arch/LowLevelWindow/LowLevelWindow_X11.h       # X11 implementation
~/src/itgmania/src/arch/LowLevelWindow/LowLevelWindow_X11.cpp     # SwapBuffers lives here
~/src/itgmania/src/RageDisplay.h                                   # Display abstraction
~/src/itgmania/src/RageDisplay_OGL.cpp                            # OpenGL renderer
~/src/itgmania/src/GameLoop.cpp                                    # Main loop (BeginFrame/EndFrame)
~/src/itgmania/src/PrefsManager.h                                  # Preferences system
~/src/itgmania/src/arch/arch_default.h                            # Default driver selection
~/src/itgmania/src/CMakeData-arch.cmake                           # Arch-specific sources
~/src/itgmania/CMake/DefineOptions.cmake                          # Build options
```

---

## 2. GroovyMiSTer Protocol Specification

### 2.1 Transport Layer

- **Protocol:** UDP/IP
- **Port:** 32100 (video/commands), configurable
- **MTU:** 1472 bytes payload (1500 - 28 header overhead)
- **Connection:** Connectionless, but requires CMD_INIT handshake

### 2.2 Command Set

| Command | Code | Size | Description |
|---------|------|------|-------------|
| CMD_CLOSE | 0x01 | 1 | Terminate connection |
| CMD_INIT | 0x02 | 5 | Initialize streaming session |
| CMD_SWITCHRES | 0x03 | 26 | Set video mode/modeline |
| CMD_AUDIO | 0x04 | 3+data | Stream audio samples |
| CMD_BLIT_VSYNC | 0x06 | 8+data | Send raw frame |
| CMD_BLIT_FIELD_VSYNC | 0x07 | 12+data | Send compressed frame |

### 2.3 CMD_INIT Packet (5 bytes)

```
Offset  Size  Field
0       1     Command (0x02)
1       1     LZ4 mode: 0=RAW, 1=LZ4, 2=LZ4_HC
2       1     Sound rate: 0=OFF, 1=22050Hz, 2=44100Hz, 3=48000Hz
3       1     Sound channels: 0=OFF, 1=MONO, 2=STEREO
4       1     RGB mode: 0=RGB888, 1=RGBA888, 2=RGB565
```

### 2.4 CMD_SWITCHRES Packet (26 bytes)

```
Offset  Size  Field
0       1     Command (0x03)
1       8     Pixel clock (double, MHz)
9       2     hActive (horizontal active pixels)
11      2     hBegin (hsync start)
13      2     hEnd (hsync end)
15      2     hTotal (total horizontal)
17      2     vActive (vertical active lines)
19      2     vBegin (vsync start)
21      2     vEnd (vsync end)
23      2     vTotal (total vertical)
25      1     Interlace: 0=progressive, 1=interlaced
```

### 2.5 CMD_BLIT_VSYNC Packet (raw frame)

```
Offset  Size  Field
0       1     Command (0x06)
1       4     Frame number (uint32_t)
5       1     Field: 0=progressive, 1=field1, 2=field2
6       2     vCountSync (scanline to sync with)
[8...]        Raw RGB pixel data (split across MTU chunks)
```

### 2.6 CMD_BLIT_FIELD_VSYNC Packet (compressed frame)

```
Offset  Size  Field
0       1     Command (0x07)
1       4     Frame number (uint32_t)
5       1     Field: 0=progressive
6       2     vCountSync
8       4     Compressed size (uint32_t)
[12...]       LZ4 compressed RGB data
```

### 2.7 ACK Response (13 bytes from MiSTer)

```
Offset  Size  Field
0       4     Frame echo (last received frame)
4       2     vCount echo (scanline at reception)
6       4     Current FPGA frame
10      2     Current FPGA vCount
12      1     Status bits:
              Bit 0: vramReady
              Bit 1: vramEndFrame
              Bit 2: vramSynced
              Bit 4: vgaVblank
              Bit 6: audio enabled
```

### 2.8 Common Modelines

```cpp
// 640x480@60 Progressive (standard VGA)
// pclock=25.175 hact=640 hbeg=656 hend=752 htot=800 vact=480 vbeg=490 vend=492 vtot=525
{25.175, 640, 656, 752, 800, 480, 490, 492, 525, 0}

// 320x240@60 Progressive (low-res arcade)
// pclock=6.7 hact=320 hbeg=336 hend=367 htot=426 vact=240 vbeg=244 vend=247 vtot=262
{6.7, 320, 336, 367, 426, 240, 244, 247, 262, 0}

// 640x480@60 for 15kHz arcade monitor (requires scandoubler bypass)
// This uses arcade_15 timing - check switchres for exact values
```

---

## 3. ITGMania Architecture

### 3.1 Rendering Pipeline

```
GameLoop::RunGameLoop()  [src/GameLoop.cpp]
    │
    ├── DISPLAY->BeginFrame()
    │       └── RageDisplay_Legacy::BeginFrame() [src/RageDisplay_OGL.cpp]
    │
    ├── SCREENMAN->Draw()
    │       └── Renders all actors to OpenGL framebuffer
    │
    └── DISPLAY->EndFrame()
            └── RageDisplay_Legacy::EndFrame()
                    └── m_pWind->SwapBuffers()  ← INTEGRATION POINT
                            └── LowLevelWindow_X11::SwapBuffers() [src/arch/LowLevelWindow/LowLevelWindow_X11.cpp]
                                    └── glXSwapBuffers()
```

### 3.2 LowLevelWindow Abstraction

```cpp
// src/arch/LowLevelWindow/LowLevelWindow.h
class LowLevelWindow {
public:
    virtual bool TryVideoMode(const VideoModeParams& p, bool&


newDeviceOut) = 0;
    virtual void SwapBuffers() = 0;
    virtual void GetDisplaySpecs(DisplaySpecs& out) const = 0;
    virtual ActualVideoModeParams GetActualVideoModeParams() const = 0;
    // ...
};
```

### 3.3 VideoModeParams Structure

```cpp
// src/RageDisplay.h (simplified)
struct VideoModeParams {
    int width, height;
    int bpp;           // bits per pixel (16, 32)
    int rate;          // refresh rate
    bool vsync;
    bool windowed;
    bool interlaced;
    std::string sDisplayId;
};
```

### 3.4 Preferences System

```cpp
// src/PrefsManager.h - Add new preferences here
// Example existing pattern:
Preference<bool> m_bVsync;
Preference<int> m_iRefreshRate;
Preference<std::string> m_sVideoRenderers;
```

---

## 4. Implementation Strategy

### 4.1 Approach: LowLevelWindow Wrapper

Create `LowLevelWindow_X11_MiSTer` that:
1. Inherits from `LowLevelWindow_X11`
2. Overrides `SwapBuffers()` to also send frames to MiSTer
3. Overrides `TryVideoMode()` to configure MiSTer modeline
4. Optionally preserves local display output

**Benefits:**
- Minimal changes to existing code
- Dual output (local + MiSTer) for debugging
- Clean separation of concerns
- Can be toggled via preferences

### 4.2 File Structure (New Files)

```
src/arch/LowLevelWindow/
├── LowLevelWindow_X11_MiSTer.h          # NEW: Wrapper class
├── LowLevelWindow_X11_MiSTer.cpp        # NEW: Implementation
└── GroovyMister/                         # NEW: Protocol library
    ├── GroovyMister.h                    # Adapted from Groovy_MiSTer/api
    ├── GroovyMister.cpp                  # Adapted for Linux
    └── GroovyMisterTypes.h               # Shared types/constants
```

---

## 5. Detailed Implementation

### 5.1 GroovyMister Library Adaptation

**Source:** `~/src/Groovy_MiSTer/api/groovymister.cpp`

**Changes needed for Linux:**
1. Remove Windows RIO (Registered I/O) code
2. Replace Winsock with POSIX sockets
3. Remove Windows threading, use std::thread
4. Keep core protocol logic intact

**Create: `src/arch/LowLevelWindow/GroovyMister/GroovyMisterTypes.h`**

```cpp
#ifndef GROOVY_MISTER_TYPES_H
#define GROOVY_MISTER_TYPES_H

#include <cstdint>

// Protocol constants
constexpr uint16_t GROOVY_DEFAULT_PORT = 32100;
constexpr int GROOVY_MTU_SIZE = 1472;
constexpr int GROOVY_MAX_FRAME_SIZE = 1245312;  // 720*576*3
constexpr int GROOVY_BUFFER_SLICES = 846;

// Commands
constexpr uint8_t CMD_CLOSE = 0x01;
constexpr uint8_t CMD_INIT = 0x02;
constexpr uint8_t CMD_SWITCHRES = 0x03;
constexpr uint8_t CMD_AUDIO = 0x04;
constexpr uint8_t CMD_GET_STATUS = 0x05;
constexpr uint8_t CMD_BLIT_VSYNC = 0x06;
constexpr uint8_t CMD_BLIT_FIELD_VSYNC = 0x07;

// Compression modes
enum class GroovyLZ4Mode : uint8_t {
    RAW = 0,
    LZ4 = 1,
    LZ4_HC = 2
};

// RGB modes
enum class GroovyRGBMode : uint8_t {
    RGB888 = 0,
    RGBA888 = 1,
    RGB565 = 2
};

// Audio rates
enum class GroovyAudioRate : uint8_t {
    OFF = 0,
    RATE_22050 = 1,
    RATE_44100 = 2,
    RATE_48000 = 3
};

// Status response from FPGA
struct GroovyStatus {
    uint32_t frameEcho;
    uint16_t vCountEcho;
    uint32_t fpgaFrame;
    uint16_t fpgaVCount;
    bool vramReady;
    bool vramEndFrame;
    bool vramSynced;
    bool vgaVblank;
    bool audioEnabled;
};

// Modeline definition
struct GroovyModeline {
    double pclock;      // Pixel clock in MHz
    uint16_t hactive;
    uint16_t hbegin;
    uint16_t hend;
    uint16_t htotal;
    uint16_t vactive;
    uint16_t vbegin;
    uint16_t vend;
    uint16_t vtotal;
    uint8_t interlace;  // 0=progressive, 1=interlaced
};

// Predefined modelines
namespace GroovyModelines {
    // 640x480@60 VGA
    constexpr GroovyModeline VGA_640x480_60 = {
        25.175, 640, 656, 752, 800, 480, 490, 492, 525, 0
    };

    // 320x240@60 (doubled to 640x480 on output)
    constexpr GroovyModeline ARCADE_320x240_60 = {
        6.7, 320, 336, 367, 426, 240, 244, 247, 262, 0
    };
}

#endif // GROOVY_MISTER_TYPES_H
```

**Create: `src/arch/LowLevelWindow/GroovyMister/GroovyMister.h`**

```cpp
#ifndef GROOVY_MISTER_H
#define GROOVY_MISTER_H

#include "GroovyMisterTypes.h"
#include <string>
#include <vector>
#include <memory>
#include <atomic>

class GroovyMister {
public:
    GroovyMister();
    ~GroovyMister();

    // Connection
    bool Connect(const std::string& ip, uint16_t port = GROOVY_DEFAULT_PORT);
    void Disconnect();
    bool IsConnected() const;

    // Initialization
    bool CmdInit(GroovyLZ4Mode lz4, GroovyAudioRate audioRate,
                 uint8_t audioChannels, GroovyRGBMode rgbMode);

    // Video mode
    bool CmdSwitchres(const GroovyModeline& modeline);

    // Frame blitting
    bool CmdBlit(const uint8_t* frameData, size_t frameSize,
                 uint32_t frameNum, uint16_t vSync = 0);

    // Audio (expandable - stub for now)
    bool CmdAudio(const int16_t* samples, size_t sampleCount);

    // Synchronization
    bool WaitSync(uint32_t timeoutMs = 16);
    GroovyStatus GetLastStatus() const;

    // Statistics
    uint32_t GetCurrentFrame() const { return m_frameCount; }
    double GetAverageLatencyMs() const;

private:
    // Socket
    int m_socket;
    std::string m_ip;
    uint16_t m_port;

    // State
    std::atomic<bool> m_connected;
    uint32_t m_frameCount;
    GroovyStatus m_lastStatus;
    GroovyModeline m_currentMode;
    GroovyLZ4Mode m_lz4Mode;
    GroovyRGBMode m_rgbMode;

    // Buffers
    std::vector<uint8_t> m_sendBuffer;
    std::vector<uint8_t> m_compressBuffer;
    std::vector<uint8_t> m_recvBuffer;

    // Internal helpers
    bool SendPacket(const uint8_t* data, size_t size);
    bool ReceiveAck();
    size_t CompressFrame(const uint8_t* src, size_t srcSize, uint8_t* dst);
    void BuildBlitPacket(uint8_t* buffer, uint32_t frameNum,
                         uint16_t vSync, uint32_t compSize);
};

#endif // GROOVY_MISTER_H
```

**Create: `src/arch/LowLevelWindow/GroovyMister/GroovyMister.cpp`**

```cpp
#include "GroovyMister.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <cstring>
#include <lz4.h>

GroovyMister::GroovyMister()
    : m_socket(-1)
    , m_port(GROOVY_DEFAULT_PORT)
    , m_connected(false)
    , m_frameCount(0)
    , m_lz4Mode(GroovyLZ4Mode::LZ4)
    , m_rgbMode(GroovyRGBMode::RGB888)
{
    m_sendBuffer.resize(GROOVY_MTU_SIZE);
    m_compressBuffer.resize(LZ4_compressBound(GROOVY_MAX_FRAME_SIZE));
    m_recvBuffer.resize(64);
    memset(&m_lastStatus, 0, sizeof(m_lastStatus));
    memset(&m_currentMode, 0, sizeof(m_currentMode));
}

GroovyMister::~GroovyMister()
{
    Disconnect();
}

bool GroovyMister::Connect(const std::string& ip, uint16_t port)
{
    if (m_connected) Disconnect();

    m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_socket < 0) return false;

    // Set non-blocking for receives
    int flags = fcntl(m_socket, F_GETFL, 0);
    fcntl(m_socket, F_SETFL, flags | O_NONBLOCK);

    // Set send buffer size
    int bufSize = 2 * 1024 * 1024;  // 2MB
    setsockopt(m_socket, SOL_SOCKET, SO_SNDBUF, &bufSize, sizeof(bufSize));

    // Connect to MiSTer (allows using send() instead of sendto())
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

    if (connect(m_socket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(m_socket);
        m_socket = -1;
        return false;
    }

    m_ip = ip;
    m_port = port;
    m_connected = true;
    m_frameCount = 0;

    return true;
}

void GroovyMister::Disconnect()
{
    if (m_socket >= 0) {
        // Send close command
        uint8_t closeCmd = CMD_CLOSE;
        send(m_socket, &closeCmd, 1, 0);
        close(m_socket);
        m_socket = -1;
    }
    m_connected = false;
}

bool GroovyMister::IsConnected() const
{
    return m_connected;
}

bool GroovyMister::CmdInit(GroovyLZ4Mode lz4, GroovyAudioRate audioRate,
                           uint8_t audioChannels, GroovyRGBMode rgbMode)
{
    if (!m_connected) return false;

    uint8_t packet[5];
    packet[0] = CMD_INIT;
    packet[1] = static_cast<uint8_t>(lz4);
    packet[2] = static_cast<uint8_t>(audioRate);
    packet[3] = audioChannels;
    packet[4] = static_cast<uint8_t>(rgbMode);

    m_lz4Mode = lz4;
    m_rgbMode = rgbMode;

    return SendPacket(packet, sizeof(packet));
}

bool GroovyMister::CmdSwitchres(const GroovyModeline& modeline)
{
    if (!m_connected) return false;

    uint8_t packet[26];
    packet[0] = CMD_SWITCHRES;

    // Pack modeline (little-endian)
    memcpy(&packet[1], &modeline.pclock, 8);
    memcpy(&packet[9], &modeline.hactive, 2);
    memcpy(&packet[11], &modeline.hbegin, 2);
    memcpy(&packet[13], &modeline.hend, 2);
    memcpy(&packet[15], &modeline.htotal, 2);
    memcpy(&packet[17], &modeline.vactive, 2);
    memcpy(&packet[19], &modeline.vbegin, 2);
    memcpy(&packet[21], &modeline.vend, 2);
    memcpy(&packet[23], &modeline.vtotal, 2);
    packet[25] = modeline.interlace;

    m_currentMode = modeline;

    return SendPacket(packet, sizeof(packet));
}

bool GroovyMister::CmdBlit(const uint8_t* frameData, size_t frameSize,
                           uint32_t frameNum, uint16_t vSync)
{
    if (!m_connected) return false;

    const uint8_t* dataToSend = frameData;
    size_t sizeToSend = frameSize;
    uint8_t cmd = CMD_BLIT_VSYNC;
    size_t headerSize = 8;

    // Compress if enabled
    if (m_lz4Mode != GroovyLZ4Mode::RAW) {
        int compSize;
        if (m_lz4Mode == GroovyLZ4Mode::LZ4_HC) {
            compSize = LZ4_compress_HC((const char*)frameData,
                                       (char*)m_compressBuffer.data(),
                                       frameSize, m_compressBuffer.size(),
                                       LZ4HC_CLEVEL_DEFAULT);
        } else {
            compSize = LZ4_compress_default((const char*)frameData,
                                            (char*)m_compressBuffer.data(),
                                            frameSize, m_compressBuffer.size());
        }

        if (compSize > 0 && (size_t)compSize < frameSize) {
            dataToSend = m_compressBuffer.data();
            sizeToSend = compSize;
            cmd = CMD_BLIT_FIELD_VSYNC;
            headerSize = 12;
        }
    }

    // Build header
    uint8_t header[12];
    header[0] = cmd;
    memcpy(&header[1], &frameNum, 4);
    header[5] = 0;  // field (progressive)
    memcpy(&header[6], &vSync, 2);

    if (cmd == CMD_BLIT_FIELD_VSYNC) {
        uint32_t compSize32 = static_cast<uint32_t>(sizeToSend);
        memcpy(&header[8], &compSize32, 4);
    }

    // Send header
    if (!SendPacket(header, headerSize)) return false;

    // Send frame data in MTU-sized chunks
    size_t offset = 0;
    while (offset < sizeToSend) {
        size_t chunkSize = std::min((size_t)GROOVY_MTU_SIZE, sizeToSend - offset);
        if (send(m_socket, dataToSend + offset, chunkSize, 0) < 0) {
            return false;
        }
        offset += chunkSize;
    }

    m_frameCount = frameNum;
    return true;
}

bool GroovyMister::CmdAudio(const int16_t* samples, size_t sampleCount)
{
    // STUB: Audio expansion point
    // Implementation follows same pattern as CmdBlit
    // See ~/src/GroovyMAME/src/osd/modules/render/drawnogpu.cpp
    // for reference (search for CMD_AUDIO)
    (void)samples;
    (void)sampleCount;
    return true;
}

bool GroovyMister::WaitSync(uint32_t timeoutMs)
{
    if (!m_connected) return false;

    struct pollfd pfd;
    pfd.fd = m_socket;
    pfd.events = POLLIN;

    int ret = poll(&pfd, 1, timeoutMs);
    if (ret > 0 && (pfd.revents & POLLIN)) {
        return ReceiveAck();
    }

    return false;
}

bool GroovyMister::ReceiveAck()
{
    ssize_t received = recv(m_socket, m_recvBuffer.data(), m_recvBuffer.size(), 0);
    if (received >= 13) {
        const uint8_t* data = m_recvBuffer.data();
        memcpy(&m_lastStatus.frameEcho, data + 0, 4);
        memcpy(&m_lastStatus.vCountEcho, data + 4, 2);
        memcpy(&m_lastStatus.fpgaFrame, data + 6, 4);
        memcpy(&m_lastStatus.fpgaVCount, data + 10, 2);

        uint8_t flags = data[12];
        m_lastStatus.vramReady = flags & 0x01;
        m_lastStatus.vramEndFrame = flags & 0x02;
        m_lastStatus.vramSynced = flags & 0x04;
        m_lastStatus.vgaVblank = flags & 0x10;
        m_lastStatus.audioEnabled = flags & 0x40;

        return true;
    }
    return false;
}

GroovyStatus GroovyMister::GetLastStatus() const
{
    return m_lastStatus;
}

bool GroovyMister::SendPacket(const uint8_t* data, size_t size)
{
    return send(m_socket, data, size, 0) == (ssize_t)size;
}

size_t GroovyMister::CompressFrame(const uint8_t* src, size_t srcSize, uint8_t* dst)
{
    return LZ4_compress_default((const char*)src, (char*)dst,
                                srcSize, LZ4_compressBound(srcSize));
}

double GroovyMister::GetAverageLatencyMs() const
{
    // Could track timing statistics here
    return 0.0;
}
```

### 5.2 LowLevelWindow_X11_MiSTer Implementation

**Create: `src/arch/LowLevelWindow/LowLevelWindow_X11_MiSTer.h`**

```cpp
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
    std::string TryVideoMode(const VideoModeParams& p, bool& newDeviceOut) override;
    void SwapBuffers() override;

    // MiSTer-specific
    bool ConnectToMiSTer(const std::string& ip, uint16_t port = GROOVY_DEFAULT_PORT);
    void DisconnectFromMiSTer();
    bool IsMiSTerConnected() const;

    // Configuration
    void SetLocalDisplayEnabled(bool enabled) { m_localDisplayEnabled = enabled; }
    void SetCompression(GroovyLZ4Mode mode);

private:
    std::unique_ptr<GroovyMister> m_groovy;

    // Frame capture
    std::vector<uint8_t> m_frameBuffer;
    uint32_t m_frameNumber;
    int m_captureWidth;
    int m_captureHeight;

    // Configuration
    bool m_localDisplayEnabled;
    GroovyLZ4Mode m_compressionMode;

    // Helpers
    void CaptureFramebuffer();
    void FlipFramebufferVertical();
    GroovyModeline VideoModeToModeline(const VideoModeParams& p);
};

#endif // LOW_LEVEL_WINDOW_X11_MISTER_H
```

**Create: `src/arch/LowLevelWindow/LowLevelWindow_X11_MiSTer.cpp`**

```cpp
#include "global.h"
#include "LowLevelWindow_X11_MiSTer.h"
#include "PrefsManager.h"
#include "RageLog.h"
#include "RageDisplay.h"  // For DISPLAY->GetActualVideoModeParams()

#include <GL/gl.h>
#include <cstring>

// Register preferences (add to PrefsManager.h/.cpp)
// Preference<bool>        g_bMiSTerEnable("MiSTerEnable", false);
// Preference<std::string> g_sMiSTerIP("MiSTerIP", "192.168.30.81");
// Preference<int>         g_iMiSTerCompression("MiSTerCompression", 1);
// Preference<bool>        g_bMiSTerLocalDisplay("MiSTerLocalDisplay", true);

LowLevelWindow_X11_MiSTer::LowLevelWindow_X11_MiSTer()
    : m_groovy(std::make_unique<GroovyMister>())
    , m_frameNumber(0)
    , m_captureWidth(0)
    , m_captureHeight(0)
    , m_localDisplayEnabled(true)
    , m_compressionMode(GroovyLZ4Mode::LZ4)
{
}

LowLevelWindow_X11_MiSTer::~LowLevelWindow_X11_MiSTer()
{
    DisconnectFromMiSTer();
}

bool LowLevelWindow_X11_MiSTer::ConnectToMiSTer(const std::string& ip, uint16_t port)
{
    if (!m_groovy->Connect(ip, port)) {
        LOG->Warn("Failed to connect to MiSTer at %s:%d", ip.c_str(), port);
        return false;
    }

    LOG->Info("Connected to MiSTer at %s:%d", ip.c_str(), port);
    return true;
}

void LowLevelWindow_X11_MiSTer::DisconnectFromMiSTer()
{
    if (m_groovy->IsConnected()) {
        m_groovy->Disconnect();
        LOG->Info("Disconnected from MiSTer");
    }
}

bool LowLevelWindow_X11_MiSTer::IsMiSTerConnected() const
{
    return m_groovy->IsConnected();
}

void LowLevelWindow_X11_MiSTer::SetCompression(GroovyLZ4Mode mode)
{
    m_compressionMode = mode;
}

std::string LowLevelWindow_X11_MiSTer::TryVideoMode(const VideoModeParams& p, bool& newDeviceOut)
{
    // First, set up local X11 window
    std::string error = LowLevelWindow_X11::TryVideoMode(p, newDeviceOut);
    if (!error.empty()) {
        return error;
    }

    // Store dimensions for framebuffer capture
    m_captureWidth = p.width;
    m_captureHeight = p.height;

    // Allocate frame buffer (RGB888)
    size_t bufferSize = m_captureWidth * m_captureHeight * 3;
    m_frameBuffer.resize(bufferSize);

    // Connect to MiSTer if configured
    // TODO: Read from preferences
    std::string misterIP = "192.168.30.81";  // PREFSMAN->m_sMiSTerIP.Get()
    bool misterEnabled = true;               // PREFSMAN->m_bMiSTerEnable.Get()

    if (misterEnabled && !m_groovy->IsConnected()) {
        if (ConnectToMiSTer(misterIP)) {
            // Initialize MiSTer streaming
            if (!m_groovy->CmdInit(m_compressionMode,
                                   GroovyAudioRate::OFF,  // Audio disabled for now
                                   0,
                                   GroovyRGBMode::RGB888)) {
                LOG->Warn("MiSTer: CmdInit failed");
            }

            // Set modeline
            GroovyModeline modeline = VideoModeToModeline(p);
            if (!m_groovy->CmdSwitchres(modeline)) {
                LOG->Warn("MiSTer: CmdSwitchres failed");
            }

            LOG->Info("MiSTer: Configured for %dx%d", p.width, p.height);
        }
    }

    return "";
}

void LowLevelWindow_X11_MiSTer::SwapBuffers()
{
    // Send frame to MiSTer if connected
    if (m_groovy->IsConnected()) {
        CaptureFramebuffer();

        if (!m_frameBuffer.empty()) {
            m_groovy->CmdBlit(m_frameBuffer.data(),
                              m_frameBuffer.size(),
                              m_frameNumber,
                              0);  // vSync=0 for auto

            // Wait for ACK (provides natural frame pacing)
            m_groovy->WaitSync(16);  // 16ms timeout (~60fps)

            m_frameNumber++;
        }
    }

    // Also swap local display if enabled
    if (m_localDisplayEnabled) {
        LowLevelWindow_X11::SwapBuffers();
    }
}

void LowLevelWindow_X11_MiSTer::CaptureFramebuffer()
{
    if (m_captureWidth <= 0 || m_captureHeight <= 0) return;

    // Read pixels from OpenGL framebuffer
    glReadPixels(0, 0, m_captureWidth, m_captureHeight,
                 GL_RGB, GL_UNSIGNED_BYTE, m_frameBuffer.data());

    // OpenGL framebuffer is bottom-up, MiSTer expects top-down
    FlipFramebufferVertical();
}

void LowLevelWindow_X11_MiSTer::FlipFramebufferVertical()
{
    const int rowSize = m_captureWidth * 3;
    std::vector<uint8_t> tempRow(rowSize);

    for (int y = 0; y < m_captureHeight / 2; y++) {
        int topOffset = y * rowSize;
        int bottomOffset = (m_captureHeight - 1 - y) * rowSize;

        memcpy(tempRow.data(), &m_frameBuffer[topOffset], rowSize);
        memcpy(&m_frameBuffer[topOffset], &m_frameBuffer[bottomOffset], rowSize);
        memcpy(&m_frameBuffer[bottomOffset], tempRow.data(), rowSize);
    }
}

GroovyModeline LowLevelWindow_X11_MiSTer::VideoModeToModeline(const VideoModeParams& p)
{
    // For now, return hardcoded modelines based on resolution
    // TODO: Integrate switchres library for dynamic calculation

    if (p.width == 640 && p.height == 480) {
        return GroovyModelines::VGA_640x480_60;
    }
    else if (p.width == 320 && p.height == 240) {
        return GroovyModelines::ARCADE_320x240_60;
    }
    else {
        // Default to VGA and scale
        LOG->Warn("MiSTer: No modeline for %dx%d, using 640x480", p.width, p.height);
        return GroovyModelines::VGA_640x480_60;
    }
}
```

### 5.3 Preferences Integration

**Modify: `src/PrefsManager.h`**

```cpp
// Add in the preference declarations section (around line 200+):

// MiSTer/GroovyMiSTer settings
Preference<bool>        m_bMiSTerEnable;
Preference<std::string> m_sMiSTerIP;
Preference<int>         m_iMiSTerCompression;  // 0=RAW, 1=LZ4, 2=LZ4_HC
Preference<bool>        m_bMiSTerLocalDisplay;
```

**Modify: `src/PrefsManager.cpp`**

```cpp
// Add in the preference initialization (in constructor):

m_bMiSTerEnable("MiSTerEnable", false),
m_sMiSTerIP("MiSTerIP", "192.168.30.81"),
m_iMiSTerCompression("MiSTerCompression", 1),
m_bMiSTerLocalDisplay("MiSTerLocalDisplay", true),
```

### 5.4 Build System Integration

**Modify: `CMake/DefineOptions.cmake`**

```cmake
# Add after other options (around line 30):

option(WITH_MISTER "Enable GroovyMiSTer CRT output support" OFF)
```

**Modify: `src/CMakeData-arch.cmake`**

```cmake
# Find the Linux/X11 LowLevelWindow section and add:

if(WITH_MISTER)
    list(APPEND SMDATA_ARCH_LOWLEVELWINDOW_SRC
        arch/LowLevelWindow/LowLevelWindow_X11_MiSTer.cpp
        arch/LowLevelWindow/GroovyMister/GroovyMister.cpp
    )
    list(APPEND SMDATA_ARCH_LOWLEVELWINDOW_HPP
        arch/LowLevelWindow/LowLevelWindow_X11_MiSTer.h
        arch/LowLevelWindow/GroovyMister/GroovyMister.h
        arch/LowLevelWindow/GroovyMister/GroovyMisterTypes.h
    )
endif()
```

**Modify: `CMakeLists.txt` (root) or `src/CMakeLists.txt`**

```cmake
# Add LZ4 dependency when MiSTer is enabled:

if(WITH_MISTER)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LZ4 REQUIRED liblz4)
    target_include_directories(ITGmania PRIVATE ${LZ4_INCLUDE_DIRS})
    target_link_libraries(ITGmania PRIVATE ${LZ4_LIBRARIES})
    target_compile_definitions(ITGmania PRIVATE WITH_MISTER)
endif()
```

### 5.5 Window Factory Integration

**Modify: `src/arch/LowLevelWindow/LowLevelWindow.cpp`** (or equivalent factory)

```cpp
// In the LowLevelWindow creation function, add:

#ifdef WITH_MISTER
#include "LowLevelWindow_X11_MiSTer.h"
#endif

LowLevelWindow* LowLevelWindow::Create(const std::string& driver)
{
#ifdef WITH_MISTER
    // Check if MiSTer output is enabled
    if (PREFSMAN->m_bMiSTerEnable.Get()) {
        return new LowLevelWindow_X11_MiSTer;
    }
#endif

    // Default X11 window
    return new LowLevelWindow_X11;
}
```

---

## 6. Testing & Validation

### 6.1 Build Commands

```bash
cd ~/src/itgmania

# Configure with MiSTer support
cmake -B build -DWITH_MISTER=ON -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build -j$(nproc)
```

### 6.2 Configuration

Create/edit `~/.itgmania/Save/Preferences.ini`:

```ini
MiSTerEnable=1
MiSTerIP=192.168.30.81
MiSTerCompression=1
MiSTerLocalDisplay=1
```

### 6.3 Test Procedure

1. **MiSTer Setup:**
   - Load Groovy core on MiSTer
   - Note MiSTer IP address (check via menu or DHCP)
   - Direct ethernet connection recommended

2. **Initial Test:**
   - Launch ITGMania with MiSTer enabled
   - Check console/log for "Connected to MiSTer"
   - Verify video appears on CRT
   - Check local display also shows (if enabled)

3. **Performance Test:**
   - Monitor frame rate (should be 60 FPS)
   - Check for stuttering or tearing
   - Verify audio plays correctly (local)

### 6.4 Troubleshooting

| Issue | Possible Cause | Solution |
|-------|---------------|----------|
| No connection | Wrong IP | Verify MiSTer IP in preferences |
| Black screen on CRT | Modeline mismatch | Check resolution settings |
| Stuttering | Network latency | Use direct ethernet, not WiFi |
| Frame drops | Compression overhead | Try RAW mode (compression=0) |
| glReadPixels slow | GPU stall | Future: implement PBO async readback |

---

## 7. Future Expansion: Audio Streaming

### 7.1 Architecture for Audio

The current implementation leaves audio expansion points:

1. **GroovyMister::CmdAudio()** - Stub ready for implementation
2. **CMD_INIT audioRate/channels** - Already configurable
3. **Sample format:** 16-bit signed PCM, 44100Hz recommended

### 7.2 Audio Integration Point

For audio streaming, hook into `RageSoundManager`:

```cpp
// src/RageSoundManager.cpp - potential hook point
// In the audio mixing callback, after samples are ready:

if (g_pGroovyMister && g_pGroovyMister->IsConnected()) {
    g_pGroovyMister->CmdAudio(mixedSamples, sampleCount);
}
```

### 7.3 Reference Implementation

See `~/src/GroovyMAME/src/osd/modules/render/drawnogpu.cpp`:
- Search for `add_audio_to_recording`
- Shows audio sample capture and CMD_AUDIO transmission

---

## 8. Reference Code Locations

### 8.1 Protocol Details

| Topic | File | Key Lines/Functions |
|-------|------|---------------------|
| Command constants | `~/src/Groovy_MiSTer/api/groovymister.h` | CMD_* defines |
| Packet building | `~/src/Groovy_MiSTer/api/groovymister.cpp` | CmdBlit(), CmdInit() |
| LZ4 compression | `~/src/GroovyMAME/src/osd/modules/render/drawnogpu.cpp` | ~line 400-500 |
| Frame timing | `~/src/GroovyMAME/src/osd/modules/render/drawnogpu.cpp` | WaitSync(), timing calc |
| ACK parsing | `~/src/MiSTerCast/Library/MiSTerCastLib/groovymister.cpp` | ReceiveBlitAck() |

### 8.2 ITGMania Architecture

| Component | File | Purpose |
|-----------|------|---------|
| Window base | `src/arch/LowLevelWindow/LowLevelWindow.h` | Abstract interface |
| X11 window | `src/arch/LowLevelWindow/LowLevelWindow_X11.cpp` | SwapBuffers impl |
| Display base | `src/RageDisplay.h` | Video mode params |
| OpenGL render | `src/RageDisplay_OGL.cpp` | BeginFrame/EndFrame |
| Game loop | `src/GameLoop.cpp` | Main loop timing |
| Preferences | `src/PrefsManager.h` | Config pattern |
| Build arch | `src/CMakeData-arch.cmake` | Platform sources |

---

## 9. Summary Checklist

### Files to Create:
- [ ] `src/arch/LowLevelWindow/GroovyMister/GroovyMisterTypes.h`
- [ ] `src/arch/LowLevelWindow/GroovyMister/GroovyMister.h`
- [ ] `src/arch/LowLevelWindow/GroovyMister/GroovyMister.cpp`
- [ ] `src/arch/LowLevelWindow/LowLevelWindow_X11_MiSTer.h`
- [ ] `src/arch/LowLevelWindow/LowLevelWindow_X11_MiSTer.cpp`

### Files to Modify:
- [ ] `CMake/DefineOptions.cmake` - Add WITH_MISTER option
- [ ] `src/CMakeData-arch.cmake` - Add new source files
- [ ] `CMakeLists.txt` or `src/CMakeLists.txt` - LZ4 dependency
- [ ] `src/PrefsManager.h` - Add MiSTer preferences
- [ ] `src/PrefsManager.cpp` - Initialize MiSTer preferences
- [ ] `src/arch/LowLevelWindow/LowLevelWindow.cpp` - Factory selection

### Dependencies:
- [ ] LZ4 library (`liblz4-dev` on Debian/Ubuntu)

### Build & Test:
- [ ] Configure with `-DWITH_MISTER=ON`
- [ ] Build successfully
- [ ] Test connection to MiSTer
- [ ] Verify video output on CRT
- [ ] Confirm local audio works

---

*Document created: Implementation plan for GroovyMiSTer video driver in ITGMania*
*Reference codebases analyzed: Groovy_MiSTer, GroovyMAME, MiSTerCast, RetroArch, ITGMania*
