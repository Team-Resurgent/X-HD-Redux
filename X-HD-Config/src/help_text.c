// SPDX-License-Identifier: GPL-3.0-only
// What each setting and screen does, why, and when to change it. Kept to what the firmware
// actually does (src/shared/adv7511_xbox.c, user_config.c, application/smbus_i2c.c).
#include "help_text.h"
#include "defines.h"

static const help_item_t kControls[] = {
    { "D-pad Up/Down", "Move between items.",
      "Moves the cursor (>) on the current tab. The left stick does the same. Left/Right only "
      "act where the D-pad is the content: moving the picture, changing a picture control, the "
      "full-screen pattern, the keyboard." },
    { "(A)", "Change or run the selected item.",
      "Flips an on/off setting, runs an action, or opens a list of values: Up/Down to choose, "
      "(A) to pick, (B) to cancel. The current value is marked with *." },
    { "(B) / BACK", "Cancel, or go back out of something.",
      "Closes a list, the keyboard, the full-screen pattern or Tab 3's white balance page, "
      "finishes moving the picture or "
      "changing a control, and cancels a "
      "\"press (A) again\" question. Here in Help it closes the open item." },
    { "WHITE / BLACK", "Previous / next tab.",
      "The tab strip under the title numbers every tab and names the current one, in brackets; "
      "\"Blk\\Wht\" on its right is the reminder. The TV remote's Left/Right do the same." },
    { "(X) / (Y)", "Save to / reload from the board.",
      "Changes apply at once but live only in the board's memory until saved. That's on purpose: "
      "change something, leave and try it in a game, and if it went wrong a console reboot puts "
      "the saved settings back. Until saved, the header shows [UNSAVED CHANGES].\n"
      "On Tabs 1, 2, 3 and 7, (X) saves every setting to the board's flash; on 1, 3 and 7 (Y) "
      "goes back to the last saved settings, and on Tab 2 it re-centres the picture. Other tabs "
      "use them for their own things (shown in the footer)." },
    { "Hold BACK", "Exit to the dashboard (1.5 s).",
      "A short press of BACK keeps its normal meaning. With unsaved changes it asks first: save "
      "and exit, exit without saving (to try them out; a reboot undoes them), or stay." },
    { "Two presses", "Risky actions ask for a second (A).",
      "Writing the EEPROM, factory defaults, firmware updates and reboots say what will happen "
      "and wait for (A) again. (B) cancels; after 10 s the question lapses on its own." },
    { "TV remote", "Drives the app over HDMI-CEC.",
      "Up/Down/OK/Back work like the D-pad, (A) and (B); Left/Right switch tabs. Needs CEC on in "
      "the TV's menu, and on some boards CEC Clock Out (Tab 1, Advanced). Which keys arrive depends on "
      "the TV (see Tab 4)." },
    { "Messages", "The line above the keys: what just happened.",
      "Plain messages clear after 3 s. Errors and the result of an action (save, reload, EEPROM) "
      "stay in amber until the next button press. With nothing to report the line is empty: the "
      "header already shows the firmware or board state, and [UNSAVED CHANGES]." },
    { 0 }
};

static const help_item_t kTab1[] = {
    { "Colour Space", "How black and white levels are sent to the TV.",
      "Video can use two ranges: PC-style Full (black 0, white 255) or video-style Limited "
      "(black 16, white 235). The TV must expect the same one. If it expects Full and gets "
      "Limited, blacks look grey and washed out; the other way round, dark and bright detail is "
      "crushed.\n"
      "Auto: SD modes on an HDMI TV go out as RGB Limited; HD as YCbCr 4:4:4 when the TV "
      "supports it, otherwise RGB Full. If the console's encoder is already sending RGB, it is "
      "passed straight through. Auto (SD RGB Full) is the same except SD goes out as RGB Full, "
      "for TVs that ignore the range flag and show grey blacks in SD.\n"
      "The other choices force one format for every mode. Judge it on your TV with Tab 3's "
      "test pattern.\n"
      "YCbCr Adjustable sends YCbCr 4:4:4 like YCbCr Passthrough, but through the board's "
      "colour conversion, so Tab 3's picture controls work on it -- in HD too, where Auto uses "
      "the passthrough. At default picture settings it looks the same as the passthrough. A TV "
      "that can't take YCbCr 4:4:4 (or a DVI screen) gets RGB Full instead; Tab 5 shows what "
      "actually goes out. It needs recent firmware: older boards keep their setting." },
    { "Aspect Ratio", "Widescreen or 4:3 for SD modes.",
      "Only SD modes (480i, 480p, 576i) are affected; HD modes are always 16:9. Auto follows the "
      "console's own widescreen setting. Force 16:9 if SD games look squashed on a widescreen "
      "TV, or 4:3 if they look stretched.\n"
      "This sets the shape the TV is told about; a TV with its own zoom/size setting may "
      "override it." },
    { "Scan Mode", "Whether the TV crops the picture edges.",
      "TV (overscan) asks the TV to zoom in slightly and hide the edges, the classic TV look. PC "
      "(underscan) asks it to show every pixel 1:1. Auto asks for underscan when the TV says it "
      "supports it, and otherwise leaves it to the TV.\n"
      "Separately, every mode is marked as game content, which lets most TVs switch to Game "
      "Mode (less lag) whatever this is set to. Many TVs follow their own picture size setting "
      "(\"Just Scan\", \"Screen Fit\", \"PC\") instead of this request." },
    { "SD 640 Modes", "640-wide SD modes: fill the screen, or standard.",
      "Some games run 640 pixels wide in 480p or 576p. Fill (default) sends just those 640 "
      "pixels, so the picture reaches the edges of the screen. Exact sends the modes that "
      "match a standard TV format as that format (720 wide, its own timing and format code), "
      "with the console's black borders as thin bars either side. Try Exact if a TV handles the standard 480p/576p better. Which modes "
      "qualify depends on the console's video encoder; the rest stay Fill." },
    { "One Touch Play", "Wake the TV and switch to the Xbox at power-on.",
      "When the console starts, the board asks the TV to turn on and select this input. CEC must "
      "be enabled in the TV's menu (Anynet+, Bravia Sync, SimpLink, ...)." },
    { "TV at Power-Off", "What the TV does when the console turns off.",
      "Restore (default) puts the TV back as the console's power-on found it: switched back "
      "to the source it was showing, or to standby if it was off. Always Standby turns the TV "
      "off regardless; Disabled leaves it alone. Whichever is set, nothing is sent when the "
      "TV itself started the shutdown or the console was showing another input." },
    { "TV Label (SPD)", "The name some TVs show for this input.",
      "Sent with the video as an 8-character maker and 16-character product name. Some TVs show "
      "it in the input list or the info banner; many ignore it. Pick a preset or \"Edit with the "
      "keyboard\"; (X) saves it." },
    { "Advanced Settings...", "Fixes for one TV or board, and test switches.",
      "(A) opens a page of the settings few people need: Wire-VIC Mode, Clock Phase, CEC Clock "
      "Out, Standalone Freeze and Audio Output. They change and save the same way; (B) comes "
      "back." },
    { "Wire-VIC Mode", "Which format code the TV is given.",
      "With each mode the TV is sent a CEA format number (VIC) saying what it is, e.g. 4 for "
      "720p.\n"
      "0 Strict (recommended) only declares a standard format when the mode's size, scan and "
      "pixel clock all match it, otherwise \"no standard format\" (VIC 0) -- so a given mode "
      "always looks the same to the TV.\n"
      "1 Chip Auto lets the HDMI chip choose from its own detector, which can carry a reading "
      "over from the previous mode, so the same mode may be labelled differently. Only try it "
      "if a TV refuses a mode in Strict." },
    { "Clock Phase", "Fine timing trim: fixes sparkling pixels.",
      "Moves the moment the HDMI chip samples the console's pixels, in 0.4 ns steps from "
      "-1.2 ns (0) to +1.6 ns (7). A wrong value shows as sparkling or shimmering fine detail "
      "and noise on sharp edges, worst in 720p and 1080i.\n"
      "The board keeps one value per console video encoder (defaults: Xcalibur 0, Conexant 3, "
      "Focus 2). Try one step either way on a detailed screen, and keep the cleanest." },
    { "CEC Clock Out", "Clock the HDMI chip's CEC from the board.",
      "HDMI-CEC needs a 12 MHz clock at the HDMI chip. Some boards have their own; others are "
      "fed by the controller (pin 29) through a wire to the ADV7511's pin 50. Without that wire "
      "this does nothing, so leaving it on is harmless. Off on a board that relies on it, CEC "
      "stops working." },
    { "Standalone Freeze", "Ignore the console's mode messages. Leave off.",
      "The board normally learns each new video mode from a message the console sends. Freeze "
      "makes it ignore them and work from what it measures alone, as with a dashboard that "
      "sends none -- a bench test. It is saved like any other setting, so if it's left on the "
      "board keeps ignoring the console after every boot." },
    { "Audio Output", "Mute HDMI audio, for testing.",
      "Shows whether sound is coming over HDMI or another cable. It is never saved: audio is "
      "back on after a restart." },
    { 0 }
};

static const help_item_t kTab2[] = {
    { "Video Mode", "Switch this app between your modes to align each.",
      "Lists the modes your dashboard settings allow (720p, 1080i, 480p, 480i -- 576i on a PAL "
      "console without PAL-60). The picture position is stored per mode, so align every mode "
      "you use. Leaving Tabs 2 and 3 returns to the mode the app started in." },
    { "Adjust Position", "Move the picture to fit the screen.",
      "(A) starts. The D-pad moves the picture 1 step, the triggers 10 sideways; (Y) centres, "
      "(X) saves, (A) or (B) finishes. Move it until the reticle's outer box just touches all "
      "four screen edges; the inner box is the 5% safe area games keep text inside.\n"
      "The top line shows how far the picture is moved right and down (up to 127 either way)." },
    { "Reset Position", "Centre this mode's picture again.",
      "Puts the current mode's position back to 0, 0. Like any change, (X) saves it." },
    { "Too far", "Lost the picture? (Y) re-centres.",
      "Moving the picture well away from centre can make some TVs distort it, show it "
      "partly, or drop it altogether. (Y) re-centres the current mode at once, from the list or "
      "while moving, so it works even with nothing on screen. Nothing is lost until (X) saves." },
    { "Profile", "The timing profile the position belongs to.",
      "\"Profile 0x..\" is the console mode the board is using, which picks the stored "
      "position. 0xFF means no mode message from the console (standalone)." },
    { 0 }
};

static const help_item_t kTabPicture[] = {
    { "Changing a control", "(A), then Left/Right.",
      "(A) on a control starts changing it: Left/Right by 1, the triggers by 10, Up/Down move to "
      "the next control, and (A) or (B) finishes. Every change is live; (X) saves them with the "
      "other settings, (Y) goes back to the saved ones.\n"
      "The controls work in the board's colour conversion, so they need firmware that has them "
      "(update on Tab 8) and a colour space other than the passthroughs." },
    { "Video Mode", "The mode this app runs in; shared with Tab 2.",
      "Switching mode here also shows which picture profile the board uses for it (see "
      "Profile). A TV may treat modes differently: check the ones you use. Leaving Tabs 2 and 3 "
      "returns to the mode the app started in." },
    { "Colour Space", "The same setting as Tab 1, judged on the pattern.",
      "(A) opens the list. The line above the controls shows what the board is sending now. "
      "YCbCr and RGB Passthrough send the console's levels untouched, so the picture controls "
      "don't apply to them; every other choice does. For YCbCr with the controls working, pick "
      "YCbCr Adjustable (see Tab 1's Colour Space)." },
    { "Profile", "SD, HD and Non-standard each keep their own settings.",
      "The board holds three complete sets of picture settings and picks one from the signal "
      "by itself: HD for 720p and 1080i; SD for 480p, 576p and 480i on the standard 27 MHz "
      "clock; Non-standard for SD sizes on any other clock (several encoder modes, e.g. the "
      "Focus 640x480 ones). A TV often handles these differently, so each can be tuned on its "
      "own.\n"
      "The controls edit the profile in use, shown as \"(in use)\". (A) opens a list to edit "
      "another one instead; the line then turns amber and names the one in use, and changes "
      "to the edited profile only show once the console sends that kind of signal. Switch Video "
      "Mode to see it. The choice follows the board again once you pick the profile in use." },
    { "White Clip", "The brightest level sent (default 250).",
      "No output level goes above this code (128-255). Some TVs lose the picture or show \"not "
      "supported\" when the signal reaches full white in some modes; a cap a little under 255 "
      "avoids it, which is why the default is 250. Lower it only if that happens to you: whites "
      "lose a little punch, and the near-white steps above the cap all look the same.\n"
      "In Limited range white is 235, so the cap only trims overshoots above it." },
    { "Black Clip", "The darkest level sent (default 0).",
      "No output level goes below this code (0-64), which lifts black towards grey. Rarely "
      "needed: only for a TV that misbehaves on full black. In Limited range black is already "
      "16, so values up to 16 change nothing." },
    { "Contrast", "Gain on the whites, black staying put (default 100 %).",
      "Scales everything above black from 50 to 150 %. Up, whites get brighter until the "
      "near-white steps merge; down, the picture goes flat and dim." },
    { "Brightness", "Lift or lower every level (default 0).",
      "Adds -64 to +64 to every output level, black and white alike (in YCbCr Adjustable only "
      "brightness moves, not the colour, as on a TV). On the pattern, step 0 "
      "should be black with 2-4 just visible. If it takes a lot, the colour space probably "
      "doesn't match the TV: try that first." },
    { "Saturation", "Colour strength (default 100 %).",
      "0 % is black and white, 200 % double strength. Judge it on the colour bars and, better, "
      "on a game with skin tones: too high and reds glow and smear." },
    { "Hue", "Turn every colour around the wheel (default 0).",
      "-30 to +30 degrees. Plus turns red toward yellow (and green toward cyan), minus turns "
      "red toward magenta. Greys stay grey. Use it when faces look too red or too green; judge "
      "it on skin tones in a game rather than on the bars." },
    { "Test pattern", "Near-black, near-white and colour bars.",
      "Grey steps near black (0-32) and near white (210-255), large near-black bars and 75% "
      "colour bars, on a black background.\n"
      "Right: 0 is deep black, 2-4 just visible, the whites distinct up to the white clip. Steps "
      "up to ~16 all black and the whites merged: the TV expects Limited. Black looks grey and "
      "every step shows: the TV expects Full." },
    { "Full-Screen Pattern", "The pattern on the whole screen.",
      "Easier to judge from across the room. Up/Down picks the control (colour space, the six "
      "picture controls, then the six white balance ones), Left/Right change it by 1 and the "
      "triggers by 10, (X) saves, (B) comes back. The top line names the profile being "
      "edited." },
    { "Reset to Defaults", "This profile back to neutral.",
      "Puts the edited profile back to white clip 250, black clip 0, contrast 100 %, "
      "brightness 0, saturation 100 %, hue 0 and neutral white balance, live. The other two "
      "profiles are left alone: Copy Profile to All after a reset puts every profile back. (X) "
      "saves; Restore Defaults on Tab 7 resets all three along with every other setting." },
    { "White Balance", "Red, green and blue gain and offset.",
      "Opens a page of its own for the edited profile; (B) or Back to Picture returns.\n"
      "Gain (50-150 %, default 100) scales one colour from black up, like a TV service menu's "
      "\"drive\": it fixes a tint in the whites and bright greys. Offset (-32..+32, default 0) "
      "adds to one colour at every level, like \"cutoff\": it fixes a tint in black and dark "
      "greys. Set the gains on the near-white steps, then the offsets on the near-black ones; "
      "lowering the colour that shows is usually better than raising the others.\n"
      "Reset White Balance puts only these six back; the other controls stay.\n"
      "In YCbCr Adjustable the same three rows act on Cr, Y and Cb instead, and the page names "
      "them so: Y gain works like contrast, Cr and Cb gain are saturation along one colour axis, "
      "and the Cr and Cb offsets shift every level toward red/cyan or blue/yellow. A profile is "
      "one set of numbers in any colour space, so values tuned in RGB carry over as Cr/Y/Cb "
      "(the \"red\" gain becomes Cr gain): tune in the colour space you use." },
    { "Copy Profile to All", "Give every signal the same picture.",
      "Copies all settings of the edited profile, white balance included, over the other two. "
      "Asks for a second (A); (B) cancels. Handy when one set of settings suits the TV in "
      "every mode: tune one profile, then copy it." },
    { 0 }
};

static const help_item_t kTabCec[] = {
    { "Remote test", "See which TV remote keys reach the console.",
      "(A), or OK on the remote, starts the test: each key lights up on the drawn remote and the "
      "frame is logged, without moving around the app. (B), or holding the remote's Back for 1 "
      "s, ends it; (Y) clears the log." },
    { "The keys", "Only keys a TV can pass on are drawn.",
      "Every key on the drawn remote has a CEC code a TV may send to a player: arrows, OK, "
      "Back, the menu keys, Info, play/pause/stop, rewind/fast forward, skip, channel, the "
      "colour keys and digits. Power, volume, mute and input have codes too, but TVs keep those "
      "for themselves, so they aren't drawn; a TV going to standby shows as <STANDBY> in the "
      "log. Many TVs send the transport keys as <Play> or <Deck Control>; those light the same "
      "buttons." },
    { "Compatibility", "Which keys arrive varies from TV to TV.",
      "CEC leaves it to each TV which remote keys it passes on, and makers differ (Anynet+, "
      "Bravia Sync, SimpLink, ...). Most pass the arrows, OK and Back; many the play keys and "
      "colour keys; fewer the digits, Info or channel keys. Some only pass keys once the TV has "
      "switched to the Xbox's input, or after their own \"device control\" setting is on." },
    { "Outside a test", "The TV remote drives this app.",
      "Arrows, OK and Back work like the D-pad, (A) and (B); Left/Right switch tabs." },
    { "CEC bus", "Whether the TV answers the board.",
      "Sending: idle or busy. Last frame: acknowledged means the TV answered; not acknowledged "
      "(retries timed out) usually means CEC is off on the TV, or there is no CEC clock (Tab 1, Advanced). "
      "Arbitration lost: another device talked at the same moment, harmless if rare. From TV: "
      "frames received since the app started; the newest are listed below it." },
    { "No keys at all", "Check CEC on the TV and on Tab 1.",
      "CEC must be on in the TV's menu, and on boards that need it, CEC Clock Out (Tab 1, Advanced). The "
      "CEC bus lines show whether the TV answers at all." },
    { 0 }
};

static const help_item_t kTabStatus[] = {
    { "System", "What's running: app, firmware, board.",
      "This app's version and build, the board's firmware version and build, its HDMI chip "
      "and its unique ID -- include these when reporting a problem." },
    { "Sending", "What goes to the TV now.",
      "The resolution and refresh rate being output, the colour format actually sent (what "
      "Auto chose), and the format code (VIC) declared to the TV. Refreshed every second." },
    { "Input", "The video clock measured from the console.",
      "Roughly 74.2 MHz in 720p and 1080i, 27 MHz in 480p. PLL locked means the HDMI chip "
      "follows it; acquiring that never settles points to an encoder or wiring problem. Then "
      "the console's video encoder: Conexant, Focus or Xcalibur, depending on the revision -- "
      "timings and Clock Phase are kept per encoder." },
    { "Mode", "The mode the board chose, and from what.",
      "\"BIOS Mode Packet\" is the normal case: the console said which mode it is in. The other "
      "sources (measured geometry, the HDMI chip's detector, fallbacks) are what the board uses "
      "when that message is missing or late. The detector's own reading is for comparison: 0 "
      "means \"not a standard CEA format\", which is normal for several Xbox modes." },
    { "Connection", "Is a TV connected and awake, and is the board sending.",
      "Hot Plug: a display is connected and reports itself. Monitor Sense: it is powered and "
      "receiving. DISCONNECTED with a TV attached points to the cable or the port. Video out is "
      "off briefly while a mode changes; Audio out says whether audio goes to the TV." },
    { "Counters", "Rising counts point to cable or timing trouble.",
      "Uptime, applied mode changes, clock losses, TMDS gate cycles (output switched off and "
      "on) and detector blind events. Counts that grow while nothing is changing mean an "
      "unstable connection or timing." },
    { 0 }
};

static const help_item_t kTab6[] = {
    { "Summary", "What the TV says it can do.",
      "Name, maker, native resolution, HDMI or DVI, colour formats, audio formats and the "
      "standard modes it lists. Tab 7's EEPROM recommendation is built from this." },
    { "(X) Hex view", "The raw EDID bytes.",
      "Two 128-byte blocks: (A) or the triggers switch between them. Useful when reporting a "
      "problem with a particular TV." },
    { "(Y) Refresh", "Read the EDID again.",
      "After changing the TV, the input, or an AV receiver in between." },
    { 0 }
};

static const help_item_t kTab7[] = {
    { "Apply EEPROM", "Set the console's video/audio options from the TV.",
      "Writes the console's settings to match the TV: 480p and 720p on; 1080i when the TV lists "
      "it (or has a 1080-line panel); widescreen unless the TV's native shape is 4:3; PAL-60 on, "
      "letterbox off. Audio: Surround for 6+ channels, else Stereo; Dolby Digital and DTS only "
      "if the TV lists them. A TV without audio leaves audio alone.\n"
      "A PAL console is switched to NTSC-M, which the dashboard needs to offer HD modes. The "
      "screen compares the current and target values first. Asks for a second (A); then a "
      "third (A) reboots the console so the change takes effect." },
    { "Save", "Store the current settings on the board.",
      "Writes every setting (colour, positions, CEC, label, ...) to the board's flash. They load "
      "at every boot." },
    { "Reload", "Go back to the last saved settings.",
      "Undoes changes since the last save without a reboot." },
    { "Restore Defaults", "Factory settings, saved.",
      "Every setting back to its default, and saved at once. Asks for a second (A)." },
    { "Exit", "Back to the dashboard.",
      "With unsaved changes it asks first: save and exit, exit without saving, or stay." },
    { 0 }
};

static const help_item_t kTab8[] = {
    { "Image", "The firmware file to use.",
      "The .bin files in this app's folder, firmware.bin first. (A) picks one; each is checked "
      "(layout, size, CRC) and its build shown. It is then compared with the board: an update "
      "available (the cursor moves to Update), already installed, older (a downgrade), or just "
      "different when either build has no date. It also warns when saved settings would reset." },
    { "Update", "Write only the parts that changed.",
      "Two (A) presses. The bootloader is written first if it differs, then the application "
      "pages that differ, each read back. Settings are kept unless the warning above says "
      "otherwise. Keep the console on, and don't switch the TV to another input while it runs." },
    { "Interrupted", "Finish an update that was cut off.",
      "The board waits in update mode. Start this app again: it opens on this tab and Finish "
      "the update writes only what is missing. A new firmware that doesn't start leaves the "
      "board here too; writing it again is then the only choice offered. While waiting, the "
      "board shows a basic picture in the console's own mode." },
    { "Bootloader", "Written first, by the running board.",
      "It holds the board's start-up code and its video mode tables. If writing it fails, keep "
      "the console on and Retry: powered off before it's written, the board needs an ST-Link to "
      "recover." },
    { "HD+ Conversion", "Turn a stock HD+ into an X-HD.",
      "About 6 minutes, two (A) presses. No backup of the HD+ firmware is made; if it fails, "
      "recovery needs an ST-Link." },
    { "Backup", "Save the board's firmware to a file.",
      "Writes backup_<build>.bin next to this app, every page checked. The file can be flashed "
      "back like any image (saved settings are not in it)." },
    { "Reboot", "Power-cycle the console.",
      "A full power cycle, so the board sees the console start. Needed after an update." },
    { 0 }
};

static const help_item_t kTabHelp[] = {
    { "Help", "This list.",
      "(A) opens or closes an item, (B) closes, (X) opens or closes every section, the triggers "
      "scroll. What's running (app, firmware, board) is on Tab 5." },
    { "Version", "X-HD Redux " APP_VERSION ".",
      "This app and the firmware it ships with are both version " APP_VERSION ". Tab 5 shows "
      "the firmware actually on the board; Tab 8 updates it." },
    { 0 }
};

const help_section_t kHelpSections[] = {
    { "CONTROLS", kControls },
    { "TAB 1: SETTINGS (VIDEO & HDMI-CEC)", kTab1 },
    { "TAB 2: PICTURE ALIGNMENT", kTab2 },
    { "TAB 3: PICTURE CONTROLS", kTabPicture },
    { "TAB 4: CEC (TV REMOTE & BUS)", kTabCec },
    { "TAB 5: STATUS", kTabStatus },
    { "TAB 6: EDID (TV CAPABILITIES)", kTab6 },
    { "TAB 7: EEPROM & FLASH", kTab7 },
    { "TAB 8: FIRMWARE UPDATE", kTab8 },
    { "TAB 9: HELP", kTabHelp },
};

const int kHelpSectionCount = (int)(sizeof(kHelpSections) / sizeof(kHelpSections[0]));
