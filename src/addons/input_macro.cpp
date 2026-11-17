#include "addons/input_macro.h"
#include "storagemanager.h"
#include "GamepadState.h"

#include "hardware/gpio.h"

// ============================================================================
//  CUSTOM MACRO FIRMWARE
//
//  1) Gear macros (label starts with "G "):
//     hold an attack button + the macro button for 2 s -> that attack becomes
//     the macro's gear. Smart start skips the first direction if it is held or
//     was released within 6 frames.
//
//  2) Randomized timings for every macro (new random values each run):
//     - Normal macros ............ arrows 1.5-3f, attack 3-7f,
//                                  OD attack 2-5f, block after OD 4-8f
//     - "super" in the label ..... arrows 1.2-3f (at least 4 arrows under 2f),
//                                  attack 2-6f, plus HUMAN ERROR
//     - Viper mode, other macros . arrows 1.2-3f (mostly under 2f), attack 2-6f
//
//  3) EARLY ARROW RELEASE (every macro): on the last arrow + button step the arrow is
//     released 2-3f before the button and the stick goes back to the user, always
//     keeping at least 1.2f of arrow + button together. 25% of the time it is skipped.
//
//  4) VIPER MODE: active when any macro label starts with "V ".
//     Macro 5 button = flip side, Macro 6 button = burnout on/off.
//     Automatic cancels after HP / d.HP / MK / HK, seismo loop, X = Level 1 in
//     burnout, 43f lock after any jump. LEDs show side and burnout.
// ============================================================================

// Shared with the LED add-on (neopicoleds.cpp)
volatile bool g_viperActive      = false;
volatile bool g_viperFacingRight = true;   // true = player 1 side (left)
volatile bool g_viperBurnout     = false;
volatile bool g_sagatMode        = false;   // built-in Sagat macro set active (LEDs light yellow)
volatile bool g_sagatBurnout     = false;   // Sagat burnout (orange LEDs, X = Level 1)
volatile int  g_profilePic       = 0;       // screen picture: 0 Sagat, 1 Arjin, 2 Manon, 3 Viper (stored splash), 4 Luke, 5 Lily, 6 Cammy, 7 Aki, 8 Chun-Li, 9 Ingrid, 10 Kimberly, 11 Jamie
volatile uint32_t g_ledBase      = 0xFFEB8C; // built-in profile LED colour (0xRRGGBB)
volatile uint32_t g_ledBurn      = 0xFF7800; // ... and its burnout colour
volatile bool g_ledRainbow       = false;    // Kimberly: fixed rainbow (a little dimmer) instead of g_ledBase
volatile uint32_t g_ledGear      = 0;        // colour of the gear buttons (Macro 1 / 2) by the gear on them; 0 = profile colour
volatile int  g_ledFreePin       = -1;       // GPIO pin of the free button (of Macro 5 / 6), -1 = none
volatile uint32_t g_ledFreeColor = 0;        // ... and its colour by the move stored on it
volatile int  g_ledAfbPin        = -1;       // GPIO pin of the anti-fireball button (21), -1 = none in this profile
volatile uint32_t g_ledAfbColor  = 0;        // ... and its colour by the strength stored on it
volatile uint32_t g_sagatGearMask = 0;      // attack button of the current gear (red LED)

namespace {
    // ---------------------------------------------------------------- tunables
    constexpr uint32_t FRAME_US        = 16667;           // 1 frame at 60fps
    constexpr uint64_t GEAR_HOLD_US    = 1000000;         // 1 second
    constexpr uint64_t SKIP_WINDOW_US  = 6 * FRAME_US;    // smart-start window

    // Frames -> microseconds (x10 to allow one decimal: F10(12) = 1.2 frames)
    constexpr uint32_t F10(uint32_t tenths) { return (uint32_t)((uint64_t)tenths * FRAME_US / 10); }

    // Normal macros
    constexpr uint32_t DIR_MIN_US      = F10(15);
    constexpr uint32_t DIR_MAX_US      = F10(30);
    constexpr uint32_t ATK_MIN_US      = F10(30);
    constexpr uint32_t ATK_MAX_US      = F10(70);
    constexpr uint32_t OD_ATK_MIN_US   = F10(20);
    constexpr uint32_t OD_ATK_MAX_US   = F10(50);
    constexpr uint32_t BLOCK_MIN_US    = F10(40);
    constexpr uint32_t BLOCK_MAX_US    = F10(80);

    // Supers / Viper-mode macros
    constexpr uint32_t FAST_MIN_US     = F10(12);
    constexpr uint32_t FAST_MID_US     = F10(20);
    constexpr uint32_t FAST_MAX_US     = F10(30);
    constexpr uint32_t BTN_MIN_US      = F10(20);
    constexpr uint32_t BTN_MAX_US      = F10(60);
    constexpr int      SUPER_FAST_COUNT = 4;              // arrows forced under 2f
    constexpr int      VIPER_FAST_PCT   = 70;             // % of arrows under 2f

    // Human error
    constexpr uint32_t HE_MIN_US       = F10(20);
    constexpr uint32_t HE_MAX_US       = F10(30);
    constexpr uint32_t HE_KEEP_US      = F10(12);

    // Viper
    constexpr uint32_t V_ARROW_MIN     = F10(12);
    constexpr uint32_t V_ARROW_MAX     = F10(35);
    constexpr uint64_t V_JUMP_LOCK_US  = 43ULL * FRAME_US;
    constexpr uint32_t V_WIN_MK_MIN    = F10(200);          // MK held (N or ↓) 20-24f = MK window
    constexpr uint32_t V_WIN_MK_MAX    = F10(240);
    constexpr uint64_t V_WIN_HK_US     = 14ULL * FRAME_US;
    constexpr uint64_t V_WIN_LOOP_US   = 36ULL * FRAME_US;  // after a seismo: hold → + punch, 36f max
    constexpr uint64_t V_NO_CANCEL_US  = 10ULL * FRAME_US;  // loop ran out: 10f with no cancels
    constexpr uint64_t V_CHP_PUNCH_US  = 10ULL * FRAME_US;  // punch within 10f of cr.HP -> plain seismo
    constexpr uint64_t V_WIN_LV2_US    = 20ULL * FRAME_US;
    constexpr uint64_t V_CHORD_US      = 2ULL * FRAME_US;  // parry / DI guard after a trigger
    constexpr uint64_t V_CHORD_WIN_US  = 3ULL * FRAME_US;  // window presses: 3f to catch 2 buttons (OD)
    constexpr uint64_t V_SEISMO_BTN_US = 4ULL * FRAME_US;  // punch within 4f of last arrow
    constexpr uint64_t V_MOTION_US     = 20ULL * FRAME_US; // whole DP motion must be this recent
    constexpr uint64_t V_SPECIAL_US    = 10ULL * FRAME_US; // down input this recent = special move

    // Buttons: LP=L3, MP=B4(Y), HP=R1, LK=B1(A), MK=B2(B), HK=R2, X=B3
    constexpr uint32_t LP = GAMEPAD_MASK_L3, MP = GAMEPAD_MASK_B4, HP = GAMEPAD_MASK_R1;
    constexpr uint32_t LK = GAMEPAD_MASK_B1, MK = GAMEPAD_MASK_B2, HK = GAMEPAD_MASK_R2;
    constexpr uint32_t XBTN = GAMEPAD_MASK_B3;
    constexpr uint32_t PUNCH_MASK  = LP | MP | HP;
    constexpr uint32_t KICK_MASK   = LK | MK | HK;
    constexpr uint32_t ATTACK_MASK = PUNCH_MASK | KICK_MASK;
    constexpr uint32_t DIR_MASK    = GAMEPAD_MASK_DU | GAMEPAD_MASK_DD |
                                     GAMEPAD_MASK_DL | GAMEPAD_MASK_DR;

    constexpr int MAX_STEPS = 64;

    // ------------------------------------------------------------- shared rng
    uint32_t rngState = 0x12345678;

    uint32_t nextRandom() {
        uint32_t x = rngState;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        rngState = (x == 0) ? 1 : x;
        return rngState;
    }
    uint32_t randomRange(uint32_t lo, uint32_t hi) {
        if (hi <= lo) return lo;
        return lo + (nextRandom() % (hi - lo + 1));
    }
    bool coin() { return (nextRandom() & 1) != 0; }
    int popcount32(uint32_t v) { return __builtin_popcount(v); }
    uint32_t lowestBit(uint32_t v) { return v & (~v + 1); }

    // ===================================================== regular macro state
    uint32_t stepDuration[MAX_STEPS];
    uint32_t stepHeCut[MAX_STEPS];                // arrow released this early
    uint32_t gearMask[MAX_MACRO_LIMIT] = {0};

    uint64_t dirLastHeld[4] = {0, 0, 0, 0};      // U, D, L, R (physical)

    int      gearPendingMacro = -1;
    uint32_t gearPendingMask  = 0;
    uint64_t gearPendingStart = 0;
    bool     gearArmed        = false;

    bool viperMode = false;

    // Macro on/off button: GPIO pin 20 (keep it mapped to any button, e.g. A1;
    // that button is not sent to the game). Macros start ON after every power-up.
    constexpr uint32_t TOGGLE_PIN_MASK = 1u << 20;
    bool macrosOn = true;
    bool prevTogglePin = false;

    // CUSTOM: Sagat macro set (from final.gp2040), built into the firmware
    MacroOptions g_sagatOpts;
    struct SagatStep  { uint32_t mask, dur; };          // dur in microseconds, no wait after any step
    struct SagatMacro { const char* label; uint8_t n; SagatStep s[6]; };
    const SagatMacro SAGAT_MACROS[6] = {
        {"G DP right",   3, {{524288u, 24999u}, {655360u, 23332u}, {525312u, 33332u}}},
        {"G DP left",    3, {{262144u, 23332u}, {393216u, 23332u}, {263168u, 49998u}}},
        {"super right ", 6, {{131072u, 39996u}, {655360u, 24165u}, {524288u, 24999u}, {131072u, 23332u}, {655360u, 23330u}, {524416u, 39996u}}},
        {"super left ",  6, {{131072u, 23330u}, {393216u, 24999u}, {262144u, 24999u}, {131072u, 23330u}, {393216u, 39996u}, {262272u, 56662u}}},
        {"dp right o",   3, {{524288u, 26664u}, {655360u, 23332u}, {525344u, 39996u}}},
        {"dp left oo",   3, {{262144u, 39996u}, {393216u, 23330u}, {263200u, 56662u}}},
    };
    void buildSagatOptions() {
        MacroOptions& o = g_sagatOpts;
        memset(&o, 0, sizeof(o));
        o.has_enabled = true; o.enabled = true; o.macroList_count = 6;
        for (int i = 0; i < 6; i++) {
            Macro& m = o.macroList[i];
            const SagatMacro& d = SAGAT_MACROS[i];
            m.has_enabled = true; m.enabled = true; m.has_exclusive = true; m.exclusive = true;
            m.has_interruptible = true; m.interruptible = false; m.has_showFrames = true; m.showFrames = true;
            m.has_macroType = true; m.macroType = (MacroType)1; m.has_useMacroTriggerButton = true; m.useMacroTriggerButton = false;
            m.has_macroLabel = true; strncpy(m.macroLabel, d.label, sizeof(m.macroLabel) - 1);
            m.macroInputs_count = d.n;
            for (int k = 0; k < d.n; k++) {
                MacroInput& in = m.macroInputs[k];
                in.has_buttonMask = true; in.buttonMask = d.s[k].mask;
                in.has_duration = true; in.duration = d.s[k].dur;
                in.has_waitDuration = true; in.waitDuration = 0;
            }
        }
    }
    bool sagatActive = false;       // a built-in profile (Sagat or pink) is active
    bool pinkActive = false;        // Manon profile
    bool arjinActive = false;       // Arjin profile
    bool lukeActive = false;        // Luke profile (Sagat set + held punch after qcf/qcb)
    bool lilyActive = false;        // Lily profile (SPD on Macro 5/6, qcb/qcf supers)
    bool cammyActive = false;       // Cammy profile (Sagat gear, kick OD DP, air qcb kick after forward jump)
    bool akiActive = false;         // Aki profile (all six macros run by the Aki engine, ↓HK -> X = Level 1)
    bool ingridActive = false;      // Ingrid profile (Aki engine: qcf/qcb + LK, punch supers, standing HK -> X = Level 1)
    bool chunActive = false;        // Chun-Li profile (↓↓ kick gear, air qcf kick dive; all six macros run by chunProcess)
    bool jamieActive = false;       // Jamie profile (Cammy's engine; its free button does the command grab / qcf kick)
    bool kimActive = false;         // Kimberly profile (anti-air gear, air qcf punch / qcb kick; all six macros run by kimProcess)
    MacroOptions g_pinkOpts;        // Manon
    MacroOptions g_cammyOpts;       // Cammy, Jamie
    MacroOptions g_gearOnlyOpts;    // Lily, Arjin: Sagat gear on Macro 1/2, Macro 3-6 run by the engine
    MacroOptions g_engineOpts;      // Aki, Chun-Li, Ingrid, Kimberly: all six macros run by the engine
    void buildEngineOptions() {
        g_engineOpts = g_sagatOpts;
        for (int i = 0; i < 6; i++) g_engineOpts.macroList[i].enabled = false;
        g_gearOnlyOpts = g_sagatOpts;
        for (int i = 2; i < 6; i++) g_gearOnlyOpts.macroList[i].enabled = false;
    }
    void buildCammyOptions() {
        g_cammyOpts = g_sagatOpts;
        g_cammyOpts.macroList[2].enabled = false;   // supers run by the Cammy engine
        g_cammyOpts.macroList[3].enabled = false;
        for (int i = 4; i < 6; i++) {               // OD DP: two kicks instead of two punches
            Macro& m = g_cammyOpts.macroList[i];
            for (int k = 0; k < (int)m.macroInputs_count; k++) {
                uint32_t b = m.macroInputs[k].buttonMask, o = b & ~(LP | MP | HP);
                if (b & LP) o |= LK;
                if (b & MP) o |= MK;
                if (b & HP) o |= HK;
                m.macroInputs[k].buttonMask = o;
            }
        }
    }
    // The pin 20 + Macro 1 (next) / Macro 2 (previous) cycle, in order.
    enum { P_SAGAT, P_LUKE, P_CAMMY, P_JAMIE, P_AKI, P_CHUN, P_INGRID, P_KIM, P_LILY, P_ARJIN, P_MANON, P_VIPER, P_COUNT };
    struct Profile { MacroOptions* opts; int pic; uint32_t ledBase, ledBurn; };   // opts nullptr = stored config
    const Profile PROFILES[P_COUNT] = {
        {&g_sagatOpts,    0,  0xFFEB8C, 0xFF7800},   // Sagat: light yellow / orange
        {&g_sagatOpts,    4,  0x5AC8FF, 0x001EFF},   // Luke: light blue / blue
        {&g_cammyOpts,    6,  0x00FFFF, 0x00506E},   // Cammy: cyan
        {&g_cammyOpts,    11, 0x808080, 0xFFDC00},   // Jamie: grey / yellow
        {&g_engineOpts,   7,  0xAA3CFF, 0x46006E},   // Aki: purple
        {&g_engineOpts,   8,  0x87CEFA, 0x003CFF},   // Chun-Li: sky blue / blue
        {&g_engineOpts,   9,  0xB57EDC, 0xE6D7FF},   // Ingrid: lavender / pale lavender
        {&g_engineOpts,   10, 0xFFEB8C, 0x808080},   // Kimberly: rainbow / grey
        {&g_gearOnlyOpts, 5,  0x0ABAB5, 0x005F5A},   // Lily: tiffany
        {&g_gearOnlyOpts, 1,  0xFFEB8C, 0xFF7800},   // Arjin: like Sagat
        {&g_pinkOpts,     2,  0xFF3CA0, 0x9600FF},   // Manon: pink / purple
        {nullptr,         3,  0xFFEB8C, 0xFF7800},   // Viper (stored config, its own LED code)
    };
    int  profileIdx = P_SAGAT;
    void setProfileFlags() {            // everything that follows from profileIdx
        int p = profileIdx;
        sagatActive  = (p != P_VIPER);
        lukeActive   = (p == P_LUKE);
        jamieActive  = (p == P_JAMIE);
        cammyActive  = (p == P_CAMMY) || jamieActive;   // Jamie runs on Cammy's engine and options
        akiActive    = (p == P_AKI);
        chunActive   = (p == P_CHUN);
        ingridActive = (p == P_INGRID);
        kimActive    = (p == P_KIM);
        lilyActive   = (p == P_LILY);
        arjinActive  = (p == P_ARJIN);
        pinkActive   = (p == P_MANON);
        g_profilePic = PROFILES[p].pic;
        g_ledBase = PROFILES[p].ledBase; g_ledBurn = PROFILES[p].ledBurn; g_ledRainbow = kimActive;
        g_sagatMode = sagatActive;
    }
    void buildPinkOptions() {
        g_pinkOpts = g_sagatOpts;
        // Macro 1/2 (gear) and 5/6 (command grab) are run by the pink engine, not as plain macros
        g_pinkOpts.macroList[0].enabled = false;
        g_pinkOpts.macroList[1].enabled = false;
        g_pinkOpts.macroList[4].enabled = false;
        g_pinkOpts.macroList[5].enabled = false;
        // supers swapped: Macro 3 = Sagat's left super, Macro 4 = Sagat's right super
        g_pinkOpts.macroList[2] = g_sagatOpts.macroList[3];
        g_pinkOpts.macroList[3] = g_sagatOpts.macroList[2];
    }
    uint32_t gearSwallow = 0;
    bool superLight = false;        // this super ends with the light (true) or heavy button
    uint32_t turboPinMask = 0;
    bool togglePinCombo = false;   // Macro 1 pressed while pin 20 held (mode switch)
    bool prevComboMacro1 = false;
    int      sideDiag = 0;         // user's held down-diagonal: +1 = ↙, -1 = ↘ (SOCD cleaned)
    uint64_t sideDiagAt = 0;
    bool prevComboMacro2 = false;  // pin 20 + Macro 2 = previous profile

    bool isGearMacro(const Macro& macro) {
        return macro.macroLabel[0] == 'G' && macro.macroLabel[1] == ' ';
    }
    bool isViperMarker(const Macro& macro) {
        return (macro.macroLabel[0] == 'V' || macro.macroLabel[0] == 'v') && macro.macroLabel[1] == ' ';
    }
    bool isSuperMacro(const Macro& macro) {
        const char* s = macro.macroLabel;
        for (int i = 0; s[i] != 0 && s[i + 1] != 0 && s[i + 2] != 0 && s[i + 3] != 0 && s[i + 4] != 0; i++) {
            if ((s[i] | 32) == 's' && (s[i+1] | 32) == 'u' && (s[i+2] | 32) == 'p' &&
                (s[i+3] | 32) == 'e' && (s[i+4] | 32) == 'r')
                return true;
        }
        return false;
    }

    int stepCount(const Macro& macro) {
        int n = (int)macro.macroInputs_count;
        return n > MAX_STEPS ? MAX_STEPS : n;
    }

    // Early arrow release (every macro): on the last arrow + button step the arrow lets go 2-3f before the
    // button and the stick goes back to the user; the arrow always stays at least 1.2f with the button.
    // One time in four it does not happen: the arrow stays with the button to the end.
    uint32_t arrowCut(uint32_t dur) {
        if (nextRandom() % 4 == 0) return 0;
        if (dur <= HE_KEEP_US) return 0;
        uint32_t cut = randomRange(HE_MIN_US, HE_MAX_US);
        uint32_t maxCut = dur - HE_KEEP_US;
        return cut > maxCut ? maxCut : cut;
    }

    void prepareStepDurations(const Macro& macro, int n);
    // Randomize the duration of every step for this run of a regular macro
    void prepareSteps(const Macro& macro, uint64_t now) {
        rngState ^= (uint32_t)now;
        if (rngState == 0) rngState = 1;

        int n = stepCount(macro);
        for (int i = 0; i < n; i++) stepHeCut[i] = 0;
        prepareStepDurations(macro, n);
        int last = -1;                          // early arrow release on the last arrow + button step
        for (int i = 0; i < n; i++) {
            uint32_t m = macro.macroInputs[i].buttonMask;
            if ((m & ATTACK_MASK) && (m & DIR_MASK)) last = i;
        }
        if (last >= 0) stepHeCut[last] = arrowCut(stepDuration[last]);
    }
    void prepareStepDurations(const Macro& macro, int n) {

        // ---------------- supers: fast arrows + human error
        if (isSuperMacro(macro)) {
            superLight = coin();            // random light / heavy version of the button
            int arrows[MAX_STEPS]; int na = 0;
            for (int i = 0; i < n; i++) {
                uint32_t m = macro.macroInputs[i].buttonMask;
                if ((m & DIR_MASK) && !(m & ATTACK_MASK)) arrows[na++] = i;
            }
            // shuffle arrow list, first SUPER_FAST_COUNT become "under 2f"
            for (int i = na - 1; i > 0; i--) {
                int j = (int)(nextRandom() % (uint32_t)(i + 1));
                int t = arrows[i]; arrows[i] = arrows[j]; arrows[j] = t;
            }
            for (int k = 0; k < na; k++) {
                stepDuration[arrows[k]] = (k < SUPER_FAST_COUNT)
                    ? randomRange(FAST_MIN_US, FAST_MID_US)
                    : randomRange(FAST_MIN_US, FAST_MAX_US);
            }
            for (int i = 0; i < n; i++) {
                uint32_t m = macro.macroInputs[i].buttonMask;
                if (m & ATTACK_MASK) stepDuration[i] = randomRange(BTN_MIN_US, BTN_MAX_US);
                else if (!(m & DIR_MASK)) stepDuration[i] = macro.macroInputs[i].duration;
            }
            return;
        }

        // ---------------- Viper mode: fast arrows, mostly under 2f
        if (viperMode) {
            for (int i = 0; i < n; i++) {
                uint32_t m = macro.macroInputs[i].buttonMask;
                if (m & ATTACK_MASK) stepDuration[i] = randomRange(BTN_MIN_US, BTN_MAX_US);
                else if (m & DIR_MASK)
                    stepDuration[i] = randomRange(F10(12), F10(22));   // arrows 1.2-2.2f
                else stepDuration[i] = macro.macroInputs[i].duration;
            }
            return;
        }

        // ---------------- normal (Sagat etc.)
        bool isOD = false;
        int firstAttack = -1;
        for (int i = 0; i < n; i++) {
            uint32_t attacks = macro.macroInputs[i].buttonMask & ATTACK_MASK;
            if (attacks) {
                if (firstAttack < 0) firstAttack = i;
                if (popcount32(attacks) >= 2) isOD = true;
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
                } else if (isGearMacro(macro)) {
                    stepDuration[i] = randomRange(F10(12), F10(22));   // gear (DP) macro arrows 1.2-2.2f
                } else {
                    stepDuration[i] = randomRange(DIR_MIN_US, DIR_MAX_US);
                }
            } else {
                stepDuration[i] = macro.macroInputs[i].duration;
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

    // =========================================================== VIPER ENGINE
    // Relative directions (F = toward the opponent)
    constexpr uint8_t RU = 1, RD = 2, RF = 4, RB = 8;
    constexpr uint8_t N_ = 0, U_ = RU, D_ = RD, F_ = RF, B_ = RB;
    constexpr uint8_t DF = RD | RF, DB = RD | RB, UF = RU | RF, UB = RU | RB;

    constexpr uint8_t FL_ABORT_FWD = 1;   // step aborts if the player holds forward
    constexpr uint8_t FL_OPEN_LOOP = 2;   // seismo loop window starts at this step
    constexpr uint8_t FL_JUMP      = 4;   // character leaves the ground here (lock after this step)
    constexpr uint8_t FL_LOCK      = 8;   // jump lock starts at the start of this step
    constexpr uint8_t FL_BACK      = 16;  // loop opened here can be released with ← (seismo after MK)
    constexpr uint8_t FL_USER_DIR  = 32;  // the stick stays the user's during this step (buttons still ours)
    constexpr uint8_t FL_OWN_CUT   = 64;  // this step sets its own early arrow release (heCut), not the general rule
    constexpr uint8_t FL_SEISMO    = 128; // the seismo punch: the 20f seismo window opens when this step starts

    struct VStep { uint8_t dir; uint32_t btn; uint32_t dur; uint32_t heCut; uint8_t flags; };

    enum VWin { W_NONE, W_MK, W_SZ };       // open window: after your MK (10f), after a seismo (20f)
    // Jump protection for the double-press cancels (Viper, Chun-Li): for 43f after you press up, the first
    // press of a cancel button is not part of any cancel (a normal hit in the air). Once per jump.
    struct JumpGuard {
        uint64_t until = 0;
        bool used = true;
        void arm(uint64_t now) { until = now + 43ULL * FRAME_US; used = false; }
        bool take(uint64_t now) {                // true = this press is the protected one
            if (used || now >= until) return false;
            used = true;
            return true;
        }
    };
    constexpr uint32_t VIPER_CANCEL_BTNS = HP | MK;         // buttons that start a cancel (HP double press, MK window)
    constexpr uint32_t CHUN_CANCEL_BTNS  = HP | MP | HK;

    struct VState {
        bool facingRight = true;
        bool burnout = false;

        VStep seq[16];
        int seqLen = 0, seqPos = 0;
        bool playing = false;
        uint64_t stepStart = 0;
        bool isCrouchHP = false;      // X can cancel this one
        // HP cancels, Chun-Li style: your first HP goes out as it is; a second HP inside the window is hidden
        // and starts the cancel (st.HP: within 18f -> right away; cr.HP: within 12f -> frame 8-12)
        uint64_t hpAt = 0, hpFireAt = 0;
        bool hpCrouch = false;
        JumpGuard jumpGuard;
        uint32_t prevLv1Pins = 0;     // Macro 5 / 6 (Level 1)

        // Windows: after your MK (10f from the press) and after any seismo (20f from its punch). The first
        // button you press inside is hidden and remembered; its cancel starts when the window ends.
        VWin win = W_NONE;
        uint64_t winAt = 0;           // the MK press / the seismo punch press
        uint64_t winLen = 0;          // MK window: 10f standing, 12f crouching
        uint32_t winPick = 0;
        bool burnSeq = false;         // the sequence playing is a burning kick
        uint64_t burnLockUntil = 0;   // 70f after a burning kick: only Macro 1-6 work

        uint32_t suppress = 0;        // attack bits hidden from the game until released
        uint32_t prevButtons = 0;
        uint8_t  prevRel = 0;
        uint64_t qcbBlockUntil = 0;   // after a qcb macro (Macro 1/2): no st.HP cancel for 57f
        uint32_t prevQcbPins = 0;
        bool prevBurn = false;

        // direction history (relative codes)
        uint8_t  hCode[16];
        uint64_t hStart[16];
        int hHead = 0, hCount = 0;
    } V;

    uint32_t vArrow() { return randomRange(V_ARROW_MIN, V_ARROW_MAX); }
    uint32_t vBtn()   { return randomRange(BTN_MIN_US, BTN_MAX_US); }

    void vClear() { V.seqLen = 0; }
    void vPush(uint8_t dir, uint32_t btn, uint32_t dur, uint8_t flags = 0) {
        if (V.seqLen >= 16) return;
        V.seq[V.seqLen++] = {dir, btn, dur, 0, flags};
    }

    uint8_t toRel(uint8_t dpad) {
        uint8_t r = 0;
        bool up = dpad & GAMEPAD_MASK_UP, dn = dpad & GAMEPAD_MASK_DOWN;
        bool lf = dpad & GAMEPAD_MASK_LEFT, rt = dpad & GAMEPAD_MASK_RIGHT;
        if (up && !dn) r |= RU;
        if (dn && !up) r |= RD;
        if (rt && !lf) r |= V.facingRight ? RF : RB;
        if (lf && !rt) r |= V.facingRight ? RB : RF;
        return r;
    }
    uint8_t toAbs(uint8_t rel) {
        uint8_t d = 0;
        if (rel & RU) d |= GAMEPAD_MASK_UP;
        if (rel & RD) d |= GAMEPAD_MASK_DOWN;
        if (rel & RF) d |= V.facingRight ? GAMEPAD_MASK_RIGHT : GAMEPAD_MASK_LEFT;
        if (rel & RB) d |= V.facingRight ? GAMEPAD_MASK_LEFT : GAMEPAD_MASK_RIGHT;
        return d;
    }

    void histPush(uint8_t code, uint64_t now) {
        V.hHead = (V.hHead + 1) & 15;
        V.hCode[V.hHead] = code;
        V.hStart[V.hHead] = now;
        if (V.hCount < 16) V.hCount++;
    }
    // k = 0 newest
    bool histGet(int k, uint8_t& code, uint64_t& start, uint64_t& end, uint64_t now) {
        if (k >= V.hCount) return false;
        int idx = (V.hHead - k) & 15;
        code = V.hCode[idx];
        start = V.hStart[idx];
        end = (k == 0) ? now : V.hStart[(idx + 1) & 15];
        return true;
    }

    // Did the player just input a forward DP motion (→ ↓ ↘ or → ↘ →)?
    bool detectSeismoMotion(uint64_t now) {
        uint8_t c; uint64_t s, e;
        int k = 0;
        if (!histGet(0, c, s, e, now)) return false;
        if (c == N_) {                       // released: last arrow must be within 4f
            if (now - s > V_SEISMO_BTN_US) return false;
            k = 1;
            if (!histGet(1, c, s, e, now)) return false;
        }
        if (c != F_ && c != DF) return false;
        // find an earlier ↓ or ↘
        int j = k + 1; bool foundDown = false;
        for (; histGet(j, c, s, e, now); j++) {
            if (now - e > V_MOTION_US) return false;
            if (c == D_ || c == DF) { foundDown = true; break; }
        }
        if (!foundDown) return false;
        // find an earlier →
        for (j = j + 1; histGet(j, c, s, e, now); j++) {
            if (now - e > V_MOTION_US) return false;
            if (c == F_) return true;
        }
        return false;
    }

    // Any down input very recently? (then HP is part of a special move, not a normal)
    bool recentDown(uint64_t now) {
        uint8_t c; uint64_t s, e;
        for (int k = 0; histGet(k, c, s, e, now); k++) {
            if (now - e > V_SPECIAL_US) break;
            if (c & RD) return true;
        }
        return false;
    }

    // ---------------------------------------------------------- sequences
    void seqArrows(std::initializer_list<uint8_t> dirs, uint32_t lo10, uint32_t hi10) {
        for (uint8_t d : dirs) vPush(d, 0, randomRange(F10(lo10), F10(hi10)));
    }
    // motion of the seismo feint: ↓ 3-5f, ↘ 2.4-3f, → 3-5f, ↗ 3-5f
    void seismoMotion() {
        vClear();
        vPush(D_, 0, randomRange(F10(30), F10(50)));
        vPush(DF, 0, randomRange(F10(24), F10(30)));
        vPush(F_, 0, randomRange(F10(30), F10(50)));
        vPush(UF, 0, randomRange(F10(30), F10(50)));
    }
    // Cancel motion for the seismo and the burning kick (fixed, no randomness):
    // ↓ 3f, ↘ 2f, → 2f, ↗ 2f, then ↑ + the button 3f (a seismo opens its 20f window there)
    void seqCancel(uint32_t btn, bool seismo) {
        vClear();
        vPush(D_, 0, F10(30));
        vPush(DF, 0, F10(20));
        vPush(F_, 0, F10(20));
        vPush(UF, 0, F10(20));
        vPush(U_, btn, F10(30), seismo ? FL_SEISMO : 0);
    }
    void seqSeismo(uint32_t punches) { seqCancel(punches, true); }
    // Burning kick (fixed): ↓ 5f, ↘ 4f, → 2f, ↗ 4f, → + kick 1f, N + kick 4f
    void seqBurnKick(uint32_t kick) {
        vClear();
        vPush(D_, 0, F10(50));
        vPush(DF, 0, F10(40));
        vPush(F_, 0, F10(20));
        vPush(UF, 0, F10(40));
        vPush(F_, kick, F10(10));
        vPush(N_, kick, F10(40));
        V.burnSeq = true;
    }
    void seqMKSeismo(uint32_t punches) { seqCancel(punches, true); }
    void seqMKBurn(uint32_t kick) { seqBurnKick(kick); }
    void seqSeismoFeint() {                     // ... then ↗ + LP 3-4f, LK joins for 3-4f; LP lets go with it or 1-2f earlier
        seismoMotion();
        vPush(UF, LP, randomRange(F10(30), F10(40)));
        uint32_t kick = randomRange(F10(30), F10(40));
        uint32_t early = coin() ? 0 : randomRange(F10(10), F10(20));
        vPush(UF, LP | LK, kick - early);
        if (early) vPush(UF, LK, early);
    }
    void seqBurn(uint32_t kick) { seqBurnKick(kick); }
    void seqStandHP() {                         // after your st.HP / MK + LP: jump cancel -> thunder dash feint
        vClear();
        seqArrows({D_, DB, B_}, 12, 20);                             // fast arrows 1.2-2f
        vPush(UB, HP, randomRange(F10(40), F10(70)));                // the jump cancel
        vPush(B_, HP | LK, randomRange(F10(120), F10(160)));         // back from here on (never up-back)
        vPush(coin() ? B_ : N_, LK, randomRange(F10(10), F10(30)));  // kick only, arrow 50/50
    }
    void seqCrouchHP() {                        // after your cr.HP: thunder dash feint
        vClear();
        seqArrows({D_, DB}, 12, 20);
        vPush(B_, 0, randomRange(F10(50), F10(70)));
        vPush(B_, HP, randomRange(F10(15), F10(25)));
        vPush(B_, HP | LK, randomRange(F10(120), F10(130)));         // 12-13f, both let go together
    }
    void superArrowsAndFinish(std::initializer_list<uint8_t> dirs, uint8_t lastDir, uint32_t btn) {
        vClear();
        int n = (int)dirs.size();
        int fastIdx[8]; for (int i = 0; i < n; i++) fastIdx[i] = i;
        for (int i = n - 1; i > 0; i--) {
            int j = (int)(nextRandom() % (uint32_t)(i + 1));
            int t = fastIdx[i]; fastIdx[i] = fastIdx[j]; fastIdx[j] = t;
        }
        bool fast[8] = {false};
        for (int i = 0; i < n && i < SUPER_FAST_COUNT; i++) fast[fastIdx[i]] = true;
        int i = 0;
        for (uint8_t d : dirs) {
            vPush(d, 0, fast[i] ? randomRange(FAST_MIN_US, FAST_MID_US) : randomRange(FAST_MIN_US, FAST_MAX_US));
            i++;
        }
        uint32_t dur = vBtn();
        vPush(lastDir, btn, dur);
    }
    void seqLevel1() { superArrowsAndFinish({D_, DF, F_, D_, DF}, F_, coin() ? LK : HK); }
    void seqLevel2() { superArrowsAndFinish({D_, DB, B_, D_, DB}, B_, coin() ? LP : HP); }
    // Level 1 fixed on screen (Macro 5 / 6, like Aki): h = +1 -> ↓ ↘ → ↓ ↘ →, h = -1 -> ↓ ↙ ← ↓ ↙ ←, + LK / HK
    void seqLevel1Screen(int h) {
        bool fwd = (h > 0) == V.facingRight;
        uint8_t dg = fwd ? DF : DB, hz = fwd ? F_ : B_;
        superArrowsAndFinish({D_, dg, hz, D_, dg}, hz, coin() ? LK : HK);
    }

    void startSeq(uint64_t now, uint32_t trigger, uint64_t guardUs) {
        (void)trigger; (void)guardUs;
        bool burn = V.burnSeq;            // set by seqBurn / seqMKBurn just before
        V.playing = V.seqLen > 0;
        V.burnSeq = burn && V.playing;
        V.isCrouchHP = false;
        V.seqPos = 0;
        V.stepStart = now;
        for (int i = 0; i < V.seqLen; i++) if (!(V.seq[i].flags & FL_OWN_CUT)) V.seq[i].heCut = 0;
        for (int i = V.seqLen - 1; i >= 0; i--) {       // early arrow release on the last arrow + button step
            const VStep& st = V.seq[i];
            if (st.btn && st.dir != N_ && !(st.flags & FL_USER_DIR)) {
                if (!(st.flags & FL_OWN_CUT)) V.seq[i].heCut = arrowCut(st.dur);
                break;
            }
        }
    }

    // Output the current sequence step into the gamepad state. Returns false when finished.
    bool runSeq(Gamepad* gp, uint64_t now, uint8_t physRel) {
        (void)physRel;
        while (V.playing) {
            VStep& st = V.seq[V.seqPos];
            if (now - V.stepStart < st.dur) break;
            V.stepStart += st.dur;
            V.seqPos++;
            if (V.seqPos >= V.seqLen) {
                V.playing = false;
                if (V.burnSeq) V.burnLockUntil = V.stepStart + 70ULL * FRAME_US;   // the burning kick is over
                V.burnSeq = false;
                return false;
            }
            if (V.seq[V.seqPos].flags & FL_SEISMO) {    // the seismo punch comes out now: its 20f window opens
                V.win = W_SZ; V.winAt = V.stepStart; V.winPick = 0;
            }
        }
        if (!V.playing) return false;
        VStep& st = V.seq[V.seqPos];
        uint64_t el = now - V.stepStart;
        // the stick stays the user's on a FL_USER_DIR step and once the arrow has let go early
        if (!(st.flags & FL_USER_DIR) && el + st.heCut < st.dur) gp->state.dpad = toAbs(st.dir);
        gp->state.buttons = st.btn;
        return true;
    }

    // ---------------------------------------------------------- Viper engine
    //  HP (standing / walking, or crouching): your own hit; HP again inside 18f (crouch 12f) = the thunder feint cancel
    //  MK (standing or crouching): your own hit; 10f window: MP = seismo, LP = the st.HP feint, HP = OD seismo (LP+MP),
    //    a kick = burning kick of that kick on the seismo motion
    //  any seismo (yours or a macro's): 20f window: LP = seismo feint, MP / HP = seismo, LK / MK / HK = burning kick
    //  a window's cancel starts when the window ends, even if you pressed earlier (the seismo window's burning kick: always 19f after the punch)
    //  Macro 5 / 6 = Level 1 (fixed on screen); burnout (turbo button): X = Level 1
    //  jump protection: 43f after you press up, the first HP / MK press starts nothing (once per jump)
    //  after a burning kick: 70f where only Macro 1-6 work (no cancels, no windows, X is a plain X)
    constexpr uint64_t V_WIN_MK_US = 10ULL * FRAME_US;
    constexpr uint64_t V_WIN_SZ_US = 20ULL * FRAME_US;
    uint32_t pickOne(uint32_t m) {               // one button out of a press: LP, MP, HP, LK, MK, HK in that order
        for (uint32_t b : {LP, MP, HP, LK, MK, HK}) if (m & b) return b;
        return 0;
    }
    void viperReset() {
        V.playing = false; V.win = W_NONE; V.winPick = 0; V.suppress = 0;
        V.hpAt = 0; V.hpFireAt = 0; V.jumpGuard = JumpGuard();
        V.burnSeq = false; V.burnLockUntil = 0;
    }
    // Returns true when the Viper engine owns the output this poll.
    bool viperProcess(Gamepad* gp, uint64_t now, uint32_t m5, uint32_t m6, uint32_t burnPin, uint32_t qcbPins, bool otherMacroBusy) {
        Mask_t pins = gp->debouncedGpio;
        uint32_t qcbAll = (uint32_t)(pins & qcbPins);
        if (qcbAll & ~V.prevQcbPins) V.qcbBlockUntil = now + 57ULL * FRAME_US;   // qcb macro pressed
        V.prevQcbPins = qcbAll;
        // burnout toggle (turbo button, like the other profiles); the side is automatic (down-diagonal held 10f)
        bool bo = burnPin && (pins & burnPin);
        if (bo && !V.prevBurn) { V.burnout = !V.burnout; g_viperBurnout = V.burnout; }
        V.prevBurn = bo;
        uint32_t lv1Now = (uint32_t)(pins & (m5 | m6));
        uint32_t lv1Rising = lv1Now & ~V.prevLv1Pins;   // Macro 5 / 6: Level 1, fixed on screen
        V.prevLv1Pins = lv1Now;

        uint32_t phys = gp->state.buttons;
        uint8_t rel = toRel(gp->state.dpad);
        uint32_t rising = phys & ~V.prevButtons;
        bool upRising = (rel & RU) && !(V.prevRel & RU);
        V.prevButtons = phys;
        V.prevRel = rel;
        if (V.hCount == 0 || V.hCode[V.hHead] != rel) histPush(rel, now);   // direction history
        V.suppress &= phys;                       // released buttons are no longer hidden

        // ---- jump protection: the first HP / MK press after your jump is a normal hit and starts nothing
        if (upRising && !V.playing) V.jumpGuard.arm(now);
        uint32_t r = rising;                      // presses that may start something
        if ((r & VIPER_CANCEL_BTNS) && V.jumpGuard.take(now)) {
            r &= ~VIPER_CANCEL_BTNS;
            V.hpAt = 0; V.hpFireAt = 0;
        }

        // ---- open window: remember the first button, start its cancel when the window ends
        if (V.win != W_NONE) {
            // seismo window: punches count until 20f, a kick until 19f
            uint32_t accept = (V.win == W_SZ && now >= V.winAt + V_WIN_SZ_US - FRAME_US) ? PUNCH_MASK : ATTACK_MASK;
            uint32_t want = rising & accept;
            if (want && !V.winPick) V.winPick = pickOne(want);
            V.suppress |= want;                   // window presses never reach the game
            // MK window: its end. Seismo window: a punch cancel starts at 20f, the burning kick always at 19f
            // (frame 20 counting the punch press as frame 1, kick on frame 35); with nothing pressed it closes at 20f
            uint64_t endUs = V.win == W_MK ? V.winLen
                           : (V.winPick & KICK_MASK) ? V_WIN_SZ_US - FRAME_US : V_WIN_SZ_US;
            if (now >= V.winAt + endUs) {
                VWin w = V.win; uint32_t b = V.winPick;
                V.win = W_NONE; V.winPick = 0;
                if (b && !V.playing && !otherMacroBusy) {
                    if (w == W_MK) {
                        if (b & KICK_MASK) seqMKBurn(b);
                        else if (b == LP) seqStandHP(); else seqMKSeismo(b == HP ? (LP | MP) : MP);
                    } else {
                        if (b == LP) seqSeismoFeint();
                        else if (b & PUNCH_MASK) seqSeismo(b);
                        else seqBurn(b);
                    }
                    startSeq(now, 0, 0);
                }
            }
        }

        // ---- a sequence is playing
        if (V.playing && V.isCrouchHP && (rising & XBTN)) {
            // X cancels the cr.HP cancel: Level 1 in burnout, otherwise X goes to the game
            V.playing = false;
            if (V.burnout) { V.suppress |= XBTN; seqLevel1(); startSeq(now, 0, 0); }
        }
        if (V.playing) {
            V.suppress |= rising & ATTACK_MASK;   // never leak a press into the game mid-move
            if (runSeq(gp, now, rel)) return true;
        }

        // ---- new triggers (idle)
        bool idle = V.win == W_NONE && !V.playing && !otherMacroBusy && !(rel & RU);
        bool burnLock = now < V.burnLockUntil;    // 70f after a burning kick: only Macro 1-6
        if (V.hpFireAt && now >= V.hpFireAt && !idle) V.hpFireAt = 0;   // something else is going on: drop the queued cancel
        if (idle) {
            if (burnLock && !lv1Rising) {
                V.hpAt = 0; V.hpFireAt = 0;       // nothing arms or fires; your presses are plain hits
            } else if (V.hpFireAt && now >= V.hpFireAt) {          // the queued HP cancel starts now
                bool crouch = V.hpCrouch;
                V.hpFireAt = 0; V.hpAt = 0;
                if (crouch) seqCrouchHP(); else seqStandHP();
                startSeq(now, 0, 0);
                V.isCrouchHP = crouch;
                if (runSeq(gp, now, rel)) return true;
            } else if (lv1Rising) {                         // Macro 5 = qcf qcf, Macro 6 = qcb qcb (+ LK / HK), fixed on screen
                seqLevel1Screen((lv1Rising & m5) ? +1 : -1);
                startSeq(now, 0, 0);
                if (runSeq(gp, now, rel)) return true;
            } else if (V.burnout && (rising & XBTN)) {
                V.suppress |= XBTN;
                seqLevel1(); startSeq(now, 0, 0);
                if (runSeq(gp, now, rel)) return true;
            } else if ((rising & PUNCH_MASK) && detectSeismoMotion(now)) {
                // you did a seismo yourself: the punch goes out as it is and the seismo window opens
                V.win = W_SZ; V.winAt = now; V.winPick = 0;
                V.hpAt = 0;
            } else if ((r & ATTACK_MASK) == HP && !(phys & ATTACK_MASK & ~HP)) {
                if (V.hpAt && !V.hpFireAt && now - V.hpAt < (V.hpCrouch ? 12ULL : 18ULL) * FRAME_US) {
                    // second HP inside the window: hidden. st.HP: the cancel starts right away. cr.HP: at a random
                    // frame 8-12 from the first HP (a late second press: between its own frame and frame 12)
                    V.suppress |= HP;
                    if (!V.hpCrouch) V.hpFireAt = now;
                    else {
                        uint64_t lo = V.hpAt + 7ULL * FRAME_US, hi = V.hpAt + 11ULL * FRAME_US;
                        if (lo < now) lo = now;
                        V.hpFireAt = (hi > lo) ? lo + randomRange(0, (uint32_t)(hi - lo)) : now;
                    }
                } else if (!V.hpFireAt) {
                    // first HP: your own hit. Crouching (↓ or a down-diagonal), or standing / walking; an HP right
                    // after a motion or a qcb macro is a special move and arms nothing
                    bool crouch = (rel & RD) != 0;
                    if (crouch || (!recentDown(now) && now >= V.qcbBlockUntil)) { V.hpAt = now; V.hpCrouch = crouch; }
                    else V.hpAt = 0;
                }
            } else if ((r & ATTACK_MASK) == MK && !(phys & ATTACK_MASK & ~MK)) {
                V.win = W_MK; V.winAt = now; V.winPick = 0;   // your MK goes out as it is; 10f window (crouching 12f)
                V.winLen = (rel & RD) ? V_WIN_MK_US + 2ULL * FRAME_US : V_WIN_MK_US;
            }
        }

        if (V.burnout && now >= V.burnLockUntil) V.suppress |= (phys & XBTN);
        gp->state.buttons &= ~V.suppress;
        return false;
    }

    // ---------------- super spam (built-in profiles): the super comes out as usual; if its button
    // (Macro 3/4, or X in burnout) is still held when it ends, ↓ ↘ → + the same button repeats in the
    // same direction until you let go. Gap 0-2f, arrows 1.2-2f. The button goes in either on ↘ (held
    // to →) or on → (held into the next ↓ or N), 50/50 each time.
    bool spamArmed = false, spamOn = false, spamIsX = false, spamTailDown = false;
    uint32_t spamPin = 0, spamBtn = 0;
    uint8_t spamHz = 0;                       // absolute GAMEPAD_MASK_LEFT / RIGHT
    void spamReset() { spamArmed = false; spamOn = false; spamTailDown = false; }
    void armSpam(uint32_t pin, bool isX, uint32_t btn, uint8_t absDpad) {
        uint8_t hz = absDpad & (GAMEPAD_MASK_LEFT | GAMEPAD_MASK_RIGHT);
        if (!btn || !hz) return;
        spamArmed = true; spamOn = false; spamIsX = isX; spamPin = pin;
        spamBtn = btn; spamHz = hz; spamTailDown = false;
    }
    void armSpamFromSeq(uint32_t pin, bool isX) {   // the V sequence just built is the super
        if (V.seqLen <= 0) return;
        const VStep& last = V.seq[V.seqLen - 1];
        armSpam(pin, isX, last.btn & ATTACK_MASK, toAbs(last.dir));
    }
    void seqSpamRep() {
        vClear();
        auto ar = [](){ return randomRange(F10(12), F10(20)); };
        uint8_t dn = toRel(GAMEPAD_MASK_DOWN), dg = toRel(GAMEPAD_MASK_DOWN | spamHz), h = toRel(spamHz);
        if (!spamTailDown) vPush(N_, 0, randomRange(0, F10(20)));   // gap 0-2f
        if (coin()) {                          // button on ↘, held to →
            vPush(dn, 0, ar()); vPush(dg, spamBtn, ar()); vPush(h, spamBtn, ar());
            spamTailDown = false;
        } else {                               // button on →, held into the next ↓ or N
            vPush(dn, 0, ar()); vPush(dg, 0, ar()); vPush(h, spamBtn, ar());
            spamTailDown = coin();
            vPush(spamTailDown ? dn : N_, spamBtn, randomRange(F10(10), F10(20)));
        }
    }
    void spamNoCut() { for (int i = 0; i < V.seqLen; i++) V.seq[i].heCut = 0; }   // the spam loop keeps its arrows
    bool spamProcess(Gamepad* gp, uint64_t now, bool macroBusy) {
        if (!spamArmed && !spamOn) return false;
        bool held = spamIsX ? (gp->state.buttons & XBTN) != 0 : (gp->debouncedGpio & spamPin) != 0;
        if (!held) {                           // let go: stop right away
            if (spamOn) V.playing = false;
            spamReset();
            return false;
        }
        if (spamArmed && !spamOn) {
            if (V.playing || macroBusy) return false;   // the super is still coming out
            spamArmed = false; spamOn = true; spamTailDown = false;
            seqSpamRep(); startSeq(now, 0, 0); spamNoCut();
        }
        uint8_t prel = toRel(gp->state.dpad);
        if (runSeq(gp, now, prel)) return true;
        seqSpamRep(); startSeq(now, 0, 0); spamNoCut();     // next round
        return runSeq(gp, now, prel);
    }
    // ---------------- Sagat mode extras: burnout (turbo button) and X = Level 1
    bool prevTurbo = false;
    bool prevSagatX = false;
    void seqSagatLevel1(bool kick, bool back = false) {   // qcf qcf (qcb qcb for Lily) + LP/HP or LK/HK, 1.2-2.1f, human error
        vClear();
        uint8_t dg = back ? DB : DF, hz = back ? B_ : F_;
        for (uint8_t d : {D_, dg, hz, D_, dg}) vPush(d, 0, randomRange(F10(12), F10(21)));
        uint32_t dur = vBtn();
        vPush(hz, kick ? (coin() ? LK : HK) : (coin() ? LP : HP), dur);
    }

    // ---------------- shared pieces of the per-profile engines
    // relative code for an absolute direction (horiz: +1 right, -1 left, 0 none)
    uint8_t absDir(bool down, int horiz) {
        uint8_t r = down ? D_ : 0;
        if (horiz) r |= ((horiz > 0) == V.facingRight) ? F_ : B_;
        return r;
    }
    int sideH(bool fwd) { return (fwd == V.facingRight) ? +1 : -1; }   // screen direction of forward / back
    uint8_t cleanStick(Gamepad* gp) {                // the stick as the game sees it (SOCD cleaned)
        return runSOCDCleaner(Gamepad::resolveSOCDMode(gp->getOptions()), gp->state.dpad);
    }
    // start the sequence just built and run its first poll
    bool play(Gamepad* gp, uint64_t now, uint8_t prel) { startSeq(now, 0, 0); return runSeq(gp, now, prel); }
    // quarter circle on screen: ↓, then the down-diagonal and the horizontal toward h (+1 →, -1 ←) + btn
    void seqQuarter(int h, uint32_t btn, uint32_t lo10, uint32_t hi10) {
        vClear();
        vPush(absDir(true, 0), 0, randomRange(F10(lo10), F10(hi10)));
        vPush(absDir(true, h), 0, randomRange(F10(lo10), F10(hi10)));
        vPush(absDir(false, h), btn, vBtn());
    }
    // super, fixed on screen: h = +1 -> ↓ ↘ → ↓ ↘ →, h = -1 -> ↓ ↙ ← ↓ ↙ ←, then + btn
    void seqSuperFixed(int h, uint32_t btn) {
        superArrowsAndFinish({absDir(true, 0), absDir(true, h), absDir(false, h), absDir(true, 0), absDir(true, h)},
                             absDir(false, h), btn);
    }
    // Free button (Jamie, Chun-Li): of Macro 5 / Macro 6, the one that points away from the side you
    // face is useless as it is, so it does a side-following special instead. Facing right -> Macro 6 is free,
    // facing left -> Macro 5. Its move is chosen like a gear: hold an attack + press it (stays until changed).
    uint32_t freeOwnedPins = 0;                      // free-button presses the engine took: no plain macro until released
    uint32_t freePin(uint32_t m5, uint32_t m6) { return V.facingRight ? m6 : m5; }
    // Anti-fireball button = GPIO pin 21 (Manon, Cammy, Jamie), hidden from the game in those profiles.
    //   Manon: qcf + punch. Cammy: qcb + punch. Jamie: qcf + kick. All follow the side.
    //   Strength is chosen like a gear: hold an attack + press it: light = OD, medium = medium, heavy = heavy.
    constexpr uint32_t AFB_PIN_MASK = 1u << 21;
    int  afbSel = 1;                                 // 0 = OD, 1 = medium (default), 2 = heavy
    bool afbPrev = false, afbRise = false;           // afbRise: pin 21 was pressed this poll (set in preprocess)
    bool afbOn() { return pinkActive || cammyActive; }   // Manon, Cammy, Jamie
    // true = a sequence was built and must be played; false = nothing to play (no press, or a strength choice)
    bool afbFire(uint32_t phys) {
        if (!afbRise) return false;
        afbRise = false;
        uint32_t held = phys & ATTACK_MASK;
        if (held) {                                  // attack held + the button = choose the strength
            if (popcount32(held) == 1) afbSel = (held & (LP | LK)) ? 0 : (held & (MP | MK)) ? 1 : 2;
            V.suppress |= held;
            return false;
        }
        uint32_t btn = jamieActive ? (afbSel == 0 ? (MK | HK) : afbSel == 1 ? MK : HK)
                                   : (afbSel == 0 ? (MP | HP) : afbSel == 1 ? MP : HP);
        seqQuarter(sideH(pinkActive || jamieActive), btn, 12, 18);   // Manon / Jamie: qcf (forward); Cammy: qcb (back)
        return true;
    }
    // Per-engine input edges, plus the "diagonal jump, then a button = special in the air" rule.
    constexpr int ENG_STICK = 1, ENG_JUMP = 2;
    struct Eng {
        uint32_t prevPins = 0, prevPhys = 0;
        uint8_t  prevAbs = 0;
        int      jumpDir = 0;                        // +1 jumped ↗, -1 jumped ↖ (on screen)
        uint64_t jumpAt = 0, airAt = 0;
        uint32_t airBtn = 0;
        bool     air = false;                        // an air special is playing
        // this poll:
        uint32_t rise = 0, phys = 0, physRise = 0;
        uint8_t  prel = 0, ad = 0;
        bool     upRise = false;                     // you pressed up this poll (any jump); needs ENG_JUMP
        void read(Gamepad* gp, uint32_t pinMask, uint64_t now, int what) {
            uint32_t pins = (uint32_t)gp->debouncedGpio & pinMask;
            rise = pins & ~prevPins; prevPins = pins;
            phys = gp->state.buttons;
            physRise = phys & ~prevPhys; prevPhys = phys;
            V.suppress &= phys;                      // released buttons are no longer hidden
            prel = toRel(gp->state.dpad);
            if (what & ENG_STICK) ad = cleanStick(gp);
            upRise = false;
            if (what & ENG_JUMP) {                   // the moment you pressed a diagonal jump (↗ or ↖)
                upRise = (ad & GAMEPAD_MASK_UP) && !(prevAbs & GAMEPAD_MASK_UP);
                const uint8_t UR = GAMEPAD_MASK_UP | GAMEPAD_MASK_RIGHT, UL = GAMEPAD_MASK_UP | GAMEPAD_MASK_LEFT;
                bool urNow = (ad & UR) == UR && !(ad & GAMEPAD_MASK_DOWN), ulNow = (ad & UL) == UL && !(ad & GAMEPAD_MASK_DOWN);
                if (urNow && (prevAbs & UR) != UR) { jumpAt = now; jumpDir = +1; }
                if (ulNow && (prevAbs & UL) != UL) { jumpAt = now; jumpDir = -1; }
                prevAbs = ad;
            }
        }
        bool jumped(uint64_t now, uint64_t frames) const { return jumpAt && now - jumpAt <= frames * FRAME_US; }
        // qcf / qcb toward h + btn in the air
        bool startAir(Gamepad* gp, uint64_t now, uint32_t btn, int h) {
            airBtn = btn; airAt = now; jumpAt = 0;
            V.suppress |= btn;
            seqQuarter(h, airBtn, 12, 18);
            air = true;
            return play(gp, now, prel);
        }
        // OD: a second button within 2f joins the air special
        void airOD(uint32_t extra, uint64_t now) {
            if (air && extra && now - airAt <= 2ULL * FRAME_US && V.seqPos < V.seqLen) {
                airBtn |= extra; V.suppress |= extra;
                V.seq[V.seqLen - 1].btn = airBtn;
            }
        }
        // keep playing; when an air special ends the side follows the jump (↗ -> facing right, ↖ -> facing left)
        bool run(Gamepad* gp, uint64_t now) {
            bool wasAir = air;
            if (runSeq(gp, now, prel)) return true;
            if (wasAir && jumpDir) { V.facingRight = (jumpDir > 0); g_viperFacingRight = V.facingRight; }
            air = false;
            return false;
        }
    };

    // ---------------- pink profile: gear qcf / ↓HP x2, both-sides command grab
    uint32_t pinkGear = LP;
    bool cgrab = false, cgrabOD = false;
    uint32_t prevPinkPins = 0;
    bool pinkHKHold = false;
    bool pinkHoldAfter = false;
    void seqPinkGear(int side, uint8_t physRel) {   // side +1 = right version, -1 = left version
        vClear();
        if (pinkGear & KICK_MASK) {             // ↓, N, ↓ + kick (LK: OD with LK+MK while holding ↙ / ↘)
            uint32_t k = pinkGear;
            if (k == LK && (physRel & RD) && (physRel & (RF | RB))) k = LK | MK;
            vPush(D_, 0, randomRange(F10(13), F10(24)));
            vPush(N_, 0, randomRange(F10(10), F10(20)));
            vPush(D_, k, randomRange(F10(20), F10(40)));
            return;
        }
        if (pinkGear == HP) {                   // ↓ + HP twice
            vPush(D_, HP, randomRange(F10(30), F10(50)));
            vPush(D_, 0, randomRange(F10(100), F10(150)));
            vPush(D_, HP, randomRange(F10(30), F10(50)));
            return;
        }
        seqQuarter(side, pinkGear == MP ? (LK | MK) : LK, 12, 22);
    }
    void seqCommandGrab(uint32_t punch, bool holdingFwd) {   // half-circle back: → ↘ ↙ ← + punch
        vClear();
        auto ar = [](){ return randomRange(F10(12), F10(20)); };
        if (!holdingFwd) vPush(F_, 0, ar());      // smart start: already holding → = skip it
        vPush(DF, 0, ar());
        vPush(DB, 0, ar());
        vPush(B_, punch, vBtn());
    }
    // absolute stick history (for Manon's level 3 and Arjin's ↓↓HK)
    uint8_t  ahDir[16]; uint64_t ahT[16]; int ahHead = 0, ahCount = 0;
    uint8_t  prevAbs = 0xFF;
    uint64_t lvl3Until = 0;
    uint64_t manonHPAt = 0;                 // Manon: your last standing HP (X within 30f = Level 1)
    bool arjinHKHold = false;
    uint64_t arjinBlockUntil = 0, arjinPendUntil = 0, arjinHKStart = 0;
    bool arjinDownSwallow = false, arjinDownPass = false;
    uint32_t arjinPending = 0, arjinPendOut = 0;
    uint32_t prevPinkPhys = 0;
    void absPush(uint8_t d, uint64_t t) { ahHead = (ahHead + 1) & 15; ahDir[ahHead] = d; ahT[ahHead] = t; if (ahCount < 16) ahCount++; }
    bool absGet(int k, uint8_t& d, uint64_t& t) { if (k >= ahCount) return false; int i = (ahHead - k) & 15; d = ahDir[i]; t = ahT[i]; return true; }
    // your own qcf / qcb (↓ ↘ → or ↓ ↙ ←, a final ↗ / ↖ is fine) just before a punch
    bool sawQuarterCircle(uint64_t now) {
        const uint8_t U = GAMEPAD_MASK_UP, Dn = GAMEPAD_MASK_DOWN, Lf = GAMEPAD_MASK_LEFT, Rt = GAMEPAD_MASK_RIGHT;
        uint8_t d; uint64_t t; int k = 0;
        if (!absGet(0, d, t)) return false;
        if (d == 0) { if (now - t > 6ULL * FRAME_US) return false; k = 1; if (!absGet(1, d, t)) return false; }
        if (d == (U | Rt) || d == (U | Lf)) { k++; if (!absGet(k, d, t)) return false; }
        uint8_t h = d & (Lf | Rt);
        if (d != Rt && d != Lf) return false;
        uint64_t tEnd = now;
        if (!absGet(k + 1, d, t) || d != (Dn | h)) return false;
        if (!absGet(k + 2, d, t) || d != Dn) return false;
        return tEnd - t <= 25ULL * FRAME_US;
    }
    // Luke: ↓ (any length) → ↘/↙ → →/← (a ↗/↖ after it is fine); the punch within 8f of reaching →/←
    bool sawQuarterCircleLuke(uint64_t now) {
        // walks back through the stick history; tiny switch flickers (↙ ← ↙ ←, ↓ ↙ ↓ ↙) are tolerated
        const uint8_t U = GAMEPAD_MASK_UP, Dn = GAMEPAD_MASK_DOWN, Lf = GAMEPAD_MASK_LEFT, Rt = GAMEPAD_MASK_RIGHT;
        uint8_t d; uint64_t t; int k = 0;
        uint8_t h = 0; uint64_t tH = 0; bool diag = false;
        for (; k < 16 && absGet(k, d, t); k++) {
            if (!h) {                                    // after the forward: neutral / ↗ / ↖ allowed
                if (d == Rt || d == Lf) { h = d; tH = t; continue; }
                if (d == 0 || d == (U | Rt) || d == (U | Lf)) continue;
                return false;
            }
            if (d == h) { if (!diag) tH = t; continue; }  // first time you reached →/←
            if (d == (Dn | h)) { diag = true; continue; }
            if (d == Dn && diag) return now - tH <= 8ULL * FRAME_US;
            return false;
        }
        return false;
    }
    // ↓ ↓ (two separate presses within 12f) then HK within 7f of the second ↓
    bool sawDownDown(uint64_t now) {
        const uint8_t Dn = GAMEPAD_MASK_DOWN;
        uint64_t t2 = 0, t1 = 0; uint8_t d; uint64_t t; int k = 0; bool gap = false;
        for (; absGet(k, d, t); k++) { if (d & Dn) { t2 = t; break; } }
        if (!t2) return false;
        // walk back over the rest of this ↓ press, then a non-↓ gap, then the first ↓
        for (k++; absGet(k, d, t); k++) {
            if (!(d & Dn)) { gap = true; continue; }
            if (gap) { t1 = t; uint8_t d2; uint64_t tt; while (absGet(k + 1, d2, tt) && (d2 & Dn)) { t1 = tt; k++; } break; }
            t2 = t;
        }
        if (!t1 || !gap) return false;
        return (t2 - t1) <= 12ULL * FRAME_US && (now - t2) <= 7ULL * FRAME_US;
    }
    bool pinkProcess(Gamepad* gp, uint64_t now, uint32_t m1, uint32_t m2, uint32_t m3, uint32_t m4,
                     uint32_t m5, uint32_t m6, bool macroBusy) {
        bool manon = pinkActive;
        uint32_t mask = m3 | m4 | m5 | m6 | (manon ? (m1 | m2) : 0);
        uint32_t pins = (uint32_t)gp->debouncedGpio & mask;
        uint32_t rise = pins & ~prevPinkPins;
        prevPinkPins = pins;
        uint32_t phys = gp->state.buttons;
        uint32_t physRise = phys & ~prevPinkPhys;
        prevPinkPhys = phys;
        uint8_t dpad = gp->state.dpad;
        if (manon) dpad = cleanStick(gp);   // Manon: the stick as the game sees it
        if (dpad != prevAbs) { absPush(dpad, now); }
        bool downRise = (dpad & GAMEPAD_MASK_DOWN) && !(prevAbs != 0xFF && (prevAbs & GAMEPAD_MASK_DOWN));
        prevAbs = dpad;
        V.suppress &= phys;                     // released buttons are no longer hidden
        uint8_t prel = toRel(dpad);
        // Manon: your own qcf / qcb + a punch or a kick opens the 30f Level 3 window (Macro 3 / 4 = qcf qcf / qcb qcb + LP / HP)
        if (manon && (physRise & (PUNCH_MASK | KICK_MASK)) && sawQuarterCircle(now)) lvl3Until = now + 30ULL * FRAME_US;
        // Manon: your HP standing or walking (no ↑ / ↓), not during a macro
        if (manon && (physRise & HP) && !V.playing && !(dpad & (GAMEPAD_MASK_UP | GAMEPAD_MASK_DOWN))) manonHPAt = now;
        if (arjinActive) {                      // Arjin: your ↓ ↓ + HK keeps HK held until ↓ or a punch
            // ↓ before frame 18 from the HK: ignored (not sent, does not stop the hold) until you let go of it
            if (arjinHKHold) {
                bool dn = (dpad & GAMEPAD_MASK_DOWN) != 0;
                if (arjinDownPass && !dn) arjinDownPass = false;   // the ↓ of your motion passes until released
                if (now < arjinHKStart + 18ULL * FRAME_US) { if (dn && !arjinDownPass) arjinDownSwallow = true; }
                else if (downRise && !arjinDownSwallow) arjinHKHold = false;   // a fresh ↓ after 18f stops it
                if (!dn) arjinDownSwallow = false;
                if (arjinHKHold && arjinDownSwallow) gp->state.dpad &= ~GAMEPAD_MASK_DOWN;
                if (arjinHKHold && rise) arjinHKHold = false;
            } else if (arjinDownSwallow) {
                if (dpad & GAMEPAD_MASK_DOWN) gp->state.dpad &= ~GAMEPAD_MASK_DOWN;
                else arjinDownSwallow = false;
            }
            if (now < arjinBlockUntil) {        // punches ignored until frame 29 from the HK, then sent
                arjinPending |= physRise & PUNCH_MASK;
                gp->state.buttons &= ~PUNCH_MASK;
                if (arjinHKHold) gp->state.buttons |= HK;
            } else {
                if (arjinPending) {             // frame 29: the punch you pressed comes out now
                    arjinHKHold = false;
                    arjinPendOut = arjinPending; arjinPending = 0;
                    arjinPendUntil = now + 3ULL * FRAME_US;
                }
                if (now < arjinPendUntil) gp->state.buttons |= arjinPendOut;
                if (arjinHKHold) {
                    if (physRise & PUNCH_MASK) arjinHKHold = false;
                    else gp->state.buttons |= HK;
                } else if ((physRise & HK) && !V.playing && sawDownDown(now)) {
                    arjinHKHold = true;
                    arjinHKStart = now;
                    arjinDownSwallow = false;
                    arjinDownPass = (dpad & GAMEPAD_MASK_DOWN) != 0;
                    arjinBlockUntil = now + 29ULL * FRAME_US;
                    arjinPending = 0;
                    gp->state.buttons &= ~PUNCH_MASK;
                }
            }
        }
        if (V.playing) {
            if (cgrab) {
                if ((phys & HP) && !(V.suppress & HP)) { cgrabOD = true; V.suppress |= HP; }
                if (cgrabOD && V.seqPos < V.seqLen - 1) V.seq[V.seqLen - 1].btn = LP | HP;   // OD window: until the last →
            }
            if (runSeq(gp, now, prel)) return true;
            cgrab = false;
        }
        // Manon: standing HP, then X within 30f = Level 1 (qcf qcf + LK / HK, follows the side), as in burnout
        if (manon && !macroBusy && (physRise & XBTN) && !g_sagatBurnout && manonHPAt && now - manonHPAt <= 30ULL * FRAME_US) {
            manonHPAt = 0;
            V.suppress |= XBTN;                 // X stays hidden until you let go of it
            seqSagatLevel1(true);
            cgrab = false;
            return play(gp, now, prel);
        }
        if (manon && !macroBusy && afbFire(phys)) {   // pin 21: anti-fireball (qcf + punch)
            cgrab = false;
            return play(gp, now, prel);
        }
        if (!rise || macroBusy) return false;
        if (manon && (rise & (m1 | m2))) {
            uint32_t held = phys & ATTACK_MASK;
            if (held) {                         // attack held + gear button = choose the gear (no HK)
                if (popcount32(held) == 1 && held != HK) pinkGear = held;
                V.suppress |= held;
                return false;
            }
            seqPinkGear((rise & m1) ? +1 : -1, prel);
            cgrab = false;
            return play(gp, now, prel);
        }
        if (rise & (m3 | m4)) {                 // supers
            if (arjinActive || now < lvl3Until) {
                seqSuperFixed((rise & m3) ? +1 : -1, coin() ? LP : HP);   // fixed on screen: Macro 3 = qcf qcf, Macro 4 = qcb qcb
                armSpamFromSeq(rise & (m3 | m4), false);
                lvl3Until = 0;
                cgrab = false;
                return play(gp, now, prel);
            }
            return false;                       // Manon without the window: normal super macro
        }
        if (rise & (m5 | m6)) {
            // Arjin: Macro 5 = LP, Macro 6 = HP. Manon: the other way round (Macro 5 = HP, Macro 6 = LP).
            seqCommandGrab((((rise & m5) != 0) != manon) ? LP : HP, (prel & RF) && !(prel & RD));
            cgrab = true; cgrabOD = (phys & HP) != 0;
            if (cgrabOD) { V.suppress |= HP; V.seq[V.seqLen - 1].btn = LP | HP; }
            return play(gp, now, prel);
        }
        return false;
    }
    // ---------------- Luke: your qcf / qcb, then a punch within 8f. The first 6f are yours (let go = nothing);
    // still held at 6f -> held until a random 18.2-19.8f from your press, then released (even if you still hold it)
    uint32_t lukeHoldBtn = 0, lukeSuppress = 0, prevLukePhys = 0;
    uint64_t lukeStart = 0, lukeEnd = 0;
    bool lukeTaken = false;
    void lukeProcess(Gamepad* gp, uint64_t now) {
        // the stick as the game sees it (SOCD cleaned: e.g. ↓ held + ↑ pressed = no vertical -> ←)
        uint8_t dpad = cleanStick(gp);
        if (dpad != prevAbs) absPush(dpad, now);
        prevAbs = dpad;
        uint32_t phys = gp->state.buttons;
        uint32_t rise = phys & ~prevLukePhys;
        prevLukePhys = phys;
        lukeSuppress &= phys;                   // let go = usable again
        if (lukeHoldBtn) {
            if (!lukeTaken) {
                if (!(phys & lukeHoldBtn)) lukeHoldBtn = 0;                 // let go before 6f: nothing
                else if (now >= lukeStart + 6ULL * FRAME_US) lukeTaken = true;
            }
            if (lukeTaken) {
                if (now < lukeEnd) gp->state.buttons |= lukeHoldBtn;
                else { lukeSuppress |= phys & lukeHoldBtn; lukeHoldBtn = 0; lukeTaken = false; }
            }
        }
        if (!lukeHoldBtn && (rise & PUNCH_MASK) && sawQuarterCircleLuke(now)) {
            lukeHoldBtn = rise & PUNCH_MASK;
            lukeStart = now;
            lukeEnd = now + (lukeHoldBtn == MP ? randomRange(F10(240), F10(250))     // MP version: 24-25f
                                               : randomRange(F10(182), F10(198)));   // LP / HP: 18.2-19.8f
            lukeTaken = false;
            lukeSuppress &= ~lukeHoldBtn;
        }
        gp->state.buttons &= ~lukeSuppress;
    }
    // ---------------- Lily: Macro 1/2 = Sagat gear, Macro 5/6 = SPD (← ↙ ↓ → ↗ + LP / HP, fixed on screen),
    // a punch held or pressed during it = OD (two punches); Macro 3 = qcb qcb + P, Macro 4 = qcf qcf + P
    Eng eLily;
    uint32_t lilyPunch = 0;
    bool lilySpd = false, lilyOD = false;
    uint32_t lilyODBtn(uint32_t extra) {
        uint32_t other = extra & ~lilyPunch;
        if (!other) other = (lilyPunch == LP) ? HP : LP;
        return lilyPunch | lowestBit(other);
    }
    bool lilyProcess(Gamepad* gp, uint64_t now, uint32_t m1, uint32_t m2, uint32_t m3, uint32_t m4, bool macroBusy) {
        Eng& e = eLily;
        e.read(gp, m1 | m2 | m3 | m4, now, 0);
        uint32_t rise = e.rise, phys = e.phys;
        uint8_t prel = e.prel;
        if (V.playing) {
            if (lilySpd && !lilyOD && V.seqPos < V.seqLen - 1) {
                uint32_t p = phys & PUNCH_MASK & ~V.suppress;
                if (p) { lilyOD = true; V.suppress |= p; V.seq[V.seqLen - 1].btn = lilyODBtn(p); }
            }
            if (runSeq(gp, now, prel)) return true;
            lilySpd = false;
        }
        if (!rise || macroBusy) return false;
        if (rise & (m1 | m2)) {               // m1 / m2 here = Macro 5 / 6
            lilyPunch = (rise & m1) ? LP : HP;
            vClear();
            auto ar = [](){ return randomRange(F10(12), F10(20)); };
            // fixed on screen (not the side switch): ← ↙ ↓ → ↗
            vPush(absDir(false, -1), 0, ar()); vPush(absDir(true, -1), 0, ar()); vPush(absDir(true, 0), 0, ar()); vPush(absDir(false, +1), 0, ar());
            vPush((uint8_t)(RU | absDir(false, +1)), lilyPunch, vBtn());
            uint32_t held = phys & PUNCH_MASK;
            lilyOD = held != 0;
            if (lilyOD) { V.suppress |= held; V.seq[V.seqLen - 1].btn = lilyODBtn(held); }
            lilySpd = true;
            return play(gp, now, prel);
        }
        if (rise & (m3 | m4)) {
            // fixed on screen (not the side switch): Macro 3 = ↓↙←↓↙←, Macro 4 = ↓↘→↓↘→ (+ LP / HP)
            seqSuperFixed((rise & m3) ? -1 : +1, coin() ? LP : HP);
            armSpamFromSeq(rise & (m3 | m4), false);
            lilySpd = false;
            return play(gp, now, prel);
        }
        return false;
    }
    // ---------------- Cammy: diagonal jump, then a kick within 25f of the jump = qcb + that kick
    // (another kick within 2f of the first = OD, both kicks); Macro 3 = qcf qcf + P, Macro 4 = qcb qcb + P
    Eng eCammy;
    uint32_t cammyFree = LP;                        // Jamie: punch chosen for the free button (LP / MP = command grab, HP = OD)
    // All profiles: after your qcf / qcb, a down-diagonal slip (↘ after →, ↙ after ←) that starts within 2f
    // of reaching the forward is sent as the forward; held longer than 2f, the diagonal goes through.
    int cqfStage = 0; uint8_t cqfH = 0, cqfPrev = 0xFF, cqfFwdH = 0;
    uint64_t cqfFwdAt = 0, cqfDiagAt = 0;
    void cammyCleanQcf(Gamepad* gp, uint64_t now) {
        const uint8_t Dn = GAMEPAD_MASK_DOWN, Lf = GAMEPAD_MASK_LEFT, Rt = GAMEPAD_MASK_RIGHT;
        uint8_t ad = cleanStick(gp);
        if (ad != cqfPrev) {
            if (ad == Dn) cqfStage = 1;
            else if ((ad == (Dn | Lf) || ad == (Dn | Rt)) && cqfStage >= 1) { cqfStage = 2; cqfH = ad & (Lf | Rt); }
            else if (cqfStage == 2 && ad == cqfH) { cqfFwdAt = now; cqfFwdH = cqfH; cqfStage = 0; }   // qcf / qcb done
            else if (!(ad == (Dn | Lf) || ad == (Dn | Rt))) cqfStage = 0;
            cqfPrev = ad;
        }
        if (!cqfFwdAt) return;
        if (ad == (Dn | cqfFwdH)) {
            if (!cqfDiagAt) {
                if (now - cqfFwdAt <= 2ULL * FRAME_US) cqfDiagAt = now;   // the slip starts within 2f of the forward
                else { cqfFwdAt = 0; return; }
            }
            if (now - cqfDiagAt <= 2ULL * FRAME_US) gp->state.dpad = cqfFwdH;   // send the forward instead
            else { cqfFwdAt = 0; cqfDiagAt = 0; }                               // held > 2f: the diagonal is real
        } else {
            cqfDiagAt = 0;
            if (ad != cqfFwdH) cqfFwdAt = 0;
        }
    }
    // Free button (Jamie only): LP / MP = command grab (+ LK), HP = OD command grab (LK+MK). Cammy: plain OD DP on both.
    bool cammyProcess(Gamepad* gp, uint64_t now, uint32_t m3, uint32_t m4, uint32_t m5, uint32_t m6, bool macroBusy) {
        Eng& e = eCammy;
        e.read(gp, m3 | m4 | m5 | m6, now, ENG_STICK | ENG_JUMP);
        uint32_t phys = e.phys, physRise = e.physRise;
        uint32_t freeRise = jamieActive ? (e.rise & freePin(m5, m6)) : 0;
        freeOwnedPins = (freeOwnedPins | freeRise) & (uint32_t)gp->debouncedGpio;
        if (V.playing) {
            e.airOD(physRise & KICK_MASK, now);
            if (e.run(gp, now)) return true;
        }
        if (macroBusy) return false;
        // diagonal jump, then a kick within 25f: qcb away from the jump (↗ -> ↓ ↙ ←, ↖ -> ↓ ↘ →) + that kick
        if ((physRise & KICK_MASK) && e.jumped(now, 25)) return e.startAir(gp, now, physRise & KICK_MASK, -e.jumpDir);
        if (afbFire(phys)) {                        // pin 21: anti-fireball (Cammy qcb + punch, Jamie qcf + kick)
            e.air = false;
            return play(gp, now, e.prel);
        }
        if (freeRise) {                             // Jamie: command grab on the free button
            uint32_t held = phys & ATTACK_MASK;
            if (held) {                             // punch held + free button = choose it (HP = OD)
                if (popcount32(held) == 1 && (held & PUNCH_MASK)) cammyFree = held;
                V.suppress |= held;
                return false;
            }
            seqCommandGrab(cammyFree == HP ? (LK | MK) : LK, (e.prel & RF) && !(e.prel & RD));
            e.air = false;
            return play(gp, now, e.prel);
        }
        if (e.rise & (m3 | m4)) {
            // fixed on screen (not the side switch): Macro 3 = ↓ ↘ → ↓ ↘ →, Macro 4 = ↓ ↙ ← ↓ ↙ ← (+ LP / HP)
            seqSuperFixed((e.rise & m3) ? +1 : -1, coin() ? LP : HP);
            armSpamFromSeq(e.rise & (m3 | m4), false);
            e.air = false;
            return play(gp, now, e.prel);
        }
        return false;
    }
    // ---------------- Aki: Macro 1 qcf+HP, 2 qcb+HP, 3 qcf qcf+HP, 4 qcb qcb+HP, 5 qcf qcf+kick, 6 qcb qcb+kick
    // (kick = LK / HK random; macros fixed on screen, only X follows the side switch); your ↓+HK then X within 25f = qcf qcf + kick
    Eng eAki;
    uint64_t akiHKAt = 0;
    bool akiProcess(Gamepad* gp, uint64_t now, const uint32_t* m, bool macroBusy) {
        Eng& e = eAki;
        e.read(gp, m[0] | m[1] | m[2] | m[3] | m[4] | m[5], now, ENG_STICK);
        uint32_t rise = e.rise;
        // Aki: your ↓ + HK. Ingrid: your HK standing or walking (no ↑ / ↓).
        bool hkOk = ingridActive ? !(e.ad & (GAMEPAD_MASK_UP | GAMEPAD_MASK_DOWN)) : (e.ad & GAMEPAD_MASK_DOWN) != 0;
        if ((e.physRise & HK) && hkOk) akiHKAt = now;
        if (V.playing) {
            if (runSeq(gp, now, e.prel)) return true;
        }
        if (macroBusy) return false;
        if ((e.physRise & XBTN) && !g_sagatBurnout && akiHKAt && now - akiHKAt <= (ingridActive ? 44ULL : 25ULL) * FRAME_US) {   // Ingrid 44f, Aki 25f
            akiHKAt = 0;
            V.suppress |= XBTN;                 // X stays hidden until you let go of it
            seqSagatLevel1(true);               // qcf qcf + LK / HK
            return play(gp, now, e.prel);
        }
        // macros are fixed on screen (not the side switch): "right" = toward →, "left" = toward ←
        if (rise & (m[0] | m[1])) {             // ↓ ↘ → + HP / ↓ ↙ ← + HP (Ingrid: + LK)
            seqQuarter((rise & m[0]) ? +1 : -1, ingridActive ? LK : HP, 12, 20);
        } else if (rise & (m[2] | m[3])) {      // supers: ↓ ↘ → ↓ ↘ → / ↓ ↙ ← ↓ ↙ ← + HP (Ingrid: LP / HP random)
            seqSuperFixed((rise & m[2]) ? +1 : -1, ingridActive ? (coin() ? LP : HP) : HP);
            armSpamFromSeq(rise & (m[2] | m[3]), false);
        } else if (rise & (m[4] | m[5])) {      // ↓ ↘ → ↓ ↘ → / ↓ ↙ ← ↓ ↙ ← + LK / HK
            seqSuperFixed((rise & m[4]) ? +1 : -1, coin() ? LK : HK);
        } else return false;
        return play(gp, now, e.prel);
    }
    // ---------------- pin 27 rule (every profile): press pin 27, then HK within 13f -> on frame 14 from the
    // pin 27 press: ↓ + HK (Arjin: ↓ + MP) for 2-5f, pin 27 released even if held; then full control back.
    // The HK pressed in the window never reaches the game (hidden until you let go of it).
    uint64_t p27At = 0, p27End = 0;
    bool p27Prev = false, p27Armed = false, p27Firing = false;
    uint32_t p27HKHide = 0, prevP27Phys = 0;
    uint8_t dpadMaskForAction(GpioAction a) {
        switch (a) {
            case GpioAction::BUTTON_PRESS_UP:    return GAMEPAD_MASK_UP;
            case GpioAction::BUTTON_PRESS_DOWN:  return GAMEPAD_MASK_DOWN;
            case GpioAction::BUTTON_PRESS_LEFT:  return GAMEPAD_MASK_LEFT;
            case GpioAction::BUTTON_PRESS_RIGHT: return GAMEPAD_MASK_RIGHT;
            default: return 0;
        }
    }
    uint64_t p27ArrowEnd = 0;
    uint32_t buttonMaskForAction(GpioAction a) {
        switch (a) {
            case GpioAction::BUTTON_PRESS_B1: return GAMEPAD_MASK_B1;
            case GpioAction::BUTTON_PRESS_B2: return GAMEPAD_MASK_B2;
            case GpioAction::BUTTON_PRESS_B3: return GAMEPAD_MASK_B3;
            case GpioAction::BUTTON_PRESS_B4: return GAMEPAD_MASK_B4;
            case GpioAction::BUTTON_PRESS_L1: return GAMEPAD_MASK_L1;
            case GpioAction::BUTTON_PRESS_R1: return GAMEPAD_MASK_R1;
            case GpioAction::BUTTON_PRESS_L2: return GAMEPAD_MASK_L2;
            case GpioAction::BUTTON_PRESS_R2: return GAMEPAD_MASK_R2;
            case GpioAction::BUTTON_PRESS_S1: return GAMEPAD_MASK_S1;
            case GpioAction::BUTTON_PRESS_S2: return GAMEPAD_MASK_S2;
            case GpioAction::BUTTON_PRESS_A1: return GAMEPAD_MASK_A1;
            case GpioAction::BUTTON_PRESS_A2: return GAMEPAD_MASK_A2;
            case GpioAction::BUTTON_PRESS_L3: return GAMEPAD_MASK_L3;
            case GpioAction::BUTTON_PRESS_R3: return GAMEPAD_MASK_R3;
            default: return 0;
        }
    }
    // returns true while it owns the output (the ↓ + HK)
    bool pin27Process(Gamepad* gp, uint64_t now) {
        bool p = (gp->debouncedGpio & (1u << 27)) != 0;
        uint32_t phys = gp->state.buttons;
        uint32_t rise = phys & ~prevP27Phys;
        prevP27Phys = phys;
        if (p && !p27Prev && !p27Firing) { p27At = now; p27Armed = false; }
        p27Prev = p;
        p27HKHide &= phys;                          // let go of HK = it works again
        if (p27At && !p27Firing) {
            if (now - p27At < 13ULL * FRAME_US) {
                if (rise & HK) { p27Armed = true; p27HKHide |= HK; }
            } else if (p27Armed) {                  // frame 14
                p27Firing = true;
                { uint32_t d = randomRange(F10(20), F10(50)); p27End = now + d; p27ArrowEnd = p27End - arrowCut(d); }
            } else {
                p27At = 0;                          // nothing in 13f: full control
            }
        }
        gp->state.buttons &= ~p27HKHide;
        if (p27Firing) {
            if (now < p27End) {
                uint32_t pinBtn = buttonMaskForAction(Storage::getInstance().getProfilePinMappings()[27].action);
                if (now < p27ArrowEnd) gp->state.dpad = GAMEPAD_MASK_DOWN;
                else gp->state.dpad &= ~dpadMaskForAction(Storage::getInstance().getProfilePinMappings()[27].action);
                gp->state.buttons = (gp->state.buttons & ~(ATTACK_MASK | pinBtn)) | (arjinActive ? MP : HK);
                return true;
            }
            p27Firing = false; p27Armed = false; p27At = 0;
        }
        return false;
    }
    // ---------------- Chun-Li
    // Macro 1 / 2 = gear: ↓ N ↓ + gear kick (MK by default; hold a kick + gear button to change it).
    //   Crouching (↓ or a down-diagonal held): N, ↓, N, ↓ + LK+MK (OD). Macro 1 -> facing right, Macro 2 -> facing left.
    // Macro 3 = ↓↙←↓↙← + LK/HK, Macro 4 = ↓↘→↓↘→ + LK/HK, Macro 5 = ↓↘→↓↘→ + LP/HP, Macro 6 = ↓↙←↓↙← + LP/HP (fixed).
    // Dive: diagonal jump, then a kick within 23f = qcf toward the jump + that kick (two kicks within 2f = OD);
    //   after it the side follows the jump (↗ -> facing right, ↖ -> facing left).
    Eng eChun;
    uint32_t chunGear = MK;
    uint32_t chunFree = LK;                         // kick chosen for the free button: LK / MK = qcb + HK, HK = qcb + LK+MK (OD)
    // qcb + HP cancels (follow the side); the second press never reaches the game:
    //   HP, then HP again within 12f          -> starts at a random frame 8-12 from the first HP
    //   crouch HP, then HP again within 18f   -> frame 18-23
    //   crouch MP, then MP again within 12f   -> frame 8-12
    //   standing / → / ← + HK, then HK again within 18f -> frame 18-23
    // (a late second press: between its own frame and the end of the range)
    uint64_t chunHPAt = 0, chunHPFireAt = 0, chunMPAt = 0, chunHKAt = 0;
    bool     chunHPCrouch = false;                  // first HP was crouching
    JumpGuard chunGuard;
    void chunArm(uint64_t first, uint64_t loFrame, uint64_t hiFrame, uint64_t now) {
        uint64_t lo = first + (loFrame - 1) * FRAME_US, hi = first + (hiFrame - 1) * FRAME_US;
        if (lo < now) lo = now;
        chunHPFireAt = (hi > lo) ? lo + randomRange(0, (uint32_t)(hi - lo)) : now;
    }
    bool chunProcess(Gamepad* gp, uint64_t now, const uint32_t* m, bool macroBusy) {
        Eng& e = eChun;
        e.read(gp, m[0] | m[1] | m[2] | m[3] | m[4] | m[5], now, ENG_STICK | ENG_JUMP);
        uint32_t rise = e.rise, phys = e.phys, physRise = e.physRise;
        uint8_t ad = e.ad;
        if (e.upRise) chunGuard.arm(now);
        if (V.playing) {
            e.airOD(physRise & KICK_MASK, now);
            if (e.run(gp, now)) return true;
        }
        if (macroBusy) return false;
        uint32_t cr = physRise;                     // presses that count for the double-press cancels
        if ((cr & CHUN_CANCEL_BTNS) && chunGuard.take(now)) {   // protected press after a jump: a normal hit
            cr &= ~CHUN_CANCEL_BTNS;
            chunHPAt = 0; chunMPAt = 0; chunHKAt = 0; chunHPFireAt = 0;
        }
        if (cr & HP) {
            if (chunHPAt && !chunHPFireAt && now - chunHPAt < (chunHPCrouch ? 18ULL : 12ULL) * FRAME_US) {   // second HP inside the window
                V.suppress |= HP;
                if (chunHPCrouch) chunArm(chunHPAt, 18, 23, now); else chunArm(chunHPAt, 8, 12, now);
            } else if (!chunHPFireAt) {
                chunHPAt = now;                     // first HP: goes out normally
                chunHPCrouch = (ad & GAMEPAD_MASK_DOWN) != 0;
            }
        }
        if (cr & MP) {
            if (chunMPAt && !chunHPFireAt && now - chunMPAt < 12ULL * FRAME_US) {   // second MP inside 12f
                V.suppress |= MP;
                chunArm(chunMPAt, 8, 12, now);
                chunMPAt = 0;
            } else {
                chunMPAt = (ad & GAMEPAD_MASK_DOWN) ? now : 0;   // only a crouching MP starts it
            }
        }
        if (cr & HK) {
            if (chunHKAt && !chunHPFireAt && now - chunHKAt < 18ULL * FRAME_US) {   // second HK inside 18f
                V.suppress |= HK;
                chunArm(chunHKAt, 18, 23, now);
                chunHKAt = 0;
            } else {
                bool standing = !(ad & (GAMEPAD_MASK_UP | GAMEPAD_MASK_DOWN));
                chunHKAt = standing ? now : 0;      // standing HK, → + HK or ← + HK starts it
            }
        }
        if (chunHPFireAt && now >= chunHPFireAt) {   // qcb + HP (follows the side)
            chunHPFireAt = 0; chunHPAt = 0; chunMPAt = 0; chunHKAt = 0;
            seqQuarter(sideH(false), HP, 12, 20);
            e.air = false;
            return play(gp, now, e.prel);
        }
        // dive: diagonal jump, then a kick within 23f = qcf toward the jump (↗ -> ↓ ↘ →, ↖ -> ↓ ↙ ←) + that kick
        if ((physRise & KICK_MASK) && e.jumped(now, 23)) return e.startAir(gp, now, physRise & KICK_MASK, e.jumpDir);
        if (!rise) return false;
        if (rise & (m[0] | m[1])) {
            uint32_t held = phys & KICK_MASK;
            if (held && popcount32(held) == 1) {    // kick held + gear button = choose the gear kick
                chunGear = held; V.suppress |= held;
                return false;
            }
            bool crouch = (ad & GAMEPAD_MASK_DOWN) != 0;
            bool walking = !crouch && (ad & (GAMEPAD_MASK_LEFT | GAMEPAD_MASK_RIGHT)) != 0;   // walking ← / →: N first, no OD
            vClear();
            if (crouch || walking) vPush(N_, 0, randomRange(F10(10), F10(20)));
            vPush(D_, 0, randomRange(F10(13), F10(24)));
            vPush(N_, 0, randomRange(F10(10), F10(20)));
            vPush(D_, crouch ? (LK | MK) : chunGear, randomRange(F10(20), F10(40)));
            V.facingRight = (rise & m[0]) != 0;     // Macro 1 -> facing right, Macro 2 -> facing left
            g_viperFacingRight = V.facingRight;
            e.air = false;
            return play(gp, now, e.prel);
        }
        if (rise & freePin(m[4], m[5])) {           // free button: qcb + kick (follows the side)
            uint32_t held = phys & ATTACK_MASK;
            if (held) {                             // kick held + free button = choose its move
                if (popcount32(held) == 1 && (held & KICK_MASK)) chunFree = held;
                V.suppress |= held;
                return false;
            }
            seqQuarter(sideH(false), chunFree == HK ? (LK | MK) : HK, 12, 18);
            e.air = false;
            return play(gp, now, e.prel);
        }
        if      (rise & m[2]) seqSuperFixed(-1, coin() ? LK : HK);
        else if (rise & m[3]) seqSuperFixed(+1, coin() ? LK : HK);
        else if (rise & m[4]) seqSuperFixed(+1, coin() ? LP : HP);
        else if (rise & m[5]) seqSuperFixed(-1, coin() ? LP : HP);
        else return false;
        if (rise & (m[2] | m[3])) armSpamFromSeq(rise & (m[2] | m[3]), false);   // supers repeat while held
        e.air = false;
        return play(gp, now, e.prel);
    }
    // ---------------- Kimberly (all six macros run here)
    // Macro 1/2 = anti-air gear (Macro 1 -> facing right, Macro 2 -> facing left):
    //   punch gear (any punch): ↓+HP 2-4f, ↓ and ↘ 2.5-4f each, → + LK 4-5f, → alone 4-6f, then → + LP 3-4f, then LP 8-10f more (stick yours);
    //   from the LK on, any attack you press cancels the rest and goes out as your own
    //   kick gear: qcb + that kick
    // Macro 3 = qcf qcf + LP/HP, Macro 4 = qcb qcb + LP/HP, Macro 5 = qcf qcf + LK/HK, Macro 6 = qcb qcb + LK/HK (fixed on screen)
    // Air: diagonal jump, then a punch within 25f = qcf toward the jump + that punch;
    //      a kick within 20f = qcb away from the jump + that kick (second button of the same kind within 2f = OD)
    Eng eKim;
    uint32_t kimGear = HP;
    bool     kimAirPunch = false;                   // the air special playing is the punch one
    bool     kimPunchGear = false;                  // the punch gear is playing (an attack press after its qcf cancels the rest)
    constexpr int KIM_GEAR_LK_STEP = 3;             // ↓+HP, ↓, ↘, then → + LK
    bool kimProcess(Gamepad* gp, uint64_t now, const uint32_t* m, bool macroBusy) {
        Eng& e = eKim;
        e.read(gp, m[0] | m[1] | m[2] | m[3] | m[4] | m[5], now, ENG_STICK | ENG_JUMP);
        uint32_t rise = e.rise, phys = e.phys;
        if (!V.playing) kimPunchGear = false;
        if (kimPunchGear && V.seqPos >= KIM_GEAR_LK_STEP && (e.physRise & ATTACK_MASK)) {
            V.playing = false;                      // your own attack after the qcf + LK: drop the rest, the button goes out
            kimPunchGear = false;
            return false;
        }
        if (V.playing) {
            e.airOD(e.physRise & (kimAirPunch ? PUNCH_MASK : KICK_MASK), now);
            if (e.run(gp, now)) return true;
        }
        if (macroBusy) return false;
        uint32_t ap = e.physRise & PUNCH_MASK, ak = e.physRise & KICK_MASK;
        bool airP = ap && e.jumped(now, 25);          // punch: 25f window
        bool airK = !airP && ak && e.jumped(now, 20); // kick: 20f window
        if (airP || airK) {
            kimAirPunch = airP;                     // punch: qcf toward the jump; kick: qcb away from it
            return e.startAir(gp, now, airP ? ap : ak, airP ? e.jumpDir : -e.jumpDir);
        }
        if (!rise) return false;
        if (rise & (m[0] | m[1])) {
            uint32_t held = phys & ATTACK_MASK;
            if (held) {                             // attack held + gear button = choose the gear
                if (popcount32(held) == 1) kimGear = held;
                V.suppress |= held;
                return false;
            }
            int side = (rise & m[0]) ? +1 : -1;
            V.facingRight = (side > 0);             // Macro 1 -> facing right, Macro 2 -> facing left
            g_viperFacingRight = V.facingRight;
            if (kimGear & KICK_MASK) {              // qcb + the gear kick
                seqQuarter(-side, kimGear, 12, 22);
            } else {                                // any punch gear
                vClear();
                vPush(absDir(true, 0), HP, randomRange(F10(20), F10(40)));      // ↓ + HP 2-4f
                vPush(absDir(true, 0), 0, randomRange(F10(25), F10(40)));       // ↓ 2.5-4f
                vPush(absDir(true, side), 0, randomRange(F10(25), F10(40)));    // down-forward 2.5-4f
                vPush(absDir(false, side), LK, randomRange(F10(40), F10(50)));  // forward + LK together 4-5f
                vPush(absDir(false, side), 0, randomRange(F10(40), F10(60)));   // forward alone 4-6f
                uint32_t lpArrow = randomRange(F10(30), F10(40));               // forward + LP 3-4f ...
                uint32_t lpOwn   = randomRange(F10(80), F10(100));              // ... then LP 8-10f more, the stick is yours
                vPush(absDir(false, side), LP, lpArrow + lpOwn, FL_OWN_CUT);
                V.seq[V.seqLen - 1].heCut = lpOwn;
                e.air = false;
                bool on = play(gp, now, e.prel);
                kimPunchGear = true;
                return on;
            }
            e.air = false;
            return play(gp, now, e.prel);
        }
        if      (rise & m[2]) seqSuperFixed(+1, coin() ? LP : HP);
        else if (rise & m[3]) seqSuperFixed(-1, coin() ? LP : HP);
        else if (rise & m[4]) seqSuperFixed(+1, coin() ? LK : HK);
        else if (rise & m[5]) seqSuperFixed(-1, coin() ? LK : HK);
        else return false;
        if (rise & (m[2] | m[3])) armSpamFromSeq(rise & (m[2] | m[3]), false);   // supers repeat while held
        e.air = false;
        return play(gp, now, e.prel);
    }
    // ---------------- X turbo (every profile, burnout off): random 1.2-2.2f on / off
    bool xTurboPrev = false, xTurboOn = true;
    uint64_t xTurboNext = 0;
    void applyXTurbo(Gamepad* gp, uint64_t now, bool physX) {
        bool burn = viperMode ? V.burnout : g_sagatBurnout;
        if (!physX || burn || !macrosOn) { xTurboPrev = false; return; }
        if (!xTurboPrev) { xTurboOn = true; xTurboNext = now + randomRange(F10(12), F10(22)); }
        else if (now >= xTurboNext) { xTurboOn = !xTurboOn; xTurboNext = now + randomRange(F10(12), F10(22)); }
        xTurboPrev = true;
        if (!xTurboOn) gp->state.buttons &= ~XBTN;
    }
    struct XTurboGuard {
        Gamepad* gp; uint64_t now; bool physX;
        ~XTurboGuard() { applyXTurbo(gp, now, physX); }
    };

    // Returns true when it owns the output this poll.
    bool sagatProcess(Gamepad* gp, uint64_t now, uint32_t turboPin, bool macroBusy) {
        bool tb = turboPin && (gp->debouncedGpio & turboPin);
        if (tb && !prevTurbo) { g_sagatBurnout = !g_sagatBurnout; }
        prevTurbo = tb;
        uint32_t phys = gp->state.buttons;
        bool x = (phys & XBTN) != 0;
        bool xRising = x && !prevSagatX;
        prevSagatX = x;
        if (V.playing) {
            if (runSeq(gp, now, toRel(gp->state.dpad))) return true;
        }
        if (g_sagatBurnout) {
            gp->state.buttons &= ~XBTN;           // X never reaches the game in burnout
            if (xRising && !macroBusy) {
                seqSagatLevel1(pinkActive || arjinActive || cammyActive || akiActive || ingridActive || kimActive, lilyActive); startSeq(now, 0, 0);
                armSpamFromSeq(0, true);
                if (runSeq(gp, now, toRel(gp->state.dpad))) return true;
            }
        }
        return false;
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

static void detectViperMode(MacroOptions* opts) {
    viperMode = false;
    for (int i = 0; i < MAX_MACRO_LIMIT; i++) {
        if (isViperMarker(opts->macroList[i])) { viperMode = true; break; }
    }
    g_viperActive = viperMode && macrosOn;
    g_viperFacingRight = V.facingRight;
    g_viperBurnout = V.burnout;
}

// Which GPIO pin each macro button / the turbo button is on.
void InputMacro::mapMacroPins() {
    GpioMappingInfo* pinMappings = Storage::getInstance().getProfilePinMappings();
    macroButtonMask = 0;
    memset(macroPinMasks, 0, sizeof(macroPinMasks));
    for (Pin_t pin = 0; pin < (Pin_t)NUM_BANK0_GPIOS; pin++) {
        GpioAction a = pinMappings[pin].action;
        if (a == GpioAction::BUTTON_PRESS_MACRO) macroButtonMask = 1 << pin;
        else if (a >= GpioAction::BUTTON_PRESS_MACRO_1 && a <= GpioAction::BUTTON_PRESS_MACRO_6)
            macroPinMasks[(int)a - (int)GpioAction::BUTTON_PRESS_MACRO_1] = 1 << pin;
        else if (a == GpioAction::BUTTON_PRESS_TURBO) turboPinMask = 1u << pin;
    }
}

void InputMacro::setup() {
    mapMacroPins();

    buildSagatOptions();
    buildPinkOptions();
    buildCammyOptions();
    buildEngineOptions();
    profileIdx = P_SAGAT;               // power-up in the built-in Sagat set
    setProfileFlags();
    inputMacroOptions = &g_sagatOpts;
    if (Storage::getInstance().getAddonOptions().macroOptions.macroBoardLedEnabled && isValidPin(BOARD_LED_PIN)) {
        gpio_init(BOARD_LED_PIN);
        gpio_set_dir(BOARD_LED_PIN, GPIO_OUT);
        boardLedEnabled = true;
    } else {
        boardLedEnabled = false;
    }
    boardLedEnabled = false;
    prevMacroInputPressed = false;

    // CUSTOM: seed random generator, clear gear state, detect Viper mode
    rngState = (uint32_t)getMicro() ^ 0x9E3779B9u;
    if (rngState == 0) rngState = 1;
    for (int i = 0; i < MAX_MACRO_LIMIT; i++) gearMask[i] = 0;
    gearPendingMacro = -1;
    detectViperMode(inputMacroOptions);

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
        if ( viperMode && (i == 4 || i == 5) )  // CUSTOM: side / burnout buttons in Viper mode
            continue;
        if ( macroPinMasks[i] & freeOwnedPins )  // CUSTOM: this press belongs to the free-button rule
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

        // CUSTOM: DP (gear Macro 1 / 2) sets the side: DP right -> facing right, DP left -> facing left
        if (sagatActive && macroPosition <= 1 && isGearMacro(macro)) {
            V.facingRight = (macroPosition == 0);
            g_viperFacingRight = V.facingRight;
        }
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
    uint8_t userDpad = gamepad->state.dpad;      // CUSTOM: your own stick, for the early arrow release

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
                if (sagatActive && !viperMode && isSuperMacro(macro) &&
                        (macroPosition == 2 || macroPosition == 3)) {   // CUSTOM: super spam while held
                    uint32_t lm = macro.macroInputs[stepCount(macro) - 1].buttonMask;
                    uint32_t a = lm & ATTACK_MASK;
                    uint32_t nb = (a & KICK_MASK) ? (superLight ? LK : HK) : (superLight ? LP : HP);
                    uint8_t d = 0;
                    if (lm & GAMEPAD_MASK_DL) d |= GAMEPAD_MASK_LEFT;
                    if (lm & GAMEPAD_MASK_DR) d |= GAMEPAD_MASK_RIGHT;
                    if (a) armSpam((uint32_t)macroPinMasks[macroPosition], false, nb, d);
                }
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
    if (isSuperMacro(macro) && (buttonMask & ATTACK_MASK)) {   // supers: random light / heavy
        uint32_t a = buttonMask & ATTACK_MASK;
        uint32_t nb = (a & KICK_MASK) ? (superLight ? LK : HK) : (superLight ? LP : HP);
        buttonMask = (buttonMask & ~ATTACK_MASK) | nb;
    }

    // Check if we should still hold this macro input based on duration
    uint64_t elapsed = currentMicros - macroStartTime;
    if (elapsed <= stepDuration[pos]) {
        // CUSTOM: early arrow release - the arrow lets go before the button and the stick is yours again
        bool dirsOn = elapsed + stepHeCut[pos] < stepDuration[pos] || stepHeCut[pos] == 0;
        if (!dirsOn) gamepad->state.dpad = userDpad;
        if (dirsOn) {
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

    Gamepad * gamepad = Storage::getInstance().GetGamepad();
    uint64_t now = getMicro();
    XTurboGuard xturbo{gamepad, now, (gamepad->state.buttons & XBTN) != 0};   // applied on every exit

    // CUSTOM: pin 20 turns all macros on/off
    {
        bool t = (gamepad->debouncedGpio & TOGGLE_PIN_MASK) != 0;
        if (t) {
            GpioMappingInfo* pm = Storage::getInstance().getProfilePinMappings();
            // hide the button pin 20 is mapped to, so the game never sees it
            if (pm[20].action == GpioAction::BUTTON_PRESS_A1) gamepad->state.buttons &= ~GAMEPAD_MASK_A1;
            if (pm[20].action == GpioAction::BUTTON_PRESS_A2) gamepad->state.buttons &= ~GAMEPAD_MASK_A2;
        }
        bool m1 = macroPinMasks[0] && (gamepad->debouncedGpio & macroPinMasks[0]);
        if (t && !prevTogglePin) togglePinCombo = false;
        bool m2 = macroPinMasks[1] && (gamepad->debouncedGpio & macroPinMasks[1]);
        bool comboNext = t && m1 && !prevComboMacro1, comboPrev = t && m2 && !prevComboMacro2;
        if (comboNext || comboPrev) {
            // pin 20 held + Macro 1: next profile; pin 20 held + Macro 2: previous profile
            togglePinCombo = true;
            reset();
            viperReset();
            gearPendingMacro = -1;
            spamReset();
            profileIdx = (profileIdx + (comboNext ? 1 : P_COUNT - 1)) % P_COUNT;
            setProfileFlags();
            eKim.air = false; eKim.jumpAt = 0; kimGear = HP;
            eChun.air = false; eChun.jumpAt = 0; chunGear = MK; chunHPAt = 0; chunHPFireAt = 0; chunMPAt = 0; chunHKAt = 0;
            akiHKAt = 0;
            eCammy.air = false; eCammy.jumpAt = 0; cammyFree = LP; chunFree = LK; freeOwnedPins = 0; afbSel = 1; afbPrev = false; afbRise = false;
            lilySpd = false; lilyOD = false;
            lukeHoldBtn = 0; lukeSuppress = 0; lukeTaken = false;
            g_sagatBurnout = false;
            chunGuard = JumpGuard(); lvl3Until = 0; manonHPAt = 0; arjinHKHold = false; arjinBlockUntil = 0; arjinPendUntil = 0; arjinPending = 0; arjinDownSwallow = false;
            cgrab = false; pinkHKHold = false; pinkHoldAfter = false;
            for (int i = 0; i < MAX_MACRO_LIMIT; i++) gearMask[i] = 0;
            if (cammyActive) { gearMask[0] = LK; gearMask[1] = LK; }   // Cammy DP gear starts on LK
            if (lilyActive)  { gearMask[0] = MP; gearMask[1] = MP; }   // Lily DP gear starts on MP
            inputMacroOptions = PROFILES[profileIdx].opts ? PROFILES[profileIdx].opts
                              : &Storage::getInstance().getAddonOptions().macroOptions;
            macrosOn = true;
            detectViperMode(inputMacroOptions);
            prevMacroInputPressed = true;    // the Macro 1 / Macro 2 press used for the switch never fires
            V.prevQcbPins = 0xFFFFFFFFu; V.prevLv1Pins = 0xFFFFFFFFu;   // nor counts as a qcb / Level 1 press
        }
        prevComboMacro1 = m1;
        prevComboMacro2 = m2;

        if (!t && prevTogglePin && !togglePinCombo && sagatActive) {
            V.facingRight = !V.facingRight;          // Sagat mode: pin 20 alone = side switch
            g_viperFacingRight = V.facingRight;
        } else if (!t && prevTogglePin && !togglePinCombo && viperMode) {
            // pin 20 alone (released, no Macro 1): macros on/off, Viper mode only
            macrosOn = !macrosOn;
            if (!macrosOn) {
                reset();
                viperReset();
                gearPendingMacro = -1;
            }
            g_viperActive = viperMode && macrosOn;
        }
        prevTogglePin = t;
        if (t) { prevMacroInputPressed = true; return; }   // holding pin 20: no macros
        if (!macrosOn) return;
    }

    // CUSTOM: pin 27 + HK rule (all profiles)
    if (pin27Process(gamepad, now)) {
        prevMacroInputPressed = true;
        return;
    }

    // CUSTOM: all profiles: a ↘/↙ slip right after your qcf/qcb forward is sent as the forward
    // CUSTOM: auto side (all profiles): ↙ held 10f = you are on the left (facing right), ↘ held 10f = facing left
    {
        uint8_t sd = runSOCDCleaner(Gamepad::resolveSOCDMode(gamepad->getOptions()), gamepad->state.dpad);
        int dg = (sd == (GAMEPAD_MASK_DOWN | GAMEPAD_MASK_LEFT)) ? +1 : (sd == (GAMEPAD_MASK_DOWN | GAMEPAD_MASK_RIGHT)) ? -1 : 0;
        if (dg != sideDiag) { sideDiag = dg; sideDiagAt = now; }
        if (dg && now - sideDiagAt >= 10ULL * FRAME_US && !V.playing && V.win == W_NONE && !isMacroRunning &&
                V.facingRight != (dg > 0)) {
            V.facingRight = (dg > 0);
            g_viperFacingRight = V.facingRight;
        }
    }
    // CUSTOM: anti-fireball button (pin 21) in Manon / Cammy / Jamie: read its press, hide what it is mapped to
    afbRise = false;
    if (afbOn()) {
        bool p = (gamepad->debouncedGpio & AFB_PIN_MASK) != 0;
        afbRise = p && !afbPrev;
        afbPrev = p;
        GpioAction a = Storage::getInstance().getProfilePinMappings()[21].action;
        gamepad->state.buttons &= ~buttonMaskForAction(a);
        gamepad->state.dpad &= ~dpadMaskForAction(a);
    } else {
        afbPrev = false;
    }
    if (!V.playing && !isMacroRunning) cammyCleanQcf(gamepad, now);

    // CUSTOM: remember when each direction was last physically held
    trackDirections(gamepad->state.dpad, now);

    // CUSTOM: super spam (built-in profiles) owns the output while it repeats
    if (sagatActive && !viperMode) {
        if (spamProcess(gamepad, now, isMacroRunning)) {
            prevMacroInputPressed = true;
            if (g_sagatBurnout) gamepad->state.buttons &= ~XBTN;
            return;
        }
    }

    // CUSTOM: Viper engine (runs first, owns the output while a Viper move plays)
    if (viperMode) {
        if (viperProcess(gamepad, now, macroPinMasks[4], macroPinMasks[5], turboPinMask,
                         (uint32_t)(macroPinMasks[0] | macroPinMasks[1]), isMacroRunning)) {
            prevMacroInputPressed = true;   // don't start a regular macro under it
            return;
        }
    }
    if (pinkActive || arjinActive) {
        if (pinkProcess(gamepad, now, macroPinMasks[0], macroPinMasks[1], macroPinMasks[2], macroPinMasks[3],
                        macroPinMasks[4], macroPinMasks[5], isMacroRunning)) {
            prevMacroInputPressed = true;
            return;
        }
        gamepad->state.buttons &= ~V.suppress;   // gear-choice punch stays hidden until released
    }
    if (lukeActive) lukeProcess(gamepad, now);
    if (chunActive) {
        if (chunProcess(gamepad, now, (const uint32_t*)macroPinMasks, isMacroRunning)) {
            prevMacroInputPressed = true;
            return;
        }
        gamepad->state.buttons &= ~V.suppress;   // the dive kick / gear-choice kick stays hidden until released
    }
    if (kimActive) {
        if (kimProcess(gamepad, now, (const uint32_t*)macroPinMasks, isMacroRunning)) {
            prevMacroInputPressed = true;
            return;
        }
        gamepad->state.buttons &= ~V.suppress;   // the air punch / kick and the gear-choice attack stay hidden until released
    }
    if (akiActive || ingridActive) {
        if (akiProcess(gamepad, now, (const uint32_t*)macroPinMasks, isMacroRunning)) {
            prevMacroInputPressed = true;
            return;
        }
        gamepad->state.buttons &= ~V.suppress;   // X used for the ↓HK rule stays hidden until released
    }
    if (cammyActive) {
        if (cammyProcess(gamepad, now, macroPinMasks[2], macroPinMasks[3], macroPinMasks[4], macroPinMasks[5], isMacroRunning)) {
            prevMacroInputPressed = true;
            return;
        }
        gamepad->state.buttons &= ~V.suppress;   // the kick you pressed stays hidden until released
    }
    if (lilyActive) {
        if (lilyProcess(gamepad, now, macroPinMasks[4], macroPinMasks[5], macroPinMasks[2], macroPinMasks[3], isMacroRunning)) {
            prevMacroInputPressed = true;
            return;
        }
        gamepad->state.buttons &= ~V.suppress;   // OD punch stays hidden until released
    }
    if (sagatActive) {
        if (sagatProcess(gamepad, now, turboPinMask, isMacroRunning)) {
            prevMacroInputPressed = true;
            return;
        }
    }

    checkMacroPress();

    gearSwallow &= gamepad->state.buttons;
    gamepad->state.buttons &= ~gearSwallow;

    // CUSTOM: gear selection (attack held, then G macro = instant gear)
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
        // attack held, then gear button: gear changes right away, for every gear macro
        if (popcount32(attackHeld) == 1 && !(arjinActive && (attackHeld & KICK_MASK)) &&
                !(cammyActive && (attackHeld & PUNCH_MASK))) {   // Arjin: punch gears only; Cammy / Jamie: kick gears only
            for (int i = 0; i < MAX_MACRO_LIMIT; i++)
                if (isGearMacro(inputMacroOptions->macroList[i])) gearMask[i] = attackHeld;
        }
        gearSwallow = attackHeld;               // keep that attack away from the game until released
        gamepad->state.buttons &= ~ATTACK_MASK;
        prevMacroInputPressed = true;
        return;
    }

    // CUSTOM: tell the LEDs which button the gear is on (Sagat / pink mode)
    if (chunActive) {
        g_sagatGearMask = chunGear;
    } else if (kimActive) {
        g_sagatGearMask = kimGear;
    } else if (pinkActive) {
        g_sagatGearMask = pinkGear;
    } else if (sagatActive) {
        uint32_t gm = 0;
        for (int i = 0; i < MAX_MACRO_LIMIT && !gm; i++) {
            const Macro& mc = inputMacroOptions->macroList[i];
            if (!mc.enabled || !isGearMacro(mc)) continue;
            if (gearMask[i]) gm = gearMask[i];
            else for (int k = 0; k < (int)mc.macroInputs_count; k++) gm |= mc.macroInputs[k].buttonMask & ATTACK_MASK;
        }
        g_sagatGearMask = lowestBit(gm);
    }
    // CUSTOM: LED colours by move type: gear buttons by the gear on them, the free button by its stored move
    {
        const uint32_t C_PUNCH = 0xFF5A00, C_KICK = 0x00FF00, C_OD_PUNCH = 0xFF0000, C_OD_KICK = 0x9600FF;   // orange, green, red, purple
        uint32_t gear = sagatActive ? (uint32_t)g_sagatGearMask : 0;
        g_ledGear = !gear ? 0 : (gear & PUNCH_MASK) ? C_PUNCH : C_KICK;
        int pin = -1;
        uint32_t color = 0;
        if (jamieActive || chunActive) {
            uint32_t pm = freePin(macroPinMasks[4], macroPinMasks[5]);
            if (pm) pin = __builtin_ctz(pm);
            uint32_t sel = chunActive ? chunFree : cammyFree;
            color = (sel & PUNCH_MASK) ? (sel == HP ? C_OD_PUNCH : C_PUNCH) : (sel == HK ? C_OD_KICK : C_KICK);
        }
        g_ledFreeColor = color;
        g_ledFreePin = pin;
        // anti-fireball button: medium orange / green, heavy yellow / blue, OD red / purple (punch / kick)
        const uint32_t C_HEAVY_PUNCH = 0xFFDC00, C_HEAVY_KICK = 0x0000FF;
        bool kick = jamieActive;
        g_ledAfbColor = afbSel == 0 ? (kick ? C_OD_KICK : C_OD_PUNCH) : afbSel == 1 ? (kick ? C_KICK : C_PUNCH)
                                    : (kick ? C_HEAVY_KICK : C_HEAVY_PUNCH);
        g_ledAfbPin = afbOn() ? 21 : -1;
    }

    checkMacroAction();
    runCurrentMacro();
}

void InputMacro::reinit() {
    mapMacroPins();
    detectViperMode(inputMacroOptions);
}
