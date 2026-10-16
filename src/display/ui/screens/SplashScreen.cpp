#include "SplashScreen.h"

#include "pico/stdlib.h"
#include "drivermanager.h"
#include "profile_pics.h"

extern volatile int g_profilePic;
extern volatile char g_lukeDbg[24];

void SplashScreen::init() {
    getRenderer()->clearScreen();
    splashStartTime = getMillis();
    configMode = DriverManager::getInstance().isConfigMode();
}

void SplashScreen::shutdown() {
    clearElements();
}

void SplashScreen::drawScreen() {
	if (getDisplayOptions().splashMode == static_cast<SplashMode>(SPLASH_MODE_NONE)) {
		getRenderer()->drawText(0, 4, " Splash NOT enabled.");
    } else {
            // CUSTOM: show the current macro profile's picture (Viper = stored splash)
            const uint8_t* pic = nullptr;
            if (!configMode) {
                if (g_profilePic == 0) pic = PIC_SAGAT;
                else if (g_profilePic == 1) pic = PIC_ARJIN;
                else if (g_profilePic == 2) pic = PIC_MANON;
                else if (g_profilePic == 4) pic = PIC_LUKE;
                else if (g_profilePic == 5) pic = PIC_LILY;
            }
            getRenderer()->clearScreen();
            if (pic) getRenderer()->drawSprite((uint8_t*) pic, 128, 64, 16, 0, 0, 1);
            else getRenderer()->drawSprite((uint8_t*) getDisplayOptions().splashImage.bytes, 128, 64, 16, 0, 0, 1);
            if (g_profilePic == 4) {                    // DEBUG: Luke motion info on the bottom row
                getRenderer()->drawRectangle(0, 56, 127, 63, 0, 1);
                getRenderer()->drawText(0, 7, std::string((const char*)g_lukeDbg));
            }

	}
}

int8_t SplashScreen::update() {
    uint32_t elapsedDuration = getMillis() - splashStartTime;
    uint32_t splashDuration = getDisplayOptions().splashDuration;
    if (!configMode) {
        // still running
        if (splashDuration != 0 && (elapsedDuration >= splashDuration)) {
            return DisplayMode::BUTTONS;
        }
    } else {
        uint16_t buttonState = getGamepad()->state.buttons;
        if (prevButtonState && !buttonState) {
            if (prevButtonState == GAMEPAD_MASK_B2) {
                prevButtonState = 0;
                return DisplayMode::CONFIG_INSTRUCTION;
            }
        }
        prevButtonState = buttonState;
    }
    return -1; // -1 means no change in screen state
}
