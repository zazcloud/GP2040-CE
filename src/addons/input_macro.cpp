#include "addons/input_macro.h"
#include "storagemanager.h"
#include "GamepadState.h"

#include "hardware/gpio.h"

// ============================================================================
//  CUSTOM: Gear system + smart start + randomized timings
//
//  - Macros whose label starts with "G " (e.g. "G DP right") use the gear system:
//      * Hold an attack button, then press the macro button, keep both held 2s
//        -> that attack becomes the macro's gear (no move, attack not sent).
//      * Macro alone -> plays the motion; the attack in the attack step is
//        replaced by the selected gear (default = what you recorded).
//      * Smart start: if the first direction of the macro is held now, or was
//        released within SKIP_WINDOW_US, the first step is skipped (no dash).
//  - ALL macros get randomized step durations:
//      * direction-only step ........ 1.5 - 3 frames
//      * step with an attack ........ 3 - 7 frames
//      * OD macro (2+ attacks in one step): attack step 2 - 5 frames,
//        direction steps after the attack (block) 4 - 8 frames
//      * empty steps keep their recorded duration
//  Gear selections reset to the recorded default when the device is unplugged.
// ============================================================================

namespace {
    // ---- Tunables (change these numbers if needed) ----
    constexpr uint32_t FRAME_US        = 16667;           // 1 frame at 60fps
    constexpr uint64_t GEAR_HOLD_US    = 2000000;         // 2 seconds
    constexpr uint64_t SKIP_WINDOW_US  = 9 * FRAME_US;    // smart-start window

    constexpr uint32_t DIR_MIN_US      = FRAME_US * 3 / 2; // 1.5f
    constexpr uint32_t DIR_MAX_US      = FRAME_US * 3;     // 3f
    constexpr uint32_t ATK_MIN_US      = FRAME_US * 3;     // 3f
    constexpr uint32_t ATK_MAX_US      = FRAME_US * 7;     // 7f
    constexpr uint32_t OD_ATK_MIN_US   = FRAME_US * 2;     // 2f
    constexpr uint32_t OD_ATK_MAX_US   = FRAME_US * 5;     // 5f
    constexpr uint32_t BLOCK_MIN_US    = FRAME_US * 4;     // 4f
    constexpr uint32_t BLOCK_MAX_US    = FRAME_US * 8;     // 8f

    // LP=L3, MP=B4(Y), HP=R1, LK=B1(A), MK=B2(B), HK=R2
    constexpr uint32_t ATTACK_MASK = GAMEPAD_MASK_L3 | GAMEPAD_MASK_B4 | GAMEPAD_MASK_R1 |
                                     GAMEPAD_MASK_B1 | GAMEPAD_MASK_B2 | GAMEPAD_MASK_R2;
    constexpr uint32_t DIR_MASK    = GAMEPAD_MASK_DU | GAMEPAD_MASK_DD |
                                     GAMEPAD_MASK_DL | GAMEPAD_MASK_DR;

    constexpr int MAX_STEPS = 64;

    // ---- State ----
    uint32_t rngState = 0x12345678;
    uint32_t stepDuration[MAX_STEPS];
    uint32_t gearMask[MAX_MACRO_LIMIT] = {0};   // 0 = use recorded attack

    // last time each physical direction was held: U, D, L, R
    uint64_t dirLastHeld[4] = {0, 0, 0, 0};

    int      gearPendingMacro = -1;
    uint32_t gearPendingMask  = 0;
    uint64_t gearPendingStart = 0;
    bool     gearArmed        = false;

    uint32_t nextRandom() {
        uint32_t x = rngState;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        rngState = (x == 0) ? 1 : x;
        return rngState;
    }

    uint32_t randomRange(uint32_t lo, uint32_t hi) {
        return lo + (nextRandom() % (hi - lo + 1));
    }

    bool isGearMacro(const Macro& macro) {
        return macro.macroLabel[0] == 'G' && macro.macroLabel[1] == ' ';
    }

    int stepCount(const Macro& macro) {
        int n = (int)macro.macroInputs_count;
        return n > MAX_STEPS ? MAX_STEPS : n;
    }

    // Randomize the duration of every step for this run of the macro
    void prepareSteps(const Macro& macro, uint64_t now) {
        rngState ^= (uint32_t)now;
        if (rngState == 0) rngState = 1;

        int n = stepCount(macro);
        bool isOD = false;
        int firstAttack = -1;
        for (int i = 0; i < n; i++) {
            uint32_t attacks = macro.macroInputs[i].buttonMask & ATTACK_MASK;
            if (attacks) {
                if (firstAttack < 0) firstAttack = i;
                if (__builtin_popcount(attacks) >= 2) isOD = true;
            }
        }

        for (int i = 0; i < n; i++) {
            uint32_t mask = macro.macroInputs[i].buttonMask;
            if (mask & ATTACK_MASK) {
                stepDuration[i] = isOD ? randomRange(OD_ATK_MIN_US, OD_ATK_MAX_US)
                                       : randomRange(ATK_MIN_US, ATK_MAX_US);
            } else if (mask & DIR_MASK) {
                if (isOD && firstAttack >= 0 && i > firstAttack) {
                    stepDuration[i] = randomRange(BLOCK_MIN_US, BLOCK_MAX_US);
                } else {
                    stepDuration[i] = randomRange(DIR_MIN_US, DIR_MAX_US);
                }
            } else {
                stepDuration[i] = macro.macroInputs[i].duration; // empty step: keep
            }
        }
    }

    uint32_t stepHoldTime(const Macro& macro, int pos) {
        uint32_t hold = stepDuration[pos] + macro.macroInputs[pos].waitDuration;
        return hold == 0 ? INPUT_HOLD_US : hold;
    }

    // Smart start: skip the first direction if it is held / was just released
    int startPosition(const Macro& macro, uint64_t now) {
        if (!isGearMacro(macro) || stepCount(macro) < 2) return 0;
        uint32_t first = macro.macroInputs[0].buttonMask;
        if ((first & ATTACK_MASK) || !(first & DIR_MASK)) return 0;

        const uint32_t bits[4] = {GAMEPAD_MASK_DU, GAMEPAD_MASK_DD,
                                  GAMEPAD_MASK_DL, GAMEPAD_MASK_DR};
        for (int d = 0; d < 4; d++) {
            if (first & bits[d]) {
                if (dirLastHeld[d] == 0 || (now - dirLastHeld[d]) > SKIP_WINDOW_US) {
                    return 0;
                }
            }
        }
        return 1;
    }

    void trackDirections(uint8_t dpad, uint64_t now) {
        if (dpad & GAMEPAD_MASK_UP)    dirLastHeld[0] = now;
        if (dpad & GAMEPAD_MASK_DOWN)  dirLastHeld[1] = now;
        if (dpad & GAMEPAD_MASK_LEFT)  dirLastHeld[2] = now;
        if (dpad & GAMEPAD_MASK_RIGHT) dirLastHeld[3] = now;
    }
}

bool InputMacro::available() {
    // Macro Button initialized by void Gamepad::setup()
    GpioMappingInfo* pinMappings = Storage::getInstance().getProfilePinMappings();
    for (Pin_t pin = 0; pin < (Pin_t)NUM_BANK0_GPIOS; pin++)
    {
        switch( pinMappings[pin].action ) {
            case GpioAction::BUTTON_PRESS_MACRO:
            case GpioAction::BUTTON_PRESS_MACRO_1:
            case GpioAction::BUTTON_PRESS_MACRO_2:
            case GpioAction::BUTTON_PRESS_MACRO_3:
            case GpioAction::BUTTON_PRESS_MACRO_4:
            case GpioAction::BUTTON_PRESS_MACRO_5:
            case GpioAction::BUTTON_PRESS_MACRO_6:
                return true;
            default:
                break;
        }
    }
    return false;
}

void InputMacro::setup() {
    GpioMappingInfo* pinMappings = Storage::getInstance().getProfilePinMappings();
    macroButtonMask = 0;
    memset(macroPinMasks, 0, sizeof(macroPinMasks));
    for (Pin_t pin = 0; pin < (Pin_t)NUM_BANK0_GPIOS; pin++)
    {
        switch( pinMappings[pin].action ) {
            case GpioAction::BUTTON_PRESS_MACRO:
                macroButtonMask = 1 << pin;
                break;
            case GpioAction::BUTTON_PRESS_MACRO_1:
                macroPinMasks[0] = 1 << pin;
                break;
            case GpioAction::BUTTON_PRESS_MACRO_2:
                macroPinMasks[1] = 1 << pin;
                break;
            case GpioAction::BUTTON_PRESS_MACRO_3:
                macroPinMasks[2] = 1 << pin;
                break;
            case GpioAction::BUTTON_PRESS_MACRO_4:
                macroPinMasks[3] = 1 << pin;
                break;
            case GpioAction::BUTTON_PRESS_MACRO_5:
                macroPinMasks[4] = 1 << pin;
                break;
            case GpioAction::BUTTON_PRESS_MACRO_6:
                macroPinMasks[5] = 1 << pin;
                break;
            default:
                break;
        }
    }

    inputMacroOptions = &Storage::getInstance().getAddonOptions().macroOptions;
    if (inputMacroOptions->macroBoardLedEnabled && isValidPin(BOARD_LED_PIN)) {
        gpio_init(BOARD_LED_PIN);
        gpio_set_dir(BOARD_LED_PIN, GPIO_OUT);
        boardLedEnabled = true;
    } else {
        boardLedEnabled = false;
    }
    boardLedEnabled = false;
    prevMacroInputPressed = false;

    // CUSTOM: seed random generator and clear gear state
    rngState = (uint32_t)getMicro() ^ 0x9E3779B9u;
    if (rngState == 0) rngState = 1;
    for (int i = 0; i < MAX_MACRO_LIMIT; i++) gearMask[i] = 0;
    gearPendingMacro = -1;

    reset();
}


void InputMacro::reset() {
    macroPosition = -1;
    pressedMacro = -1;
    isMacroRunning = false;
    macroStartTime = 0;
    macroInputPosition = 0;
    isMacroTriggerHeld = false;
    macroInputHoldTime = INPUT_HOLD_US;
    if (boardLedEnabled) {
        gpio_put(BOARD_LED_PIN, 0);
    }
}

void InputMacro::restart(Macro& macro) {
    // CUSTOM: new random timings + smart start on every repeat
    macroStartTime = currentMicros;
    prepareSteps(macro, currentMicros);
    macroInputPosition = startPosition(macro, currentMicros);
    macroInputHoldTime = stepHoldTime(macro, macroInputPosition);
}

void InputMacro::checkMacroPress() {
    Gamepad * gamepad = Storage::getInstance().GetGamepad();
    Mask_t allPins = gamepad->debouncedGpio;

    // Go through our macro list
    pressedMacro = -1;
    for(int i = 0; i < MAX_MACRO_LIMIT; i++) {
        if ( inputMacroOptions->macroList[i].enabled == false ) // Skip disabled macros
            continue;
        Macro * macro = &inputMacroOptions->macroList[i];
        if ( macro->useMacroTriggerButton ) {
            // Use Gamepad Button for Macro Trigger
            if ((allPins & macroButtonMask) &&
                ((gamepad->state.buttons & macro->macroTriggerButton) ||
                    (gamepad->state.dpad & (macro->macroTriggerButton >> 16))) ) {
                pressedMacro = i;
                break;
            }
        } else if ( allPins & macroPinMasks[i] ) {
            // Use Pin Manager for Macro Trigger
            pressedMacro = i;
            break;
        }
    }
}

void InputMacro::checkMacroAction() {
    bool macroInputPressed = (pressedMacro != -1); // Was any macro input pressed?

    // Is our pressed macro button different from our current macro AND no macro is running?
    if ( pressedMacro != macroPosition && !isMacroRunning ) {
        macroPosition = pressedMacro; // move our position to that macro
    }

    // CUSTOM: guard against reading macroList[-1] when nothing is pressed
    if (macroPosition < 0) {
        prevMacroInputPressed = macroInputPressed;
        return;
    }

    bool newPress = macroInputPressed && (prevMacroInputPressed ^ macroInputPressed);

    // Check to see if we should change the current macro (or turn off based on input)
    if ( inputMacroOptions->macroList[macroPosition].macroType == ON_PRESS ) {
        // START Macro: On Press or On Hold Repeat
        if (!isMacroRunning ) {
            isMacroTriggerHeld = newPress;
        }
    } else if ( inputMacroOptions->macroList[macroPosition].macroType == ON_HOLD_REPEAT ) {
        isMacroTriggerHeld = macroInputPressed;
    } else if ( inputMacroOptions->macroList[macroPosition].macroType == ON_TOGGLE ) {
        //isMacroTriggerHeld = macroInputPressed;
        if (!isMacroRunning ) {
            isMacroTriggerHeld = newPress;
        } else if (isMacroRunning && newPress) {
            // STOP Macro: Toggle on new press
            reset(); // Stop Macro: Toggle
            prevMacroInputPressed = macroInputPressed;
            return;
        }
    }

    prevMacroInputPressed = macroInputPressed;
    if (!isMacroRunning && isMacroTriggerHeld && pressedMacro >= 0) {
        // New Macro to run
        macroPosition = pressedMacro; // Set current macro
        Macro& macro = inputMacroOptions->macroList[macroPosition];
        uint64_t now = getMicro();

        // CUSTOM: random timings + smart start
        prepareSteps(macro, now);
        macroInputPosition = startPosition(macro, now);
        macroInputHoldTime = stepHoldTime(macro, macroInputPosition);

        isMacroRunning = true;
        macroStartTime = now; // current time
    }
}

void InputMacro::runCurrentMacro() {
    // Do nothing if macro is not currently running
    if (!isMacroRunning ||
            macroPosition == -1)
        return;

    Macro& macro = inputMacroOptions->macroList[macroPosition];

    // Stop Macro if released (ON PRESS & ON HOLD REPEAT)
    if (inputMacroOptions->macroList[macroPosition].macroType == ON_HOLD_REPEAT &&
            !isMacroTriggerHeld ) {
        reset();
        return;
    }

    Gamepad * gamepad = Storage::getInstance().GetGamepad();
    currentMicros = getMicro();

    if (!macro.interruptible && macro.exclusive) {
        // Prevent any other inputs from modifying our input (Exclusive)
        gamepad->state.dpad = 0;
        gamepad->state.buttons = 0;
    } else {
        if (macro.useMacroTriggerButton) {
            // Remove the trigger button from the input state
            gamepad->state.dpad &= ~(macro.macroTriggerButton >> 16);
            gamepad->state.buttons &= ~macro.macroTriggerButton;
        }
        if (macro.interruptible &&
            (gamepad->state.buttons != 0 || gamepad->state.dpad != 0)) {
            // Macro is interruptible and a user pressed something
            reset();
            return;
        }
    }

    // Have we elapsed the input hold time?
    if ((currentMicros - macroStartTime) >= macroInputHoldTime) {
        macroStartTime = currentMicros;
        macroInputPosition++;

        if ((int)macroInputPosition >= stepCount(macro)) {
            if ( macro.macroType == ON_PRESS ) {
                reset(); // On press = no more macro
                return;
            } else {
                restart(macro); // On Hold-Repeat or On Toggle = start macro again
            }
        } else {
            macroInputHoldTime = stepHoldTime(macro, macroInputPosition);
        }
    }

    // CUSTOM: read the CURRENT step (after any advance) and apply the gear
    int pos = (int)macroInputPosition;
    uint32_t buttonMask = macro.macroInputs[pos].buttonMask;
    if (isGearMacro(macro) && gearMask[macroPosition] && (buttonMask & ATTACK_MASK)) {
        buttonMask = (buttonMask & ~ATTACK_MASK) | gearMask[macroPosition];
    }

    // Check if we should still hold this macro input based on duration
    if ((currentMicros - macroStartTime) <= stepDuration[pos]) {
        if (buttonMask & GAMEPAD_MASK_DU) {
            gamepad->state.dpad |= GAMEPAD_MASK_UP;
        }
        if (buttonMask & GAMEPAD_MASK_DD) {
            gamepad->state.dpad |= GAMEPAD_MASK_DOWN;
        }
        if (buttonMask & GAMEPAD_MASK_DL) {
            gamepad->state.dpad |= GAMEPAD_MASK_LEFT;
        }
        if (buttonMask & GAMEPAD_MASK_DR) {
            gamepad->state.dpad |= GAMEPAD_MASK_RIGHT;
        }
        gamepad->state.buttons |= buttonMask;

        // Macro LED is on if we're currently running and inputs are doing something (wait-timers turn it off)
        if (boardLedEnabled) {
            gpio_put(BOARD_LED_PIN, (gamepad->state.dpad || gamepad->state.buttons) ? 1 : 0);
        }
    }
}

void InputMacro::preprocess()
{
    FocusModeOptions * focusModeOptions = &Storage::getInstance().getAddonOptions().focusModeOptions;
    if (focusModeOptions->enabled && focusModeOptions->macroLockEnabled) {
        Gamepad * gamepad = Storage::getInstance().GetGamepad();
        // Override Toggle Pressed OR focus mode pin is set
        if (focusModeOptions->overrideEnabled ||
            (gamepad->mapFocusMode->pinMask && (gamepad->debouncedGpio & gamepad->mapFocusMode->pinMask))) {
            return;
        }
    }

    // CUSTOM: remember when each direction was last physically held
    Gamepad * gamepad = Storage::getInstance().GetGamepad();
    uint64_t now = getMicro();
    trackDirections(gamepad->state.dpad, now);

    checkMacroPress();

    // CUSTOM: gear selection (attack held + G macro held for 2 seconds)
    uint32_t attackHeld = gamepad->state.buttons & ATTACK_MASK;
    Mask_t allPins = gamepad->debouncedGpio;

    if (gearPendingMacro >= 0) {
        bool pinHeld = (allPins & macroPinMasks[gearPendingMacro]) != 0;
        if (!pinHeld && attackHeld == 0) {
            // Both released: leave gear mode, continue normally
            gearPendingMacro = -1;
            prevMacroInputPressed = (pressedMacro != -1);
        } else {
            // Changed or released one of them: cancel the timer
            if (!pinHeld || attackHeld != gearPendingMask) {
                gearArmed = false;
            }
            if (gearArmed && (now - gearPendingStart) >= GEAR_HOLD_US) {
                gearMask[gearPendingMacro] = gearPendingMask;
                gearArmed = false;
            }
            gamepad->state.buttons &= ~ATTACK_MASK; // attack never reaches the game
            prevMacroInputPressed = (pressedMacro != -1);
            return;                                  // macro does not fire
        }
    }

    if (!isMacroRunning && pressedMacro >= 0 && !prevMacroInputPressed && attackHeld &&
            isGearMacro(inputMacroOptions->macroList[pressedMacro]) &&
            !inputMacroOptions->macroList[pressedMacro].useMacroTriggerButton) {
        gearPendingMacro = pressedMacro;
        gearPendingMask  = attackHeld;
        gearPendingStart = now;
        gearArmed        = (__builtin_popcount(attackHeld) == 1); // only one attack allowed
        gamepad->state.buttons &= ~ATTACK_MASK;
        prevMacroInputPressed = true;
        return;
    }

    checkMacroAction();
    runCurrentMacro();
}

void InputMacro::reinit() {
    GpioMappingInfo* pinMappings = Storage::getInstance().getProfilePinMappings();
    macroButtonMask = 0;
    memset(macroPinMasks, 0, sizeof(macroPinMasks));
    for (Pin_t pin = 0; pin < (Pin_t)NUM_BANK0_GPIOS; pin++)
    {
        switch( pinMappings[pin].action ) {
            case GpioAction::BUTTON_PRESS_MACRO:
                macroButtonMask = 1 << pin;
                break;
            case GpioAction::BUTTON_PRESS_MACRO_1:
                macroPinMasks[0] = 1 << pin;
                break;
            case GpioAction::BUTTON_PRESS_MACRO_2:
                macroPinMasks[1] = 1 << pin;
                break;
            case GpioAction::BUTTON_PRESS_MACRO_3:
                macroPinMasks[2] = 1 << pin;
                break;
            case GpioAction::BUTTON_PRESS_MACRO_4:
                macroPinMasks[3] = 1 << pin;
                break;
            case GpioAction::BUTTON_PRESS_MACRO_5:
                macroPinMasks[4] = 1 << pin;
                break;
            case GpioAction::BUTTON_PRESS_MACRO_6:
                macroPinMasks[5] = 1 << pin;
                break;
            default:
                break;
        }
    }
}

