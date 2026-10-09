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
//     was released within 9 frames.
//
//  2) Randomized timings for every macro (new random values each run):
//     - Normal macros ............ arrows 1.5-3f, attack 3-7f,
//                                  OD attack 2-5f, block after OD 4-8f
//     - "super" in the label ..... arrows 1.2-3f (at least 4 arrows under 2f),
//                                  attack 2-6f, plus HUMAN ERROR
//     - Viper mode, other macros . arrows 1.2-3f (mostly under 2f), attack 2-6f
//
//  3) HUMAN ERROR (supers): 50% of the time the arrow is released 1-3f before
//     the button, always keeping at least 1.4f of arrow + button together.
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

namespace {
    // ---------------------------------------------------------------- tunables
    constexpr uint32_t FRAME_US        = 16667;           // 1 frame at 60fps
    constexpr uint64_t GEAR_HOLD_US    = 2000000;         // 2 seconds
    constexpr uint64_t SKIP_WINDOW_US  = 9 * FRAME_US;    // smart-start window

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
    constexpr uint32_t HE_MIN_US       = F10(10);
    constexpr uint32_t HE_MAX_US       = F10(30);
    constexpr uint32_t HE_KEEP_US      = F10(14);

    // Viper
    constexpr uint32_t V_ARROW_MIN     = F10(12);
    constexpr uint32_t V_ARROW_MAX     = F10(35);
    constexpr uint64_t V_JUMP_LOCK_US  = 43ULL * FRAME_US;
    constexpr uint32_t V_WIN_MK_MIN    = F10(200);          // MK held (N or ↓) 20-24f = MK window
    constexpr uint32_t V_WIN_MK_MAX    = F10(240);
    constexpr uint64_t V_WIN_HK_US     = 14ULL * FRAME_US;
    constexpr uint64_t V_WIN_LOOP_US   = 44ULL * FRAME_US;  // after a seismo: hold → + punch 44f
    constexpr uint64_t V_NO_CANCEL_US  = 10ULL * FRAME_US;  // loop ran out: 10f with no cancels
    constexpr uint64_t V_CHP_PUNCH_US  = 10ULL * FRAME_US;  // punch within 10f of cr.HP -> plain seismo
    constexpr uint64_t V_WIN_LV2_US    = 20ULL * FRAME_US;
    constexpr uint64_t V_CHORD_US      = 2ULL * FRAME_US;  // time to catch 2 buttons together
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
    bool airSwallow = false;   // macro button pressed in the air: ignore it until released

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

    uint32_t humanErrorCut(uint32_t dur) {
        if (!coin()) return 0;
        if (dur <= HE_KEEP_US) return 0;
        uint32_t cut = randomRange(HE_MIN_US, HE_MAX_US);
        uint32_t maxCut = dur - HE_KEEP_US;
        return cut > maxCut ? maxCut : cut;
    }

    // Randomize the duration of every step for this run of a regular macro
    void prepareSteps(const Macro& macro, uint64_t now) {
        rngState ^= (uint32_t)now;
        if (rngState == 0) rngState = 1;

        int n = stepCount(macro);
        for (int i = 0; i < n; i++) stepHeCut[i] = 0;

        // ---------------- supers: fast arrows + human error
        if (isSuperMacro(macro)) {
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
            int lastAttack = -1;
            for (int i = 0; i < n; i++) {
                uint32_t m = macro.macroInputs[i].buttonMask;
                if (m & ATTACK_MASK) { stepDuration[i] = randomRange(BTN_MIN_US, BTN_MAX_US); lastAttack = i; }
                else if (!(m & DIR_MASK)) stepDuration[i] = macro.macroInputs[i].duration;
            }
            if (lastAttack >= 0 && (macro.macroInputs[lastAttack].buttonMask & DIR_MASK))
                stepHeCut[lastAttack] = humanErrorCut(stepDuration[lastAttack]);
            return;
        }

        // ---------------- Viper mode: fast arrows, mostly under 2f
        if (viperMode) {
            for (int i = 0; i < n; i++) {
                uint32_t m = macro.macroInputs[i].buttonMask;
                if (m & ATTACK_MASK) stepDuration[i] = randomRange(BTN_MIN_US, BTN_MAX_US);
                else if (m & DIR_MASK)
                    stepDuration[i] = (randomRange(1, 100) <= VIPER_FAST_PCT)
                        ? randomRange(FAST_MIN_US, FAST_MID_US)
                        : randomRange(FAST_MID_US, FAST_MAX_US);
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

    struct VStep { uint8_t dir; uint32_t btn; uint32_t dur; uint32_t heCut; uint8_t flags; };

    enum VWin { W_NONE, W_MK, W_HK, W_LOOP, W_LV2, W_CHP };

    struct VState {
        bool facingRight = true;
        bool burnout = false;

        VStep seq[16];
        int seqLen = 0, seqPos = 0;
        bool playing = false;
        uint64_t stepStart = 0;
        uint32_t seqTrigger = 0;      // button that started this sequence (for chord guard)
        uint64_t guardUntil = 0;      // other attack rising before this = cancel trigger
        bool lv2Pending = false;      // open Level 2 window when sequence ends
        bool isCrouchHP = false;      // X can cancel this one
        uint64_t chpStart = 0;

        VWin win = W_NONE;
        uint64_t winDeadline = 0;
        bool winGuard = false;        // MK/HK window: cancel if a 2nd attack comes right away

        bool chordPending = false;
        uint32_t chordMask = 0, chordFirst = 0;
        uint64_t chordStart = 0;

        uint32_t bufMask = 0, bufFirst = 0;   // pressed while a sequence was playing
        uint64_t bufStart = 0;
        bool bufJump = false;

        uint32_t suppress = 0;        // attack bits hidden from the game until released
        uint32_t prevButtons = 0;
        uint8_t  prevRel = 0;
        uint64_t jumpLockUntil = 0;   // Viper triggers off (first attack in the air ends it)
        uint64_t airUntil = 0;        // macro buttons off for the whole jump
        uint64_t noCancelUntil = 0;   // after the loop runs out: no cancels
        uint32_t loopPunch = 0;       // punch held with → during the loop
        uint64_t holdFrom = 0;        // loop/MK hold output starts here
        uint8_t  mkDir = 0;           // N or ↓ held with MK
        uint32_t prevSuper = 0;
        bool superCancel = false;     // a macro super cancelled the loop this poll
        bool loopBack = false;        // ← releases this loop hold

        bool prevSwitch = false, prevBurn = false;

        // direction history (relative codes)
        uint8_t  hCode[16];
        uint64_t hStart[16];
        int hHead = 0, hCount = 0;
    } V;

    void setLock(uint64_t until) { V.jumpLockUntil = until; V.airUntil = until; }
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
    void addArrows(std::initializer_list<uint8_t> dirs) {
        for (uint8_t d : dirs) vPush(d, 0, vArrow());
    }

    void seqJump()                  { vClear(); addArrows({D_, DF, F_, UF}); vPush(U_, 0, vArrow(), FL_JUMP); }
    void seqSeismo(uint32_t punches, bool walk) {   // jump-cancel seismo
        vClear();
        if (walk) vPush(F_, 0, randomRange(F10(60), F10(110)));
        addArrows({D_, DF, F_, UF});
        vPush(U_, punches, vBtn(), FL_OPEN_LOOP);
    }
    void seqFeint(bool walk) {                  // jump-cancel seismo feint
        vClear();
        if (walk) vPush(F_, 0, randomRange(F10(60), F10(110)));
        addArrows({D_, DF, F_, UF});
        vPush(U_, HP | LK, randomRange(F10(40), F10(70)));
        vPush(U_, coin() ? LK : HP, randomRange(F10(50), F10(90)));
    }
    void seqBurn(uint32_t kicks) {
        vClear(); addArrows({D_, DF, F_, UF}); vPush(U_, 0, vArrow(), FL_JUMP);
        vPush(N_, kicks, vBtn());
    }
    void seqThunder(uint32_t punches) {        // jump cancel thunder dash
        vClear(); addArrows({D_, DB, B_, UB});
        vPush(UB, punches, randomRange(F10(15), F10(25)));
        vPush(coin() ? B_ : UB, punches, vBtn());
    }
    void seqStandHP() {                         // st.HP -> jump cancel -> thunder dash feint
        vClear();
        vPush(N_, HP, randomRange(F10(50), F10(70)));
        addArrows({D_, DB, B_});
        vPush(UB, HP, randomRange(F10(40), F10(70)));
        uint8_t d6 = coin() ? B_ : UB;
        vPush(d6, HP | LK, randomRange(F10(120), F10(160)));
        vPush(coin() ? d6 : N_, LK, randomRange(F10(10), F10(30)));  // kick only, arrow 50/50
    }
    void seqCrouchHP() {                        // cr.HP -> thunder dash feint
        vClear();
        vPush(D_, HP, randomRange(F10(40), F10(70)), FL_ABORT_FWD);
        vPush(DB, 0, randomRange(F10(20), F10(50)));
        vPush(B_, 0, randomRange(F10(20), F10(50)));
        uint32_t s4 = randomRange(F10(15), F10(25));
        vPush(B_, HP, s4);
        vPush(B_, HP | LK, randomRange(F10(120), F10(140)));  // 12-14f
        vPush(B_, LK, randomRange(F10(10), F10(30)));         // punch released, kick held 1-3f
    }
    void seqSeismoNormal(uint32_t punches) {    // plain seismo: → ↘ → + punch (fast arrows 1.2-2f)
        vClear();
        vPush(F_, 0, randomRange(F10(12), F10(20)));
        vPush(DF, 0, randomRange(F10(12), F10(20)));
        vPush(F_, punches, vBtn(), FL_OPEN_LOOP);
    }
    // after MK: ↓ ↘ → ↗ → (1.2-3f each), then the button on N
    void mkMotion() {
        vClear();
        for (uint8_t d : {D_, DF, F_, UF, F_}) vPush(d, 0, randomRange(F10(12), F10(30)));
    }
    void seqMKSeismo(uint32_t punches) { mkMotion(); vPush(N_, punches, vBtn(), FL_OPEN_LOOP | FL_BACK); }
    void seqMKFeint() {
        mkMotion();
        vPush(N_, HP | LK, randomRange(F10(40), F10(70)));
        vPush(N_, coin() ? LK : HP, randomRange(F10(50), F10(90)));
    }
    void seqMKBurn(uint32_t kicks) { mkMotion(); vPush(N_, kicks, vBtn(), FL_LOCK); }
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
        V.seq[V.seqLen - 1].heCut = humanErrorCut(dur);
    }
    void seqLevel1() { superArrowsAndFinish({D_, DF, F_, D_, DF}, F_, HK); }
    void seqLevel2() { superArrowsAndFinish({D_, DB, B_, D_, DB}, B_, HP); }

    void startSeq(uint64_t now, uint32_t trigger, uint64_t guardUs) {
        V.playing = V.seqLen > 0;
        V.isCrouchHP = false;
        V.seqPos = 0;
        V.stepStart = now;
        V.seqTrigger = trigger;
        V.guardUntil = now + guardUs;
        V.bufMask = 0; V.bufFirst = 0; V.bufJump = false;
        V.chordPending = false;
        if (V.seqLen > 0 && (V.seq[0].flags & FL_JUMP)) setLock(now + V.seq[0].dur + V_JUMP_LOCK_US);
    }

    void closeWindow(uint64_t now) {
        (void)now;
        V.win = W_NONE; V.chordPending = false; V.winGuard = false;
    }
    void openLoop(uint64_t start, uint32_t punches, uint64_t holdFrom, bool backRelease = false) {
        V.win = W_LOOP; V.winDeadline = start + V_WIN_LOOP_US; V.winGuard = false;
        V.loopPunch = punches; V.holdFrom = holdFrom; V.loopBack = backRelease;
    }

    // Decide what a window press means. Returns true if a sequence was started.
    bool resolveWindow(uint32_t mask, uint32_t first, bool jump, uint64_t now) {
        VWin w = V.win;
        closeWindow(now);
        V.lv2Pending = false;
        if (jump && w == W_HK) { seqJump(); startSeq(now, 0, 0); return true; }

        if (w == W_LV2) {
            if (mask & LK) { seqLevel2(); startSeq(now, 0, 0); return true; }
            return false;
        }
        if (w == W_CHP) {                     // within 10f of cr.HP: punch = plain seismo
            uint32_t p = mask & PUNCH_MASK;
            if (!p) return false;
            seqSeismoNormal(popcount32(p) >= 2 ? p : lowestBit(p));
            startSeq(now, 0, 0); return true;
        }

        uint32_t punches = mask & PUNCH_MASK, kicks = mask & KICK_MASK;
        if (w == W_MK) {                      // MK: new motion, button on N
            if ((mask & LP) && (mask & LK)) { seqMKFeint(); startSeq(now, 0, 0); return true; }
            if (popcount32(punches) >= 2) { seqMKSeismo(punches); startSeq(now, 0, 0); return true; }
            if (punches && kicks) { if (first & PUNCH_MASK) kicks = 0; else punches = 0; }
            if (punches) { seqMKSeismo(lowestBit(punches)); startSeq(now, 0, 0); return true; }
            if (popcount32(kicks) >= 2) { seqMKBurn(MK | HK); startSeq(now, 0, 0); return true; }
            if (kicks) { seqMKBurn(kicks); startSeq(now, 0, 0); return true; }
            return false;
        }
        bool walk = (w != W_LOOP);            // loop: already holding →, start at ↓
        if ((mask & LP) && (mask & LK)) { seqFeint(walk); startSeq(now, 0, 0); return true; }
        if (popcount32(punches) >= 2) { seqSeismo(punches, walk); startSeq(now, 0, 0); return true; }
        if (punches && kicks) {               // mixed: take the button pressed first
            if (first & PUNCH_MASK) kicks = 0; else punches = 0;
        }
        if (punches) { seqSeismo(lowestBit(punches), walk); startSeq(now, 0, 0); return true; }
        if (kicks) {
            if (w == W_HK) {
                if (popcount32(kicks) != 1) return false;
                if (kicks == HK) { seqThunder(HP); startSeq(now, 0, 0); return true; }
                if (kicks == MK) { seqBurn(MK | HK); startSeq(now, 0, 0); return true; }
                if (kicks == LK) { seqThunder(MP | HP); V.lv2Pending = true; startSeq(now, 0, 0); return true; }
                return false;
            }
            if (popcount32(kicks) >= 2) { seqBurn(MK | HK); startSeq(now, 0, 0); return true; }
            seqBurn(kicks); startSeq(now, 0, 0); return true;
        }
        return false;
    }

    // Output the current sequence step into the gamepad state. Returns false when finished.
    bool runSeq(Gamepad* gp, uint64_t now, uint8_t physRel) {
        while (V.playing) {
            VStep& st = V.seq[V.seqPos];
            if ((st.flags & FL_ABORT_FWD) && physRel == F_) {   // cr.HP: walking forward cancels
                V.playing = false;
                V.suppress &= ~V.seqTrigger;
                return false;
            }
            if (now - V.stepStart < st.dur) break;
            V.stepStart += st.dur;
            V.seqPos++;
            if (V.seqPos >= V.seqLen) { V.playing = false; return false; }
            VStep& nx = V.seq[V.seqPos];
            if (nx.flags & FL_JUMP) setLock(V.stepStart + nx.dur + V_JUMP_LOCK_US);   // lock starts after ↑
            if (nx.flags & FL_LOCK) setLock(V.stepStart + V_JUMP_LOCK_US);           // lock starts at the kick
            if (nx.flags & FL_OPEN_LOOP) openLoop(V.stepStart, nx.btn, V.stepStart, (nx.flags & FL_BACK) != 0);
        }
        if (!V.playing) return false;
        VStep& st = V.seq[V.seqPos];
        uint64_t el = now - V.stepStart;
        gp->state.dpad = (el + st.heCut < st.dur) ? toAbs(st.dir) : 0;
        gp->state.buttons = st.btn;
        return true;
    }

    // Returns true when the Viper engine owns the output this poll.
    bool viperProcess(Gamepad* gp, uint64_t now, uint32_t switchPin, uint32_t burnPin, uint32_t superPins, bool otherMacroBusy) {
        Mask_t pins = gp->debouncedGpio;
        uint32_t superNow = (uint32_t)(pins & superPins);
        uint32_t superRising = superNow & ~V.prevSuper;
        V.prevSuper = superNow;

        // ---- side / burnout toggles (Macro 5 / Macro 6 buttons)
        bool sw = switchPin && (pins & switchPin);
        bool bo = burnPin && (pins & burnPin);
        if (sw && !V.prevSwitch) { V.facingRight = !V.facingRight; g_viperFacingRight = V.facingRight; }
        if (bo && !V.prevBurn)   { V.burnout = !V.burnout; g_viperBurnout = V.burnout; }
        V.prevSwitch = sw; V.prevBurn = bo;

        uint32_t phys = gp->state.buttons;
        uint8_t rel = toRel(gp->state.dpad);
        uint32_t rising = phys & ~V.prevButtons;
        bool upRising = (rel & RU) && !(V.prevRel & RU);
        bool ufRising = (rel == UF) && (V.prevRel != UF);
        V.prevButtons = phys;
        V.prevRel = rel;

        // direction history
        if (V.hCount == 0 || V.hCode[V.hHead] != rel) histPush(rel, now);

        V.suppress &= phys;                       // released buttons are no longer hidden

        // ---- a sequence is playing
        if (V.playing && V.isCrouchHP && (rising & PUNCH_MASK) && now - V.chpStart < V_CHP_PUNCH_US) {
            // punch early in cr.HP: stop it and do a plain seismo (two punches = OD)
            V.playing = false;
            V.win = W_CHP; V.winDeadline = now + V_CHORD_US; V.winGuard = false;
            V.chordPending = true; V.chordMask = 0; V.chordFirst = lowestBit(rising & PUNCH_MASK); V.chordStart = now;
        }
        if (V.playing && V.isCrouchHP && (rising & XBTN)) {
            // X cancels cr.HP: Level 1 in burnout, otherwise X goes to the game
            V.playing = false;
            V.suppress &= ~V.seqTrigger;
            if (V.burnout) {
                V.suppress |= XBTN;
                seqLevel1(); startSeq(now, 0, 0);
                if (runSeq(gp, now, rel)) return true;
            }
        }
        if (V.playing) {
            // trigger chord guard: another attack right after HP = player meant something else
            if (V.seqTrigger && now < V.guardUntil && (rising & ATTACK_MASK & ~V.seqTrigger)) {
                V.playing = false;
                V.suppress &= ~V.seqTrigger;
            } else {
                // remember presses for the next window (seismo loop / Level 2)
                bool armed = V.lv2Pending || V.win != W_NONE;
                uint32_t r = rising & (V.lv2Pending ? LK : ATTACK_MASK);
                if (armed && r) {
                    if (!V.bufMask) { V.bufFirst = lowestBit(r); V.bufStart = now; }
                    V.bufMask |= r;
                }
                V.suppress |= rising & ATTACK_MASK;   // never leak a press into the game mid-move
                if (ufRising && !V.lv2Pending && V.win == W_HK) V.bufJump = true;
                if (runSeq(gp, now, rel)) return true;
                // sequence just finished
                if (V.lv2Pending) { V.lv2Pending = false; V.win = W_LV2; V.winDeadline = now + V_WIN_LV2_US; }
                if (V.win != W_NONE && (V.bufMask || V.bufJump)) {
                    if (V.bufJump) {
                        V.bufMask = 0; V.bufJump = false;
                        if (resolveWindow(0, 0, true, now) && runSeq(gp, now, rel)) return true;
                    } else {
                        V.chordPending = true;
                        V.chordMask = V.bufMask; V.chordFirst = V.bufFirst; V.chordStart = V.bufStart;
                        V.bufMask = 0;
                    }
                }
            }
        }

        bool locked = now < V.jumpLockUntil;
        bool holding = !V.playing && (V.win == W_LOOP || V.win == W_MK);   // we own the stick
        if (upRising && !V.playing && !holding) setLock(now + V_JUMP_LOCK_US);   // you jumped
        // first attack in the air goes to the game and ends the lock (the next one can trigger)
        if (locked && !V.playing && V.win == W_NONE && (rising & ATTACK_MASK)) V.jumpLockUntil = 0;

        // ---- window handling
        if (V.win == W_LOOP && !V.playing && V.loopBack && (rel & RB)) {
            closeWindow(now);                     // seismo after MK: ← gives you the stick back
        }
        if (V.win == W_LOOP && !V.playing && superRising) {
            closeWindow(now);                     // macro super cancels the loop hold
            V.superCancel = true;
        }
        if (V.win != W_NONE) {
            if (V.winGuard && now < V.guardUntil && (rising & ATTACK_MASK & ~V.seqTrigger)) {
                closeWindow(now);                    // MP+MK parry, HP+HK impact, etc.
            } else {
                V.winGuard = V.winGuard && now < V.guardUntil;
                uint32_t r = rising & ATTACK_MASK;
                if (V.winGuard) r &= ~V.seqTrigger;
                if (r && !V.chordPending) {
                    V.chordPending = true; V.chordMask = 0; V.chordFirst = lowestBit(r); V.chordStart = now;
                }
                if (V.chordPending) {
                    V.chordMask |= r;
                    V.suppress |= r;
                    bool ready = popcount32(V.chordMask) >= 2 || now - V.chordStart >= V_CHORD_US
                                 || (V.win == W_LV2);
                    if (ready) {
                        uint32_t m = V.chordMask, f = V.chordFirst;
                        V.chordPending = false;
                        if (resolveWindow(m, f, false, now)) {
                            V.seqTrigger = 0;
                            if (runSeq(gp, now, rel)) return true;
                        } else {
                            V.suppress &= ~m;     // not a window input: let it through
                        }
                    }
                } else if (ufRising && V.win == W_HK) {   // jump only after st.HK
                    if (resolveWindow(0, 0, true, now) && runSeq(gp, now, rel)) return true;
                }
                if (V.win != W_NONE && !V.chordPending && now >= V.winDeadline) {
                    if (V.win == W_LOOP) V.noCancelUntil = now + V_NO_CANCEL_US;   // ran out: 10f no cancels
                    closeWindow(now);
                }
            }
        }

        // ---- new triggers (idle)
        if (V.win == W_NONE && !V.playing && !otherMacroBusy && !locked && !(rel & RU)) {
            uint32_t r = rising;
            bool canCancel = now >= V.noCancelUntil;
            if (V.burnout && (r & XBTN)) {
                V.suppress |= XBTN;
                seqLevel1(); startSeq(now, 0, 0);
                if (runSeq(gp, now, rel)) return true;
            } else if (!canCancel) {
                // loop just ran out: everything goes to the game as a normal hit
            } else if ((r & PUNCH_MASK) && detectSeismoMotion(now)) {
                // you did a seismo yourself: let it through, then hold → + that punch (loop)
                openLoop(now, r & PUNCH_MASK, now + FRAME_US);
            } else if ((r & ATTACK_MASK) == HP && !(phys & ATTACK_MASK & ~HP)) {
                if (rel & RD) {
                    seqCrouchHP();
                } else if (!recentDown(now)) {
                    seqStandHP();
                } else {
                    V.seqLen = 0;               // HP right after a motion = special move, leave it
                }
                if (V.seqLen > 0) {
                    bool crouch = (rel & RD) != 0;
                    V.suppress |= HP;
                    startSeq(now, HP, 0);               // nothing stops st.HP; cr.HP rules are above
                    V.isCrouchHP = crouch;
                    V.chpStart = now;
                    if (runSeq(gp, now, rel)) return true;
                }
            } else if ((r & ATTACK_MASK) == MK && !(phys & ATTACK_MASK & ~MK)) {
                V.win = W_MK; V.winDeadline = now + randomRange(V_WIN_MK_MIN, V_WIN_MK_MAX);
                V.winGuard = true; V.seqTrigger = MK; V.guardUntil = now + V_CHORD_US;
                V.mkDir = (rel & RD) ? D_ : N_; V.holdFrom = now;
            } else if ((r & ATTACK_MASK) == HK && !(phys & ATTACK_MASK & ~HK) && (rel == N_ || rel == B_)) {
                V.win = W_HK; V.winDeadline = now + V_WIN_HK_US;
                V.winGuard = true; V.seqTrigger = HK; V.guardUntil = now + V_CHORD_US;
            }
        }

        // ---- hold output: MK held on N/↓ (MK window), → + punch (seismo loop)
        if (!V.playing && (V.win == W_MK || V.win == W_LOOP) && now >= V.holdFrom) {
            bool loop = (V.win == W_LOOP);
            gp->state.dpad = toAbs(loop ? F_ : V.mkDir);
            gp->state.buttons = (phys & ~(ATTACK_MASK | XBTN)) | (loop ? V.loopPunch : MK);
            return true;
        }

        if (V.burnout) V.suppress |= (phys & XBTN);
        gp->state.buttons &= ~V.suppress;
        return false;
    }

    bool viperLocked(uint64_t now) { return now < V.airUntil; }
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
    uint64_t elapsed = currentMicros - macroStartTime;
    if (elapsed <= stepDuration[pos]) {
        // CUSTOM: human error - arrow released a little before the button
        bool dirsOn = elapsed + stepHeCut[pos] < stepDuration[pos] || stepHeCut[pos] == 0;
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

    // CUSTOM: pin 20 turns all macros on/off
    {
        bool t = (gamepad->debouncedGpio & TOGGLE_PIN_MASK) != 0;
        if (t) {
            GpioMappingInfo* pm = Storage::getInstance().getProfilePinMappings();
            // hide the button pin 20 is mapped to, so the game never sees it
            if (pm[20].action == GpioAction::BUTTON_PRESS_A1) gamepad->state.buttons &= ~GAMEPAD_MASK_A1;
            if (pm[20].action == GpioAction::BUTTON_PRESS_A2) gamepad->state.buttons &= ~GAMEPAD_MASK_A2;
        }
        if (t && !prevTogglePin) {
            macrosOn = !macrosOn;
            if (!macrosOn) {
                reset();
                V.playing = false; V.win = W_NONE; V.chordPending = false; V.lv2Pending = false;
                V.suppress = 0;
                gearPendingMacro = -1;
            }
            g_viperActive = viperMode && macrosOn;
        }
        prevTogglePin = t;
        if (!macrosOn) return;
    }

    // CUSTOM: remember when each direction was last physically held
    trackDirections(gamepad->state.dpad, now);

    // CUSTOM: Viper engine (runs first, owns the output while a Viper move plays)
    if (viperMode) {
        if (viperProcess(gamepad, now, macroPinMasks[4], macroPinMasks[5],
                         (uint32_t)(macroPinMasks[2] | macroPinMasks[3]), isMacroRunning)) {
            prevMacroInputPressed = true;   // don't start a regular macro under it
            return;
        }
        if (V.superCancel) { V.superCancel = false; prevMacroInputPressed = false; }   // super fires now
    }

    checkMacroPress();
    // no macros in the air: a macro button pressed during the jump lock is thrown away
    // and stays dead until you let go of it (it never fires on landing)
    if (viperMode && !isMacroRunning) {
        if (pressedMacro < 0) airSwallow = false;
        else if (viperLocked(now)) airSwallow = true;
        if (airSwallow) { pressedMacro = -1; prevMacroInputPressed = true; }
    }

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
        gearArmed        = (popcount32(attackHeld) == 1); // only one attack allowed
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
    detectViperMode(inputMacroOptions);
}
