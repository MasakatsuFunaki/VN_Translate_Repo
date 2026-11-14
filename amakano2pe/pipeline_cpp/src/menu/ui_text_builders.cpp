// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "menu/ui_text_builders.h"

#include "menu/core/fes_script_codec.h"
#include "menu/core/menu_patch_container.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ama::menu {

namespace {

struct StringReplacementRule {
    std::string_view target;
    std::string_view replacement;
    std::size_t expected_count = 1;
};

struct LocalizationEntry {
    std::string_view id_tag;
    std::string_view english_text;
};

constexpr std::size_t kExpectedUiRecordCount = 8;
constexpr std::size_t kExpectedConfigBasicMsgSampleCount = 6;
constexpr std::size_t kExpectedConfigBasicAutoSampleCount = 6;
constexpr std::size_t kExpectedDualOccurrenceCount = 2;
constexpr std::size_t kExpectedSingleOccurrenceCount = 1;
constexpr std::size_t kApproximateBytesPerLocalizationEntry = 64;

constexpr std::string_view kCrLf = "\r\n";
constexpr std::string_view kFesArchiveRelPath = "fes.int";
constexpr std::string_view kConfigArchiveRelPath = "config.int";

constexpr std::string_view kLanguageTablePrimaryRelPath = "language_.txt";
constexpr std::string_view kLanguageTableExportRelPath = "export\\lang.zt";
constexpr std::string_view kLanguageTableBootRelPath = "language.txt";

constexpr std::string_view kStartupXmlRelPath = "config\\startup.xml";
constexpr std::string_view kSoundconfXmlRelPath = "config\\soundconf.xml";

constexpr std::string_view kConfigBasicFesEntryName = "config_basic.fes";
constexpr std::string_view kConfigBasicFesTargetRelPath = "fes\\config_basic.fes";

constexpr std::string_view kInputTxtFesEntryName = "inputtxt.fes";
constexpr std::string_view kInputTxtFesTargetRelPath = "fes\\inputtxt.fes";

constexpr std::string_view kAdvXmlEntryName = "adv.xml";
constexpr std::string_view kAdvXmlTargetRelPath = "config\\adv.xml";

constexpr LocalizationEntry kLocalizationTableEntries[] = {
    // Section 2.1: cs2.exe - $0000-* Core Engine & Startup Messages (52 IDs)
    {"$0000-0001$", "Error"},
    {"$0000-0002$", "Confirmation"},
    {"$0000-0010$", "Failed to load archive."},
    {"$0000-0011$", "Startup check failed (1)."},
    {"$0000-0012$", "Startup check failed (2)."},
    {"$0000-0013$", "Startup check failed (3)."},
    {"$0000-0014$", "Startup check failed (4)."},
    {"$0000-0015$", "Key code not found."},
    {"$0000-0016$", "Starting in debug mode."},
    {"$0000-0017$", "Color Depth Check"},
    {"$0000-0018$", "Display color depth is not set to 16-bit or 32-bit color. Start anyway?"},
    {"$0000-0019$", "Cannot switch to %dx%d fullscreen mode."},
    {"$0000-0020$", "Startup Error"},
    {"$0000-0021$", "Failed to initialize Direct3D."},
    {"$0000-0022$", "Failed to create texture."},
    {"$0000-0023$", "Cannot start the game because DirectX 9.0c or later is not installed. Please install the latest DirectX."},
    {"$0000-0024$", "Cannot start the game because the shader version is %d.%d or lower."},
    {"$0000-0025$", "Cannot start the game because the vertex shader version is %d.%d or lower."},
    {"$0000-0026$", "Cannot start the game because the pixel shader version is %d.%d or lower."},
    {"$0000-0027$", "Initialization Error"},
    {"$0000-0028$", "Failed to initialize the game."},
    {"$0000-0029$", "Exit Confirmation"},
    {"$0000-0030$", "Are you sure you want to exit?"},
    {"$0000-0031$", "Return to the title screen?"},
    {"$0000-0032$", "%s\tCtrl + %c"},
    {"$0000-0033$", "%s\tAlt + %c"},
    {"$0000-0034$", "%s\tShift + %c"},
    {"$0000-0035$", "%s\t%c"},
    {"$0000-0036$", "Cannot switch to fullscreen mode because it is currently disabled or unavailable."},
    {"$0000-0037$", "Screen Mode Error"},
    {"$0000-0038$", "Cannot start in windowed mode because display resolution is below %d x %d. Start in fullscreen mode instead?"},
    {"$0000-0039$", "Cannot start the game because display resolution is below %d x %d and fullscreen mode is unavailable."},
    {"$0000-0040$", "Required files were not found. The installation may be incomplete. Please reinstall the game."},
    {"$0000-0041$", "Cannot start the game because Steam is not running or authentication failed."},
    {"$0000-0050$", "Failed to load texture [%s]."},
    {"$0000-0051$", "Script [%s] not found."},
    {"$0000-0052$", "Failed to save image."},
    {"$0000-0053$", "Save Image"},
    {"$0000-0054$", "$nr$"},
    {"$0000-0055$", "Failed to initialize the text-to-speech engine (OpenJTalk)."},
    {"$0000-0060$", "Standard"},
    {"$0000-0061$", "200%"},
    {"$0000-0062$", "150%"},
    {"$0000-0063$", "100%"},
    {"$0000-0064$", "75%"},
    {"$0000-0065$", "50%"},
    {"$0000-0070$", "Unassigned"},
    {"$0000-0071$", "A Button"},
    {"$0000-0072$", "B Button"},
    {"$0000-0073$", "X Button"},
    {"$0000-0074$", "Y Button"},
    {"$0000-0075$", "Button %d"},

    // Section 2.2: cs2.exe - $0010-* DirectX / Audio / DShow / Save Engine Errors (69 IDs)
    {"$0010-0001$", "Cannot run because the DirectX version is outdated. Please install DirectX 8.1 or later."},
    {"$0010-0002$", "Failed to create Direct3D object."},
    {"$0010-0003$", "Failed to get display mode."},
    {"$0010-0004$", "Failed to create Direct3D device."},
    {"$0010-0005$", "Failed to create sprite object."},
    {"$0010-0006$", "Failed to get back buffer."},
    {"$0010-0007$", "Failed to create render target."},
    {"$0010-0008$", "Failed to create depth-stencil surface."},
    {"$0010-0009$", "Failed to create texture. VRAM may be insufficient."},
    {"$0010-0010$", "Failed to lock surface."},
    {"$0010-0011$", "Failed to unlock surface."},
    {"$0010-0012$", "Device lost. Attempting recovery."},
    {"$0010-0013$", "Failed to reset device."},
    {"$0010-0014$", "Failed to set viewport."},
    {"$0010-0015$", "Failed to create vertex buffer."},
    {"$0010-0016$", "Failed to create index buffer."},
    {"$0010-0017$", "Failed to compile shader."},
    {"$0010-0018$", "Failed to create vertex shader."},
    {"$0010-0019$", "Failed to create pixel shader."},
    {"$0010-0020$", "Failed to create DirectSound object."},
    {"$0010-0021$", "Failed to set DirectSound cooperative level."},
    {"$0010-0022$", "Failed to create primary sound buffer."},
    {"$0010-0023$", "Failed to set primary buffer format."},
    {"$0010-0024$", "Failed to create secondary sound buffer."},
    {"$0010-0025$", "Failed to lock sound buffer."},
    {"$0010-0026$", "Failed to play sound buffer."},
    {"$0010-0027$", "Failed to stop sound buffer."},
    {"$0010-0028$", "No sound device found. Starting without sound."},
    {"$0010-0029$", "Failed to create DirectShow graph builder."},
    {"$0010-0030$", "Failed to get media control interface."},
    {"$0010-0031$", "Failed to get media seeking interface."},
    {"$0010-0032$", "Failed to get video window interface."},
    {"$0010-0033$", "Failed to get basic audio interface."},
    {"$0010-0034$", "Failed to render movie file [%s]. Required codecs may not be installed."},
    {"$0010-0035$", "Failed to start movie playback."},
    {"$0010-0036$", "Failed to create VMR9 filter."},
    {"$0010-0037$", "Failed to add VMR9 filter."},
    {"$0010-0038$", "Failed to allocate memory. The application will now exit."},
    {"$0010-0039$", "An error occurred while reading file [%s]."},
    {"$0010-0040$", "An error occurred while writing file [%s]."},
    {"$0010-0041$", "Invalid data format [%s]."},
    {"$0010-0042$", "Failed to decompress data [%s]."},
    {"$0010-0043$", "Failed to create font."},
    {"$0010-0044$", "Failed to register window class [%s]."},
    {"$0010-0045$", "Failed to create window."},
    {"$0010-0046$", "Failed to create timer."},
    {"$0010-0047$", "Failed to create thread."},
    {"$0010-0048$", "A system error occurred (Code: %d)."},
    {"$0010-0049$", "Script syntax error (File: %s, Line: %d)."},
    {"$0010-0050$", "Undefined command called [%s]."},
    {"$0010-0051$", "Stack overflow occurred."},
    {"$0010-0052$", "Division by zero error occurred."},
    {"$0010-0053$", "Label [%s] not found."},
    {"$0010-0054$", "Variable index out of bounds."},
    {"$0010-0055$", "Save data version mismatch."},
    {"$0010-0056$", "Save data is corrupted."},
    {"$0010-0057$", "Failed to load save data."},
    {"$0010-0058$", "Failed to write save data. Please check available disk space."},
    {"$0010-0059$", "Failed to load system data."},
    {"$0010-0060$", "Failed to write system data."},
    {"$0010-0061$", "Failed to generate capture image."},
    {"$0010-0062$", "Failed to initialize network."},
    {"$0010-0063$", "Failed to connect to server."},
    {"$0010-0064$", "Failed to send data."},
    {"$0010-0065$", "Failed to receive data."},
    {"$0010-0066$", "Connection timed out."},
    {"$0010-0067$", "Failed to load plugin [%s]."},
    {"$0010-0068$", "Failed to initialize plugin [%s]."},
    {"$0010-0069$", "An unknown error occurred."},

    // Section 2.3: cs2.exe - $0011-* Win32 Menu Bar (RT_MENU 104 / 105) & About Dialog (RT_DIALOG 101) (25 IDs)
    {"$0011-0001$", "File(&F)"},
    {"$0011-0002$", "Return to Title(&R)"},
    {"$0011-0003$", "Save Screenshot(&G)..."},
    {"$0011-0004$", "Exit(&X)\tAlt + F4"},
    {"$0011-0005$", "View(&V)"},
    {"$0011-0006$", "Windowed(&W)\tAlt + Enter"},
    {"$0011-0007$", "Fullscreen(&F)\tAlt + Enter"},
    {"$0011-0008$", "Window Size(&S)"},
    {"$0011-0013$", "Show Frame Rate(&R)"},
    {"$0011-0014$", "Settings(&C)"},
    {"$0011-0015$", "Configuration(&C)..."},
    {"$0011-0016$", "Always on Top(&T)"},
    {"$0011-0017$", "Help(&H)"},
    {"$0011-0018$", "Manual(&M)...\tF1"},
    {"$0011-0019$", "About(&A)..."},
    {"$0011-0020$", "Version Information"},
    {"$0011-0021$", "OK"},
    {"$0011-0022$", "Copy System Info to Clipboard"},
    {"$0011-0023$", "Image Export Settings"},
    {"$0011-0024$", "$nr$"},
    {"$0011-0025$", "$nr$"},
    {"$0011-0026$", "Scene Select(&S)..."},
    {"$0011-0027$", "Tweet(&T)..."},
    {"$0011-0028$", "Text-to-Speech(&S)"},
    {"$0011-0029$", "Always Render(&A)"},

    // Section 2.4: cs2.exe - $0012-*..$0034-* Native Win32 Config & Utility Dialogs (169 IDs)
    {"$0012-0001$", "Settings"},
    {"$0012-0002$", "Reset all settings to their default values?"},
    {"$0012-0003$", "Sample Text"},
    {"$0012-0010$", "Scene Select"},
    {"$0012-0011$", "Select a scene to start and click OK, or double-click a scene in the list."},
    {"$0012-0012$", "OK"},
    {"$0012-0013$", "Cancel"},
    {"$0020-0001$", "System"},
    {"$0020-0002$", "Screen Mode"},
    {"$0020-0003$", "Start in Windowed Mode"},
    {"$0020-0004$", "Start in Fullscreen Mode"},
    {"$0020-0005$", "Ask on Every Startup"},
    {"$0020-0006$", "Hardware Processing"},
    {"$0020-0007$", "Use Texture Compression"},
    {"$0020-0008$", "Multimedia Features"},
    {"$0020-0009$", "Disable Sound Playback"},
    {"$0020-0010$", "Disable Movie Playback"},
    {"$0020-0011$", "* Changes on this page take effect on next startup."},
    {"$0020-0012$", "Enable VSync"},
    {"$0020-0013$", "Enable Anti-Aliasing"},
    {"$0020-0014$", "Use 16-bit Textures"},
    {"$0020-0015$", "Disable Shaders"},
    {"$0021-0001$", "Message 1"},
    {"$0021-0002$", "Text Display Speed"},
    {"$0021-0003$", "Display Read Text Instantly"},
    {"$0021-0004$", "Auto Mode"},
    {"$0021-0005$", "Stop Auto Mode at Choices"},
    {"$0021-0006$", "Fast"},
    {"$0021-0007$", "Slow"},
    {"$0021-0008$", "Reset to Default"},
    {"$0021-0009$", "Allow Dragging the Message Window"},
    {"$0021-0010$", "Show Message Window Frame"},
    {"$0021-0011$", "Wait for Voice Playback"},
    {"$0021-0012$", "Skip Unread Text as Well"},
    {"$0022-0001$", "Message 2"},
    {"$0022-0002$", "Font"},
    {"$0022-0003$", "Text Shadow"},
    {"$0022-0004$", "None"},
    {"$0022-0005$", "Drop Shadow"},
    {"$0022-0006$", "Outline (Thin)"},
    {"$0022-0007$", "Outline (Thick)"},
    {"$0022-0008$", "Use Proportional Font"},
    {"$0022-0009$", "Sample"},
    {"$0023-0001$", "Window"},
    {"$0023-0002$", "Window Color"},
    {"$0023-0003$", "Red"},
    {"$0023-0004$", "Green"},
    {"$0023-0005$", "Blue"},
    {"$0023-0006$", "Opacity"},
    {"$0023-0007$", "Sample"},
    {"$0023-0008$", "Window Opacity"},
    {"$0023-0009$", "Clear"},
    {"$0023-0010$", "Opaque"},
    {"$0024-0001$", "Auto Mode Details"},
    {"$0024-0002$", "1920"},
    {"$0024-0003$", "Base Wait (ms)"},
    {"$0024-0004$", "1080"},
    {"$0024-0005$", "Per-Character Time (ms)"},
    {"$0024-0006$", "Wait for Voice Playback When Voiced"},
    {"$0024-0007$", "Presets"},
    {"$0025-0001$", "Sound"},
    {"$0025-0002$", "Mute"},
    {"$0025-0003$", "Volume"},
    {"$0025-0004$", "System Voice"},
    {"$0025-0005$", "Master Volume"},
    {"$0025-0006$", "BGM"},
    {"$0025-0007$", "SE"},
    {"$0025-0008$", "Voice"},
    {"$0025-0009$", "System SE"},
    {"$0026-0001$", "Voice"},
    {"$0026-0002$", "Per-Character Voice Settings"},
    {"$0026-0003$", "All ON"},
    {"$0026-0004$", "All OFF"},
    {"$0027-0001$", "System Voice"},
    {"$0027-0002$", "System Voice Character"},
    {"$0027-0003$", "Random (Choose from Checked Characters)"},
    {"$0027-0004$", "Individual System Voice Settings"},
    {"$0028-0001$", "Other"},
    {"$0028-0002$", "Mouse Cursor"},
    {"$0028-0003$", "Use Custom Cursor"},
    {"$0028-0004$", "Snap Cursor to Buttons Automatically"},
    {"$0028-0005$", "Hide Cursor When Inactive"},
    {"$0028-0006$", "Screensaver"},
    {"$0028-0007$", "Disable Screensaver While Playing"},
    {"$0028-0008$", "Boss Key (Minimize to System Tray)"},
    {"$0028-0009$", "Miscellaneous"},
    {"$0028-0010$", "Skip Screen Transition Effects (Fades, etc.)"},
    {"$0029-0001$", "System 2"},
    {"$0029-0002$", "Folder Settings"},
    {"$0029-0003$", "Screenshot Save Folder"},
    {"$0029-0004$", "Browse..."},
    {"$0029-0005$", "Export Folder"},
    {"$0029-0006$", "Import Folder"},
    {"$0029-0007$", "Other"},
    {"$0029-0008$", "Capture Using GDI"},
    {"$0029-0009$", "Output Debug Information"},
    {"$0029-0010$", "Enable Multi-Core CPU Optimization"},
    {"$0029-0011$", "Cache Images in Memory"},
    {"$0030-0001$", "Fullscreen"},
    {"$0030-0002$", "Fullscreen Mode Settings"},
    {"$0030-0003$", "Do Not Change Display Resolution (Borderless Windowed)"},
    {"$0030-0004$", "Scaling Mode"},
    {"$0030-0005$", "Stretch to Fill Screen"},
    {"$0030-0006$", "Scale Preserving Aspect Ratio"},
    {"$0030-0007$", "Do Not Scale (1:1 Pixel Mapping)"},
    {"$0030-0008$", "Use Bilinear Filtering"},
    {"$0030-0009$", "Adjust Menu Bar Display Position"},
    {"$0031-0001$", "Settings Mode"},
    {"$0031-0002$", "Settings Display Mode"},
    {"$0031-0003$", "Use Simple Settings"},
    {"$0031-0004$", "Use Advanced Settings"},
    {"$0032-0001$", "Key Settings"},
    {"$0032-0002$", "Function"},
    {"$0032-0003$", "Keyboard"},
    {"$0032-0004$", "Key"},
    {"$0032-0005$", "Mouse"},
    {"$0032-0006$", "Button"},
    {"$0032-0007$", "Gamepad"},
    {"$0032-0008$", "Enable Turbo"},
    {"$0033-0001$", "3D Vision"},
    {"$0033-0002$", "Stereoscopic 3D Settings"},
    {"$0033-0003$", "Enable Stereoscopic 3D"},
    {"$0033-0004$", "Mode"},
    {"$0033-0005$", "Parallax"},
    {"$0033-0006$", "Depth"},
    {"$0034-0001$", "Tweet"},
    {"$0034-0002$", "Attach Screenshot"},
    {"$0034-0003$", "Post"},
    {"$0034-0004$", "Cancel"},
    {"$0034-0005$", "Auth Settings..."},
    {"$0034-0006$", "Please authenticate with Twitter."},
    {"$0034-0007$", "Your browser will open. Please enter the displayed PIN code."},
    {"$0034-0008$", "Authentication succeeded."},
    {"$0034-0009$", "Authentication failed. Please check the PIN code."},
    {"$0034-0010$", "Tweet posted."},
    {"$0034-0011$", "Failed to post tweet."},
    {"$0034-0012$", "Character limit exceeded."},
    {"$0034-0013$", "Please enter a message."},
    {"$0034-0014$", "Connecting..."},
    {"$0034-0015$", "Account unlinked."},
    {"$0034-0016$", "Unlink"},
    {"$0034-0017$", "Authenticated: @%s"},
    {"$0034-0018$", "Not Authenticated"},
    {"$0034-0019$", "%d characters remaining"},
    {"$0034-0020$", "Authentication"},
    {"$0034-0021$", "Enter the PIN code shown in your browser and click OK."},
    {"$0034-0022$", "OK"},
    {"$0034-0023$", "Cancel"},
    {"$0034-0030$", "Text-to-Speech Settings"},
    {"$0034-0031$", "Speech Rate"},
    {"$0034-0032$", "Voice Pitch"},
    {"$0034-0033$", "Voice Type"},
    {"$0034-0034$", "Close"},
    {"$0034-0035$", "Normal"},
    {"$0034-0036$", "Bright"},
    {"$0034-0037$", "Calm"},
    {"$0034-0038$", "Sad"},
    {"$0034-0039$", "Angry"},
    {"$0034-0040$", "Whisper"},

    // Section 2.5: cs2.exe - $0035-*, $0036-*, $0045-*, and $0100-* String Table & Shell Errors (114 IDs)
    {"$0035-0000$", "Play the game in a window."},
    {"$0035-0001$", "Play the game in fullscreen mode."},
    {"$0035-0002$", "Show a dialog to choose windowed or fullscreen mode each time the game starts."},
    {"$0035-0003$", "Use texture compression. Uncheck this if performance is slow."},
    {"$0035-0004$", "Disable all sound playback."},
    {"$0035-0005$", "Skip movie playback during the game."},
    {"$0035-0006$", "Sync rendering with monitor refresh rate to reduce screen tearing."},
    {"$0035-0007$", "Reduce jagged edges during 3D rendering."},
    {"$0035-0008$", "Reduce texture color depth to lower video memory usage."},
    {"$0035-0009$", "Disable shaders. Check this if graphics do not display properly."},
    {"$0035-0010$", "Temporarily mute sound."},
    {"$0035-0011$", "Adjust the volume of all sounds."},
    {"$0035-0012$", "Toggle voice playback and adjust volume for each character."},
    {"$0035-0013$", "Turn ON voices for all characters."},
    {"$0035-0014$", "Turn OFF voices for all characters."},
    {"$0035-0015$", "Play system voices randomly."},
    {"$0035-0016$", "Select characters for system voice playback."},
    {"$0035-0017$", "Use the custom game mouse cursor."},
    {"$0035-0018$", "Automatically move the mouse cursor to buttons when dialogs appear."},
    {"$0035-0019$", "Hide the mouse cursor after a period of inactivity."},
    {"$0035-0020$", "Prevent the screensaver from starting while playing."},
    {"$0035-0021$", "Minimize to the system tray instead of the taskbar."},
    {"$0035-0022$", "Specify the folder where captured screenshots are saved."},
    {"$0035-0023$", "Open the folder selection dialog."},
    {"$0035-0024$", "Specify the folder for exported data."},
    {"$0035-0025$", "Specify the folder for imported data."},
    {"$0035-0026$", "Use GDI for screen capture. Check this if normal capture fails."},
    {"$0035-0027$", "Output debug log information."},
    {"$0035-0028$", "Improve performance on multi-core CPUs."},
    {"$0035-0029$", "Cache images in memory to improve loading speed."},
    {"$0035-0030$", "Keep current monitor resolution in fullscreen mode and scale the display."},
    {"$0035-0031$", "Stretch to fill the entire screen, ignoring aspect ratio."},
    {"$0035-0032$", "Scale to fit the monitor while preserving the game's aspect ratio."},
    {"$0035-0033$", "Display centered at original resolution without scaling."},
    {"$0035-0034$", "Smoothly interpolate images when scaling."},
    {"$0035-0035$", "Adjust menu bar position in fullscreen mode so it stays on screen."},
    {"$0035-0036$", "Set the text display speed."},
    {"$0035-0037$", "Select the font used in the game."},
    {"$0035-0038$", "Select a wait-time preset for Auto Mode."},
    {"$0035-0039$", "Configure Auto Mode timing in detail."},
    {"$0035-0040$", "Configure text shadow style."},
    {"$0035-0041$", "Draw a black drop shadow at the bottom-right of characters."},
    {"$0035-0042$", "Base wait time."},
    {"$0035-0043$", "Draw a thin black outline around characters."},
    {"$0035-0044$", "Draw a thick black outline around characters."},
    {"$0035-0045$", "Set the red component of the message window color."},
    {"$0035-0046$", "Set the green component of the message window color."},
    {"$0035-0047$", "Set the blue component of the message window color."},
    {"$0035-0048$", "Set the opacity of the message window."},
    {"$0035-0049$", "When checked, allows dragging the message window with the mouse."},
    {"$0035-0050$", "When unchecked, hides the message window frame and shows only buttons."},
    {"$0035-0051$", "Reset settings to their default values."},
    {"$0035-0052$", "Use the simple view for configuration settings."},
    {"$0035-0053$", "Use the advanced view for configuration settings."},
    {"$0035-0054$", "Set the text advance speed during Auto Mode."},
    {"$0035-0055$", "Allow pressing the skip button on the message window even for unread text."},
    {"$0035-0056$", "Use a proportional font. Depending on the font, text may overflow the message window."},
    {"$0035-0057$", "Select the character for system voices on the title screen and menus."},
    {"$0035-0058$", "Skip fade-in and fade-out transitions when switching screens."},
    {"$0035-0059$", "Enable stereoscopic 3D display."},
    {"$0035-0060$", "Select the stereoscopic 3D display mode."},
    {"$0035-0061$", "Adjust stereoscopic 3D parallax."},
    {"$0035-0062$", "Adjust stereoscopic 3D depth."},
    {"$0036-0001$", "Please specify a save destination."},
    {"$0036-0002$", "Failed to open source file [%s]."},
    {"$0036-0003$", "Failed to read source file [%s]."},
    {"$0036-0004$", "Failed to create folder [%s]. It may be write-protected. Please choose a different folder."},
    {"$0036-0005$", "Failed to write file [%s]."},
    {"$0036-0006$", "Failed to open file [%s]."},
    {"$0045-0033$", "Select the function you want to configure."},
    {"$0045-0034$", "Specify which keyboard key to assign."},
    {"$0045-0035$", "Specify which mouse button to assign."},
    {"$0045-0036$", "Specify which gamepad button to assign."},
    {"$0045-0037$", "Enable key repeat when held down (disabled for some keys)."},
    {"$0045-0046$", "Adjust BGM volume."},
    {"$0045-0047$", "Adjust BGM volume."},
    {"$0045-0048$", "Adjust BGM volume."},
    {"$0045-0050$", "Adjust sound effect volume."},
    {"$0045-0051$", "Adjust sound effect volume."},
    {"$0045-0067$", "Wait time per character."},
    {"$0045-0069$", "Display previously read text instantly regardless of the text speed setting."},
    {"$0045-0070$", "Choose whether Auto Mode stops when a choice appears."},
    {"$0045-0071$", "Wait for voice playback to finish before advancing in Auto Mode."},
    {"$0100-0000$", "Execution Error"},
    {"$0100-0001$", "Out of memory or system resources."},
    {"$0100-0002$", "The specified file was not found."},
    {"$0100-0003$", "The specified path was not found."},
    {"$0100-0004$", "The .exe file is invalid (non-Win32 .exe or error in .exe image)."},
    {"$0100-0005$", "The operating system denied access to the specified file."},
    {"$0100-0006$", "The file name association is incomplete or invalid."},
    {"$0100-0007$", "Could not complete the DDE transaction because another DDE transaction is in progress."},
    {"$0100-0008$", "The DDE transaction failed."},
    {"$0100-0009$", "Could not complete the DDE transaction because the request timed out."},
    {"$0100-0010$", "The specified dynamic-link library (DLL) was not found."},
    {"$0100-0011$", "There is no application associated with the given file name extension."},
    {"$0100-0012$", "There is not enough memory to complete the operation."},
    {"$0100-0013$", "A sharing violation occurred."},

    // Section 2.6: cs2confx.dll - $0500-*..$0520-* System Settings Dialogs & Tooltips (135 IDs)
    {"$0500-0001$", "%s System Settings"},
    {"$0500-0002$", "Reset settings to their default values?"},
    {"$0500-0003$", "Confirmation"},
    {"$0501-0001$", "System"},
    {"$0501-0002$", "Screen Mode"},
    {"$0501-0003$", "Start in Windowed Mode"},
    {"$0501-0004$", "Start in Fullscreen Mode"},
    {"$0501-0005$", "Ask on Every Startup"},
    {"$0501-0006$", "Hardware Processing"},
    {"$0501-0007$", "Use Texture Compression"},
    {"$0501-0008$", "Multimedia Features"},
    {"$0501-0009$", "Disable Sound Playback"},
    {"$0501-0010$", "Disable Movie Playback"},
    {"$0501-0011$", "* Changes on this page take effect on next startup."},
    {"$0501-0012$", "Enable VSync"},
    {"$0501-0013$", "Enable Anti-Aliasing"},
    {"$0501-0014$", "Use 16-bit Textures"},
    {"$0501-0015$", "Disable Shaders"},
    {"$0501-0016$", "Reset to Default"},
    {"$0502-0001$", "Message 1"},
    {"$0502-0002$", "Text Display Speed"},
    {"$0502-0003$", "Display Read Text Instantly"},
    {"$0502-0004$", "Auto Mode"},
    {"$0502-0005$", "Stop Auto Mode at Choices"},
    {"$0502-0006$", "Fast"},
    {"$0502-0007$", "Slow"},
    {"$0502-0008$", "Reset to Default"},
    {"$0502-0009$", "Allow Dragging the Message Window"},
    {"$0502-0010$", "Show Message Window Frame"},
    {"$0502-0011$", "Wait for Voice Playback"},
    {"$0502-0012$", "Skip Unread Text as Well"},
    {"$0503-0001$", "Message 2"},
    {"$0503-0002$", "Font"},
    {"$0503-0003$", "Text Shadow"},
    {"$0503-0004$", "None"},
    {"$0503-0005$", "Drop Shadow"},
    {"$0503-0006$", "Outline (Thin)"},
    {"$0503-0007$", "Outline (Thick)"},
    {"$0503-0008$", "Use Proportional Font"},
    {"$0503-0009$", "Sample"},
    {"$0504-0001$", "Window"},
    {"$0504-0002$", "Window Color"},
    {"$0504-0003$", "Red"},
    {"$0504-0004$", "Green"},
    {"$0504-0005$", "Blue"},
    {"$0504-0006$", "Opacity"},
    {"$0504-0007$", "Sample"},
    {"$0505-0001$", "Auto Mode Details"},
    {"$0505-0002$", "1920"},
    {"$0505-0003$", "Base Wait (ms)"},
    {"$0505-0004$", "1080"},
    {"$0505-0005$", "Per-Character Time (ms)"},
    {"$0505-0006$", "Wait for Voice Playback When Voiced"},
    {"$0505-0007$", "Presets"},
    {"$0506-0001$", "Sound"},
    {"$0506-0002$", "Mute"},
    {"$0506-0003$", "Volume"},
    {"$0506-0004$", "System Voice"},
    {"$0507-0001$", "Voice"},
    {"$0507-0002$", "Per-Character Voice Settings"},
    {"$0507-0003$", "All ON"},
    {"$0507-0004$", "All OFF"},
    {"$0508-0001$", "System Voice"},
    {"$0508-0002$", "System Voice Character"},
    {"$0508-0003$", "Random (Choose from Checked Characters)"},
    {"$0509-0001$", "Other"},
    {"$0509-0002$", "Mouse Cursor"},
    {"$0509-0003$", "Use Custom Cursor"},
    {"$0509-0004$", "Snap Cursor to Buttons Automatically"},
    {"$0509-0005$", "Hide Cursor When Inactive"},
    {"$0509-0006$", "Screensaver"},
    {"$0509-0007$", "Disable Screensaver While Playing"},
    {"$0509-0008$", "Boss Key (Minimize to System Tray)"},
    {"$0509-0009$", "Miscellaneous"},
    {"$0509-0010$", "Skip Screen Transition Effects (Fades, etc.)"},
    {"$0509-0011$", "Folder Settings"},
    {"$0509-0012$", "Screenshot Save Folder"},
    {"$0509-0013$", "Browse..."},
    {"$0509-0014$", "Export Folder"},
    {"$0509-0015$", "Import Folder"},
    {"$0509-0016$", "Capture Using GDI"},
    {"$0509-0017$", "Output Debug Information"},
    {"$0509-0018$", "Enable Multi-Core CPU Optimization"},
    {"$0509-0019$", "Cache Images in Memory"},
    {"$0509-0020$", "Fullscreen Mode Settings"},
    {"$0509-0021$", "Do Not Change Display Resolution (Borderless Windowed)"},
    {"$0509-0022$", "Scaling Mode"},
    {"$0509-0023$", "Stretch to Fill Screen"},
    {"$0509-0024$", "Scale Preserving Aspect Ratio"},
    {"$0509-0025$", "Do Not Scale (1:1 Pixel Mapping)"},
    {"$0509-0026$", "Use Bilinear Filtering"},
    {"$0509-0027$", "Adjust Menu Bar Display Position"},
    {"$0510-0001$", "Key Settings"},
    {"$0510-0002$", "Function"},
    {"$0510-0003$", "Keyboard"},
    {"$0510-0004$", "Key"},
    {"$0510-0005$", "Mouse"},
    {"$0510-0006$", "Button"},
    {"$0510-0007$", "Gamepad"},
    {"$0510-0008$", "Enable Turbo"},
    {"$0511-0001$", "Starting Up"},
    {"$0511-0002$", "Starting the configuration tool. Please wait..."},
    {"$0520-0001$", "Play the game in a window."},
    {"$0520-0002$", "Play the game in fullscreen mode."},
    {"$0520-0003$", "Show a dialog to choose windowed or fullscreen mode each time the game starts."},
    {"$0520-0004$", "Use texture compression. Uncheck this if performance is slow."},
    {"$0520-0005$", "Disable all sound playback."},
    {"$0520-0006$", "Skip movie playback during the game."},
    {"$0520-0007$", "Sync rendering with monitor refresh rate to reduce screen tearing."},
    {"$0520-0008$", "Reduce jagged edges during 3D rendering."},
    {"$0520-0009$", "Reduce texture color depth to lower video memory usage."},
    {"$0520-0010$", "Disable shaders. Check this if graphics do not display properly."},
    {"$0520-0011$", "Use the custom game mouse cursor."},
    {"$0520-0012$", "Automatically move the mouse cursor to buttons when dialogs appear."},
    {"$0520-0013$", "Hide the mouse cursor after a period of inactivity."},
    {"$0520-0014$", "Prevent the screensaver from starting while playing."},
    {"$0520-0015$", "Minimize to the system tray instead of the taskbar."},
    {"$0520-0016$", "Specify the folder where captured screenshots are saved."},
    {"$0520-0017$", "Open the folder selection dialog."},
    {"$0520-0018$", "Specify the folder for exported data."},
    {"$0520-0019$", "Specify the folder for imported data."},
    {"$0520-0020$", "Use GDI for screen capture. Check this if normal capture fails."},
    {"$0520-0021$", "Output debug log information."},
    {"$0520-0022$", "Improve performance on multi-core CPUs."},
    {"$0520-0023$", "Cache images in memory to improve loading speed."},
    {"$0520-0024$", "Keep current monitor resolution in fullscreen mode and scale the display."},
    {"$0520-0025$", "Stretch to fill the entire screen, ignoring aspect ratio."},
    {"$0520-0026$", "Scale to fit the monitor while preserving the game's aspect ratio."},
    {"$0520-0027$", "Display centered at original resolution without scaling."},
    {"$0520-0028$", "Smoothly interpolate images when scaling."},
    {"$0520-0029$", "Adjust menu bar position in fullscreen mode so it stays on screen."},
    {"$0520-0030$", "Reset settings to their default values."},
    {"$0520-0031$", "Select the function you want to configure."},
    {"$0520-0032$", "Specify which keyboard key to assign."},
    {"$0520-0033$", "Specify which mouse button to assign."},
    {"$0520-0034$", "Specify which gamepad button to assign."},
    {"$0520-0035$", "Enable key repeat when held down (disabled for some keys)."},

    // Section 2.7: BootMenu.exe - $1000-* Launcher Messages (34 IDs)
    {"$1000-0001$", "Execution Error"},
    {"$1000-0002$", "Out of memory or system resources."},
    {"$1000-0003$", "The specified file was not found."},
    {"$1000-0004$", "The specified path was not found."},
    {"$1000-0005$", "The .exe file is invalid (non-Win32 .exe or error in .exe image)."},
    {"$1000-0006$", "The operating system denied access to the specified file."},
    {"$1000-0007$", "The file name association is incomplete or invalid."},
    {"$1000-0008$", "Could not complete the DDE transaction because another DDE transaction is in progress."},
    {"$1000-0009$", "The DDE transaction failed."},
    {"$1000-0010$", "Could not complete the DDE transaction because the request timed out."},
    {"$1000-0011$", "The specified dynamic-link library (DLL) was not found."},
    {"$1000-0012$", "There is no application associated with the given file name extension."},
    {"$1000-0013$", "There is not enough memory to complete the operation."},
    {"$1000-0014$", "A sharing violation occurred."},
    {"$1000-0020$", "Cannot open definition file."},
    {"$1000-0021$", "Boot Error"},
    {"$1000-0022$", "Cannot start because DirectX %d.%d or later is not installed. Launching the DirectX installer."},
    {"$1000-0023$", "Cannot start because DirectX %d.%d%c (%s) or later is not installed. Launching the DirectX installer."},
    {"$1000-0024$", "DirectX Version Check"},
    {"$1000-0025$", "Cannot start because the DirectX %d.%d%c (%s) or later runtime is not installed. Launching the DirectX installer."},
    {"$1000-0026$", "DirectX Runtime Check"},
    {"$1000-0027$", "Serial code has not been entered."},
    {"$1000-0028$", "Input Error"},
    {"$1000-0029$", "Authentication succeeded."},
    {"$1000-0030$", "Authentication Complete"},
    {"$1000-0031$", "Authentication failed. (Error: %d)"},
    {"$1000-0032$", "Authentication Error"},
    {"$1000-0033$", "Are you sure you want to uninstall?"},
    {"$1000-0034$", "Uninstall Confirmation"},
    {"$1000-0035$", "Do you also want to delete save data?"},
    {"$1000-0036$", "Delete Save Data Confirmation"},
    {"$1000-0037$", "Uninstallation complete."},
    {"$1000-0038$", "Complete"},
    {"$1000-0039$", "Uninstallation failed."},
};

constexpr std::array<StringReplacementRule, 30> kStartupKeyCustomizeRules = {{
    {"<name>決定</name>", "<name>Confirm</name>", kExpectedSingleOccurrenceCount},
    {"<name>キャンセル</name>", "<name>Cancel</name>",
     kExpectedSingleOccurrenceCount},
    {"<name>文章を進める</name>", "<name>Advance Text</name>",
     kExpectedSingleOccurrenceCount},
    {"<name>メッセージウィンドウ消去</name>",
     "<name>Hide Message Window</name>", kExpectedSingleOccurrenceCount},
    {"<name>カーソル↑移動</name>", "<name>Move Cursor Up</name>",
     kExpectedSingleOccurrenceCount},
    {"<name>カーソル↓移動</name>", "<name>Move Cursor Down</name>",
     kExpectedSingleOccurrenceCount},
    {"<name>カーソル←移動</name>", "<name>Move Cursor Left</name>",
     kExpectedSingleOccurrenceCount},
    {"<name>カーソル→移動</name>", "<name>Move Cursor Right</name>",
     kExpectedSingleOccurrenceCount},
    {"<name>スクロール↑</name>", "<name>Scroll Up</name>",
     kExpectedSingleOccurrenceCount},
    {"<name>スクロール↓</name>", "<name>Scroll Down</name>",
     kExpectedSingleOccurrenceCount},
    {"<name>１ページ↑</name>", "<name>Page Up</name>",
     kExpectedSingleOccurrenceCount},
    {"<name>１ページ↓</name>", "<name>Page Down</name>",
     kExpectedSingleOccurrenceCount},
    {"<name>強制スキップ</name>", "<name>Force Skip</name>",
     kExpectedSingleOccurrenceCount},
    {"<name>既読スキップ</name>", "<name>Skip Read Text</name>",
     kExpectedSingleOccurrenceCount},
    {"<name>オートモード</name>", "<name>Auto Mode</name>",
     kExpectedSingleOccurrenceCount},
    {"<name>履歴ウィンドウ</name>", "<name>Backlog</name>",
     kExpectedSingleOccurrenceCount},
    {"<name>次の選択肢までスキップ</name>", "<name>Skip to Next Choice</name>",
     kExpectedSingleOccurrenceCount},
    {"<name>前の選択肢に戻る</name>", "<name>Return to Previous Choice</name>",
     kExpectedSingleOccurrenceCount},
    {"<name>音声再生</name>", "<name>Replay Voice</name>",
     kExpectedSingleOccurrenceCount},
    {"<name>セーブ画面</name>", "<name>Save Screen</name>",
     kExpectedSingleOccurrenceCount},
    {"<name>ロード画面</name>", "<name>Load Screen</name>",
     kExpectedSingleOccurrenceCount},
    {"<name>クイックセーブ</name>", "<name>Quick Save</name>",
     kExpectedSingleOccurrenceCount},
    {"<name>クイックロード</name>", "<name>Quick Load</name>",
     kExpectedSingleOccurrenceCount},
    {"<name>環境設定</name>", "<name>Settings</name>",
     kExpectedSingleOccurrenceCount},
    {"<name>オートスピードアップ</name>", "<name>Increase Auto Speed</name>",
     kExpectedSingleOccurrenceCount},
    {"<name>オートスピードダウン</name>", "<name>Decrease Auto Speed</name>",
     kExpectedSingleOccurrenceCount},
    {"<name>スクリーンモード変更</name>", "<name>Toggle Screen Mode</name>",
     kExpectedSingleOccurrenceCount},
    {"<name>ウィンドウ最小化</name>", "<name>Minimize Window</name>",
     kExpectedSingleOccurrenceCount},
    {"<name>ゲーム終了</name>", "<name>Exit Game</name>",
     kExpectedSingleOccurrenceCount},
    {"<name>おあずけモード切り替え</name>", "<name>Toggle Tease Mode</name>",
     kExpectedSingleOccurrenceCount},
}};

constexpr std::array<std::string_view, 4> kProtectedStartupTokens = {{
    "<title>アマカノ2～Perfect Edition～</title>",
    "<brand>あざらしそふと</brand>",
    "<save>%USERPROFILE%\\%brand%\\%title%</save>",
    "<imglib>%USERPROFILE%\\%brand%\\アマカノ2\\imglib</imglib>",
}};

constexpr std::array<StringReplacementRule, 31> kSoundconfReplacementRules = {{
    {"<comment>ＢＧＭ</comment>", "<comment>BGM</comment>",
     kExpectedSingleOccurrenceCount},
    {"<comment>効果音</comment>", "<comment>SE</comment>",
     kExpectedSingleOccurrenceCount},
    {"<comment>キャラクタボイス</comment>",
     "<comment>Character Voice</comment>", kExpectedSingleOccurrenceCount},
    {"<comment>スピーチ</comment>", "<comment>Speech</comment>",
     kExpectedSingleOccurrenceCount},
    {"<comment>システムボイス</comment>", "<comment>System Voice</comment>",
     kExpectedSingleOccurrenceCount},
    {"<comment>システム効果音</comment>", "<comment>System SE</comment>",
     kExpectedSingleOccurrenceCount},
    {"<name>効果音</name>", "<name>SE</name>", kExpectedSingleOccurrenceCount},
    {"<name>音声</name>", "<name>Voice</name>", kExpectedSingleOccurrenceCount},
    {"<name>システムボイス</name>", "<name>System Voice</name>",
     kExpectedSingleOccurrenceCount},
    {"<name>システム効果音</name>", "<name>System SE</name>",
     kExpectedSingleOccurrenceCount},
    // Main heroines appear in both <voice> and <sysvoice> sections
    {"<name>結灯</name>", "<name>Yuuhi</name>", kExpectedDualOccurrenceCount},
    {"<name>玲</name>", "<name>Rei</name>", kExpectedDualOccurrenceCount},
    {"<name>ちとせ</name>", "<name>Chitose</name>",
     kExpectedDualOccurrenceCount},
    // Supporting cast in <voice> section
    {"<name>紗雪</name>", "<name>Sayuki</name>",
     kExpectedSingleOccurrenceCount},
    {"<name>聖</name>", "<name>Mizuki</name>", kExpectedSingleOccurrenceCount},
    {"<name>こはる</name>", "<name>Koharu</name>",
     kExpectedSingleOccurrenceCount},
    {"<name>穂波</name>", "<name>Honami</name>",
     kExpectedSingleOccurrenceCount},
    {"<name>奏</name>", "<name>Kanade</name>", kExpectedSingleOccurrenceCount},
    {"<name>雪静</name>", "<name>Yuzuka</name>",
     kExpectedSingleOccurrenceCount},
    {"<name>涙香</name>", "<name>Ruika</name>", kExpectedSingleOccurrenceCount},
    {"<name>母さん</name>", "<name>Mom</name>", kExpectedSingleOccurrenceCount},
    {"<name>副部長</name>", "<name>Vice President</name>",
     kExpectedSingleOccurrenceCount},
    {"<name>女将さん</name>", "<name>Hostess</name>",
     kExpectedSingleOccurrenceCount},
    {"<name>寶泉路</name>", "<name>Housenji</name>",
     kExpectedSingleOccurrenceCount},
    {"<name>淵上</name>", "<name>Fuchinoe</name>",
     kExpectedSingleOccurrenceCount},
    {"<name>マスター</name>", "<name>Master</name>",
     kExpectedSingleOccurrenceCount},
    {"<name>放送</name>", "<name>Broadcast</name>",
     kExpectedSingleOccurrenceCount},
    {"<name>玲の父</name>", "<name>Rei's Father</name>",
     kExpectedSingleOccurrenceCount},
    {"<name>結灯の父</name>", "<name>Yuuhi's Father</name>",
     kExpectedSingleOccurrenceCount},
    {"<name>迷子の女の子</name>", "<name>Lost Girl</name>",
     kExpectedSingleOccurrenceCount},
    {"<name>プリント機</name>", "<name>Printer</name>",
     kExpectedSingleOccurrenceCount},
}};

constexpr std::array<StringReplacementRule, 3> kConfigBasicFesRules = {{
    {"str[0] str メッセージ速度のサンプルです。",
     "str[0] str Sample text display speed.",
     kExpectedConfigBasicMsgSampleCount},
    {"str[1] str オートモードの速度のサンプルです。",
     "str[1] str Sample Auto Mode speed.",
     kExpectedConfigBasicAutoSampleCount},
    {"font_sample str \\pcテキストサンプル", "font_sample str \\pcSample Text",
     kExpectedSingleOccurrenceCount},
}};

constexpr std::array<StringReplacementRule, 3> kInputTxtFesRules = {{
    {"strvar 11 苗字を入力", "strvar 11 Enter Last Name",
     kExpectedSingleOccurrenceCount},
    {"strvar 11 名前を入力", "strvar 11 Enter First Name",
     kExpectedSingleOccurrenceCount},
    {"messagebox g_TEMP_0 2 確認 $str10 $str20 さんでお間違いないですか？",
     "messagebox g_TEMP_0 2 Confirmation Is $str10 $str20 correct?",
     kExpectedSingleOccurrenceCount},
}};

constexpr std::array<std::string_view, 4> kProtectedInputTxtTokens = {{
    "strvar 10 和倉",
    "strvar 20 賢一",
    "strcmp g_TEMP_0 $str10 和倉",
    "strcmp g_TEMP_0 $str20 賢一",
}};

constexpr std::array<StringReplacementRule, 1> kAdvXmlRules = {{
    {"<selectcomment>[選択肢]</selectcomment>",
     "<selectcomment>[Choice]</selectcomment>", kExpectedSingleOccurrenceCount},
}};

[[nodiscard]] std::string BytesToString(const std::vector<std::uint8_t>& bytes) {
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

[[nodiscard]] std::vector<std::uint8_t> StringToBytes(std::string_view text) {
    const auto* begin = reinterpret_cast<const std::uint8_t*>(text.data());
    return std::vector<std::uint8_t>(begin, begin + text.size());
}

[[nodiscard]] std::size_t ReplaceAllOccurrences(std::string& text,
                                                std::string_view target,
                                                std::string_view replacement) {
    if (target.empty()) {
        throw std::invalid_argument("replacement target must not be empty");
    }

    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = text.find(target, pos)) != std::string::npos) {
        text.replace(pos, target.size(), replacement);
        pos += replacement.size();
        ++count;
    }
    return count;
}

template <std::size_t N>
void ApplyReplacementRules(std::string& text,
                           const std::array<StringReplacementRule, N>& rules,
                           std::string_view artifact_label) {
    for (const auto& rule : rules) {
        const std::size_t replaced =
            ReplaceAllOccurrences(text, rule.target, rule.replacement);
        if (replaced != rule.expected_count) {
            throw std::runtime_error(
                "unexpected replacement count in " + std::string(artifact_label) +
                " for pattern '" + std::string(rule.target) + "': expected " +
                std::to_string(rule.expected_count) + ", got " +
                std::to_string(replaced));
        }
    }
}

template <std::size_t N>
void VerifyProtectedTokensPresent(std::string_view text,
                                  const std::array<std::string_view, N>& tokens,
                                  std::string_view artifact_label) {
    for (const std::string_view token : tokens) {
        if (text.find(token) == std::string_view::npos) {
            throw std::runtime_error("protected invariant token missing in " +
                                     std::string(artifact_label) + ": " +
                                     std::string(token));
        }
    }
}

[[nodiscard]] std::vector<std::uint8_t> BuildLocalizationTableBytes() {
    std::string table;
    table.reserve(std::size(kLocalizationTableEntries) *
                  kApproximateBytesPerLocalizationEntry);

    for (const auto& entry : kLocalizationTableEntries) {
        if (entry.id_tag.empty() || entry.english_text.empty()) {
            throw std::runtime_error("localization table entry must not be empty");
        }
        table.append(entry.id_tag);
        table.append(kCrLf);
        table.append(entry.english_text);
        table.append(kCrLf);
    }

    return StringToBytes(table);
}

[[nodiscard]] std::vector<std::uint8_t> TransformStartupXmlBytes(
    const std::vector<std::uint8_t>& original_bytes) {
    std::string xml = BytesToString(original_bytes);
    VerifyProtectedTokensPresent(xml, kProtectedStartupTokens, kStartupXmlRelPath);
    ApplyReplacementRules(xml, kStartupKeyCustomizeRules, kStartupXmlRelPath);

    constexpr std::string_view kOptTableMarker = "<OPTTABLE>";
    if (xml.find(kOptTableMarker) == std::string::npos) {
        constexpr std::string_view kClosingDocumentTag = "</document>";
        const std::size_t close_pos = xml.rfind(kClosingDocumentTag);
        if (close_pos == std::string::npos) {
            throw std::runtime_error("config\\startup.xml missing </document> tag");
        }
        constexpr std::string_view kLanguageNodeBlock =
            "  <language>\r\n"
            "    <OPTTABLE>lang.zt</OPTTABLE>\r\n"
            "  </language>\r\n";
        xml.insert(close_pos, kLanguageNodeBlock);
    }

    VerifyProtectedTokensPresent(xml, kProtectedStartupTokens, kStartupXmlRelPath);
    return StringToBytes(xml);
}

[[nodiscard]] std::vector<std::uint8_t> TransformSoundconfXmlBytes(
    const std::vector<std::uint8_t>& original_bytes) {
    std::string xml = BytesToString(original_bytes);
    ApplyReplacementRules(xml, kSoundconfReplacementRules, kSoundconfXmlRelPath);
    return StringToBytes(xml);
}

template <std::size_t RuleCount>
[[nodiscard]] std::vector<std::uint8_t> TransformFesArchiveEntryBytes(
    const std::vector<std::uint8_t>& raw_fes_bytes,
    const std::array<StringReplacementRule, RuleCount>& rules,
    std::string_view artifact_label, std::string* out_patched_script = nullptr) {
    const FesPayload unpacked = FesScriptCodec::UnpackFes(raw_fes_bytes);
    std::string script_text = unpacked.AsText();

    ApplyReplacementRules(script_text, rules, artifact_label);

    const std::vector<std::uint8_t> repacked =
        FesScriptCodec::PackFesText(script_text, unpacked.reserved);

    const FesPayload verified = FesScriptCodec::UnpackFes(repacked);
    if (verified.AsText() != script_text || verified.reserved != unpacked.reserved) {
        throw std::runtime_error("FES round-trip verification failed for " +
                                 std::string(artifact_label));
    }

    if (out_patched_script != nullptr) {
        *out_patched_script = std::move(script_text);
    }
    return repacked;
}

[[nodiscard]] std::vector<std::uint8_t> TransformConfigBasicFesBytes(
    const std::vector<std::uint8_t>& raw_fes_bytes) {
    return TransformFesArchiveEntryBytes(raw_fes_bytes, kConfigBasicFesRules,
                                         kConfigBasicFesTargetRelPath);
}

[[nodiscard]] std::vector<std::uint8_t> TransformInputTxtFesBytes(
    const std::vector<std::uint8_t>& raw_fes_bytes) {
    std::string patched_script;
    std::vector<std::uint8_t> repacked = TransformFesArchiveEntryBytes(
        raw_fes_bytes, kInputTxtFesRules, kInputTxtFesTargetRelPath,
        &patched_script);
    VerifyProtectedTokensPresent(patched_script, kProtectedInputTxtTokens,
                                 kInputTxtFesTargetRelPath);
    return repacked;
}

[[nodiscard]] std::vector<std::uint8_t> TransformAdvXmlBytes(
    const std::vector<std::uint8_t>& original_bytes) {
    std::string xml = BytesToString(original_bytes);
    ApplyReplacementRules(xml, kAdvXmlRules, kAdvXmlTargetRelPath);
    return StringToBytes(xml);
}

class UiTextPayloadsRenderer final : public IMenuDomainRenderer {
public:
    std::string DomainName() const override { return "ui_text_payloads"; }

    std::vector<MenuPatchEntrySpec> RenderDomain(
        const GameAssetSource& source, const DomainRenderOptions& options) override {
        (void)options;

        std::vector<MenuPatchEntrySpec> records;
        records.reserve(kExpectedUiRecordCount);

        const ExtractedAssetEntry exe_slice = source.ReadExecutableSlice();
        const std::vector<std::uint8_t> localization_table =
            BuildLocalizationTableBytes();

        records.push_back(GameAssetSource::BuildRawPatchEntry(
            exe_slice, std::string(kLanguageTablePrimaryRelPath),
            localization_table, kFlagLooseOverride));
        records.push_back(GameAssetSource::BuildRawPatchEntry(
            exe_slice, std::string(kLanguageTableExportRelPath),
            localization_table, kFlagLooseOverride));
        records.push_back(GameAssetSource::BuildRawPatchEntry(
            exe_slice, std::string(kLanguageTableBootRelPath),
            localization_table, kFlagLooseOverride));

        const ExtractedAssetEntry startup_source =
            source.ReadStandaloneFile(std::string(kStartupXmlRelPath));
        records.push_back(GameAssetSource::BuildRawPatchEntry(
            startup_source, std::string(kStartupXmlRelPath),
            TransformStartupXmlBytes(startup_source.plain_bytes),
            kFlagBackupStandalone));

        const ExtractedAssetEntry soundconf_source =
            source.ReadStandaloneFile(std::string(kSoundconfXmlRelPath));
        records.push_back(GameAssetSource::BuildRawPatchEntry(
            soundconf_source, std::string(kSoundconfXmlRelPath),
            TransformSoundconfXmlBytes(soundconf_source.plain_bytes),
            kFlagBackupStandalone));

        const ExtractedAssetEntry config_basic_source = source.ReadArchiveEntry(
            std::string(kFesArchiveRelPath), std::string(kConfigBasicFesEntryName));
        records.push_back(GameAssetSource::BuildRawPatchEntry(
            config_basic_source, std::string(kConfigBasicFesTargetRelPath),
            TransformConfigBasicFesBytes(config_basic_source.plain_bytes),
            kFlagLooseOverride));

        const ExtractedAssetEntry inputtxt_source = source.ReadArchiveEntry(
            std::string(kFesArchiveRelPath), std::string(kInputTxtFesEntryName));
        records.push_back(GameAssetSource::BuildRawPatchEntry(
            inputtxt_source, std::string(kInputTxtFesTargetRelPath),
            TransformInputTxtFesBytes(inputtxt_source.plain_bytes),
            kFlagLooseOverride));

        const ExtractedAssetEntry adv_xml_source = source.ReadArchiveEntry(
            std::string(kConfigArchiveRelPath), std::string(kAdvXmlEntryName));
        records.push_back(GameAssetSource::BuildRawPatchEntry(
            adv_xml_source, std::string(kAdvXmlTargetRelPath),
            TransformAdvXmlBytes(adv_xml_source.plain_bytes),
            kFlagLooseOverride));

        return records;
    }
};

}  // namespace

std::unique_ptr<IMenuDomainRenderer> CreateUiTextPayloadsRenderer() {
    return std::make_unique<UiTextPayloadsRenderer>();
}

}  // namespace ama::menu

