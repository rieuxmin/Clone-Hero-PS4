// ============================================================================
// game.h - PS4 Guitar Hero / Clone Hero 1080p60 Core Engine
// Authentic Clone Hero Hit Window (±70ms), HOPO Coyote Timing, Star Power
// Phrases, Sustain Tick Scoring, Star Rating (1-7 Stars), & Miss Patterns
// Optimized O(1) active-window note processing for locked 60 FPS on PS4
// ============================================================================
#pragma once
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>

struct Note {
    double time = 0.0;
    int mask = 0;         // bits 0..4 = Green..Orange, bit 5 = Open Note
    double sustain = 0.0;
    bool isHopo = false;
    bool isStarPower = false;
    int spPhraseId = 0;
    bool hit = false;
    bool missed = false;
    bool sustainActive = false;
};

struct Section {
    double time = 0.0;
    std::string name;
};

struct Chart {
    std::vector<Note> notes;
    std::vector<Section> sections;
    double bpm = 120.0;
    double duration = 180.0;
    int baseScore = 10000;
    std::string title = "Unknown Track";
    std::string artist = "Unknown Artist";
    std::string charter = "";
};

struct Game {
    Chart* ch = nullptr;
    size_t first = 0;
    int score = 0;
    int combo = 0;
    int maxCombo = 0;
    int notesHit = 0;
    int misses = 0;
    int overstrums = 0;
    int rockMeter = 70;
    int flubEvents = 0;           // Increments on every note miss or overstrum for GH flub SFX
    bool guitarMuted = false;     // Guitar stem ducks on miss/overstrum, restores on next hit

    // Star Power / Overdrive state (Clone Hero / YARG exact 0..100% bar with active recharge)
    double starPower = 0.0;       // 0.0 .. 100.0
    bool spActive = false;
    double spEndTime = 0.0;
    double sustainScoreCarry = 0.0;
    double lastSpPhraseAwardTime = -10.0;

    // Per-lane hit flame timers (lanes 0..4) & overstrum flash timer
    double laneFlameTime[5] = {-10.0, -10.0, -10.0, -10.0, -10.0};
    double lastHitTime = -10.0;
    double lastOverstrumTime = -10.0;
    int lastHitMask = 0;
    double lastUpdateTime = 0.0;

    // Clone Hero Hit Windows (Expert = ±70ms = 140ms total window)
    double hitWindow = 0.070;
    double missWindow = 0.070;
    int difficulty = 1; // 0=Easy, 1=Medium, 2=Hard, 3=Expert

    void reset(Chart* chartPtr, int diff) {
        ch = chartPtr;
        first = 0;
        score = 0;
        combo = 0;
        maxCombo = 0;
        notesHit = 0;
        misses = 0;
        overstrums = 0;
        rockMeter = 70;
        flubEvents = 0;
        guitarMuted = false;
        starPower = 0.0;
        spActive = false;
        spEndTime = 0.0;
        sustainScoreCarry = 0.0;
        lastSpPhraseAwardTime = -10.0;
        for (int i = 0; i < 5; i++) laneFlameTime[i] = -10.0;
        lastHitTime = -10.0;
        lastOverstrumTime = -10.0;
        lastHitMask = 0;
        lastUpdateTime = 0.0;
        difficulty = diff;

        // Clone Hero hit window scaling by difficulty (±70ms on Expert)
        if (diff == 0)      { hitWindow = 0.085; missWindow = 0.085; }
        else if (diff == 1) { hitWindow = 0.078; missWindow = 0.078; }
        else if (diff == 2) { hitWindow = 0.074; missWindow = 0.074; }
        else                { hitWindow = 0.070; missWindow = 0.070; }

        if (ch) {
            double maxEnd = 10.0;
            int totalPts = 0;
            for (auto& n : ch->notes) {
                n.hit = false;
                n.missed = false;
                n.sustainActive = false;
                double endT = n.time + n.sustain;
                if (endT > maxEnd) maxEnd = endT;
                int gems = 0;
                for (int l = 0; l < 5; l++) if (n.mask & (1 << l)) gems++;
                if (n.mask & 32) gems = 1;
                totalPts += 50 * std::max(1, gems) + (int)(n.sustain * 25.0 * std::max(1, gems));
            }
            ch->duration = maxEnd;
            ch->baseScore = std::max(500, totalPts);
        }
    }

    void seekTo(double targetTime) {
        if (!ch) return;
        first = 0;
        combo = 0;
        guitarMuted = false;
        sustainScoreCarry = 0.0;
        lastUpdateTime = targetTime;
        for (size_t i = 0; i < ch->notes.size(); i++) {
            if (ch->notes[i].time < targetTime - 0.10) {
                ch->notes[i].hit = true;
                ch->notes[i].missed = false;
                ch->notes[i].sustainActive = false;
                first = i + 1;
            } else {
                ch->notes[i].hit = false;
                ch->notes[i].missed = false;
                ch->notes[i].sustainActive = false;
            }
        }
    }

    // Clone Hero / YARG Full Combo (FC) state: true as long as 0 misses and 0 overstrums
    bool isFullCombo() const {
        return (misses == 0 && overstrums == 0);
    }

    // Clone Hero / YARG Base Multiplier: 1x (0-9), 2x (10-19), 3x (20-29), 4x (30+)
    int baseMultiplier() const {
        if (combo >= 30) return 4;
        if (combo >= 20) return 3;
        if (combo >= 10) return 2;
        return 1;
    }

    int multiplier() const {
        return baseMultiplier();
    }

    // Clone Hero / YARG 0..10 sub-multiplier progress pips (10 when at max 4x)
    int multiplierPips() const {
        if (combo >= 30) return 10;
        return combo % 10;
    }

    int activeMultiplier() const {
        return baseMultiplier() * (spActive ? 2 : 1);
    }

    // Clone Hero Star Rating (0 to 7 stars + fractional progress 0.0..1.0 to next star)
    int getStarRating(double* outProgress = nullptr) const {
        if (!ch || ch->baseScore <= 0) {
            if (outProgress) *outProgress = 0.0;
            return 0;
        }
        // Authentic Clone Hero / Rock Band average multiplier thresholds for 1..7 stars
        const double multThresholds[8] = {0.0, 0.20, 0.55, 1.00, 1.60, 2.20, 2.80, 3.60};
        double avgMult = (double)score / (double)ch->baseScore;
        int stars = 0;
        for (int s = 1; s <= 7; s++) {
            if (avgMult >= multThresholds[s]) stars = s;
            else {
                if (outProgress) {
                    double span = multThresholds[s] - multThresholds[s - 1];
                    *outProgress = (span > 0.0) ? std::clamp((avgMult - multThresholds[s - 1]) / span, 0.0, 1.0) : 0.0;
                }
                return stars;
            }
        }
        if (outProgress) *outProgress = 1.0;
        return 7;
    }

    void activateStarPower(double now) {
        if (!spActive && starPower >= 50.0) {
            spActive = true;
            double bpm = (ch && ch->bpm > 20.0) ? ch->bpm : 120.0;
            double fullDrainSec = 32.0 * (60.0 / bpm); // 8 measures (32 beats)
            spEndTime = now + (starPower / 100.0) * fullDrainSec;
        }
    }

    static bool matchesFrets(int noteMask, int held, int justPressed, bool isTapEvent, int difficulty) {
        int noteLanes = noteMask & 0x1F;
        bool isOpen = (noteMask & 32) != 0;
        if (isOpen) {
            return (held & 0x1F) == 0;
        }
        bool isSingle = (noteLanes & (noteLanes - 1)) == 0;
        if (isSingle) {
            int lane = 0;
            for (int l = 0; l < 5; l++) if (noteLanes & (1 << l)) lane = l;
            int higherMask = ~((1 << (lane + 1)) - 1) & 0x1F;
            if (isTapEvent) {
                if (difficulty <= 1) return ((justPressed & (1 << lane)) != 0) || ((held & (1 << lane)) != 0);
                return (((justPressed & (1 << lane)) != 0) || ((held & (1 << lane)) != 0)) && ((held & higherMask) == 0);
            } else {
                // Clone Hero single-note anchoring: lower frets can be held, higher frets cannot
                if (difficulty == 0) return (held & (1 << lane)) != 0;
                return ((held & (1 << lane)) != 0) && ((held & higherMask) == 0);
            }
        } else {
            // Chord: exact fret match required (on Easy/Medium allow lower anchor)
            if (difficulty <= 1) {
                int maxBit = 0;
                for (int b = 4; b >= 0; b--) if (noteLanes & (1 << b)) { maxBit = b; break; }
                int higherMask = ~((1 << (maxBit + 1)) - 1) & 0x1F;
                return ((held & noteLanes) == noteLanes) && ((held & higherMask) == 0);
            }
            return (held & 0x1F) == noteLanes;
        }
    }

    // O(1) localized check if this note completes a Star Power phrase (supports active SP overlap/recharge!)
    void registerHit(Note& n, size_t noteIdx, double t) {
        n.hit = true;
        n.sustainActive = (n.sustain > 0.05);
        notesHit++;
        combo++;
        if (combo > maxCombo) maxCombo = combo;
        guitarMuted = false; // Restore guitar stem to 100% volume immediately on hit!

        int mult = activeMultiplier();
        int chordCount = 0;
        for (int l = 0; l < 5; l++) {
            if (n.mask & (1 << l)) {
                chordCount++;
                laneFlameTime[l] = t;
            }
        }
        if (n.mask & 32) {
            chordCount = 1;
            for (int l = 0; l < 5; l++) laneFlameTime[l] = t;
        }

        score += 50 * std::max(1, chordCount) * mult;
        rockMeter = std::min(100, rockMeter + 2);
        lastHitTime = t;
        lastHitMask = n.mask;

        if (n.isStarPower && n.spPhraseId > 0 && ch) {
            int pid = n.spPhraseId;
            bool moreInPhrase = false;
            size_t limit = std::min(ch->notes.size(), noteIdx + 64);
            for (size_t k = noteIdx + 1; k < limit; k++) {
                const Note& other = ch->notes[k];
                if (other.spPhraseId == pid && !other.hit && !other.missed) {
                    moreInPhrase = true;
                    break;
                }
                if (other.spPhraseId > pid) break;
            }
            if (!moreInPhrase) {
                size_t backStart = (noteIdx > 64) ? (noteIdx - 64) : 0;
                for (size_t k = backStart; k < noteIdx; k++) {
                    const Note& other = ch->notes[k];
                    if (other.spPhraseId == pid && !other.hit && !other.missed) {
                        moreInPhrase = true;
                        break;
                    }
                }
            }
            if (!moreInPhrase) {
                // Clone Hero / YARG: Completing an SP phrase awards +25% SP even if SP is currently active!
                starPower = std::min(100.0, starPower + 25.0);
                lastSpPhraseAwardTime = t;
                if (spActive) {
                    double bpm = (ch && ch->bpm > 20.0) ? ch->bpm : 120.0;
                    double fullDrainSec = 32.0 * (60.0 / bpm);
                    spEndTime = t + (starPower / 100.0) * fullDrainSec;
                }
            }
        }
    }

    // O(1) localized Star Power phrase cancellation on miss
    void cancelStarPowerPhrase(size_t noteIdx, int phraseId) {
        if (!ch || phraseId <= 0) return;
        size_t startIdx = (noteIdx > 64) ? (noteIdx - 64) : 0;
        size_t endIdx = std::min(ch->notes.size(), noteIdx + 64);
        for (size_t k = startIdx; k < endIdx; k++) {
            if (ch->notes[k].spPhraseId == phraseId) {
                ch->notes[k].isStarPower = false;
            }
        }
    }

    void strum(double t, int held, int justPressed = 0, bool isTapEvent = false) {
        if (!ch) return;

        int bestIdx = -1;
        double bestAbs = 999.0;

        // Only scan notes within hitWindow of current time (breaks immediately past window)
        for (size_t i = first; i < ch->notes.size(); i++) {
            Note& n = ch->notes[i];
            if (n.hit || n.missed) continue;
            double diff = n.time - t;
            if (diff > hitWindow) break;
            double ad = std::fabs(diff);
            if (ad <= hitWindow) {
                // In Clone Hero / YARG, tapping only hits HOPO/Tap notes (or any note when tapMode is on)
                if (isTapEvent && !n.isHopo && combo == 0) {
                    // Allow tap-mode to hit first note too
                }
                if (matchesFrets(n.mask, held, justPressed, isTapEvent, difficulty)) {
                    bestIdx = (int)i;
                    break;
                }
                if (ad < bestAbs) {
                    bestAbs = ad;
                    bestIdx = (int)i;
                }
            }
        }

        if (bestIdx >= 0) {
            Note& n = ch->notes[bestIdx];
            if (matchesFrets(n.mask, held, justPressed, isTapEvent, difficulty)) {
                registerHit(n, (size_t)bestIdx, t);
                return;
            }
        }

        // Authentic Clone Hero / YARG Overstrum Penalty + Guitar Hero Flub Trigger
        if (!isTapEvent) {
            combo = 0;
            overstrums++;
            flubEvents++;
            guitarMuted = true;
            rockMeter = std::max(0, rockMeter - 2);
            lastOverstrumTime = t;
        }
    }

    void update(double now, int held = 0, bool tapMode = false) {
        if (!ch) return;
        // Never evaluate notes or miss logic during pre-song lead-in (now < -0.05)!
        // Guarantees early notes are NEVER falsely marked as missed or drawn greyed-out on song launch!
        if (now < -0.05) {
            lastUpdateTime = now;
            return;
        }
        double dt = (lastUpdateTime > 0.0 && now > lastUpdateTime) ? (now - lastUpdateTime) : 0.0;
        lastUpdateTime = now;

        // Clone Hero / YARG Beat-Synchronized Star Power Drain (8 measures = 32 beats)
        if (spActive) {
            double bpm = (ch->bpm > 20.0) ? ch->bpm : 120.0;
            double fullDrainSec = 32.0 * (60.0 / bpm);
            double drainPerSec = 100.0 / std::max(4.0, fullDrainSec);
            starPower -= dt * drainPerSec;
            if (starPower <= 0.0) {
                starPower = 0.0;
                spActive = false;
            } else {
                spEndTime = now + (starPower / 100.0) * fullDrainSec;
            }
        }

        const size_t totalNotes = ch->notes.size();

        // Advance `first` past completely finished notes (> 0.28s behind strikeline)
        while (first < totalNotes) {
            const Note& fn = ch->notes[first];
            if ((fn.hit || fn.missed) && !fn.sustainActive && (now - (fn.time + fn.sustain)) > 0.28) {
                first++;
            } else {
                break;
            }
        }

        // Only iterate over notes near the strikeline — break immediately once n.time > now + 0.15!
        for (size_t i = first; i < totalNotes; i++) {
            Note& n = ch->notes[i];
            double diff = n.time - now;

            if (diff > 0.15) break;

            // Clone Hero / YARG HOPO & Tap Front-End Pre-Hold Catch
            // HOPOs auto-hit when combo > 0 and frets are held across strikeline; Tap mode auto-hits even at combo == 0!
            if (!n.hit && !n.missed && ((n.isHopo && combo > 0) || tapMode)) {
                if (diff <= 0.030 && diff >= -hitWindow) {
                    if (matchesFrets(n.mask, held, held, false, difficulty)) {
                        registerHit(n, i, now);
                    }
                }
            }

            // Sustain tail scoring & early-release handling (releasing sustain early does NOT break combo!)
            if (n.hit && n.sustainActive) {
                if (now <= n.time + n.sustain) {
                    int noteLanes = n.mask & 0x1F;
                    bool stillHeld = (n.mask & 32) ? true : ((held & noteLanes) == noteLanes);
                    if (stillHeld) {
                        int gems = 0;
                        for (int l = 0; l < 5; l++) if (noteLanes & (1 << l)) gems++;
                        if (n.mask & 32) gems = 1;
                        double bpm = (ch->bpm > 20.0) ? ch->bpm : 120.0;
                        double ptsPerSec = 25.0 * (bpm / 120.0) * std::max(1, gems) * activeMultiplier();
                        sustainScoreCarry += dt * ptsPerSec;
                        if (sustainScoreCarry >= 1.0) {
                            int pts = (int)sustainScoreCarry;
                            score += pts;
                            sustainScoreCarry -= pts;
                        }
                        for (int l = 0; l < 5; l++) {
                            if (noteLanes & (1 << l)) laneFlameTime[l] = now;
                        }
                        if (n.isStarPower) {
                            starPower = std::min(100.0, starPower + dt * 4.5);
                        }
                    } else if (now - n.time > 0.10) {
                        n.sustainActive = false;
                    }
                } else {
                    n.sustainActive = false;
                }
            }

            // Note Pass Miss (slipped past back of strikeline window)
            if (!n.hit && !n.missed && now >= 0.0 && (now - n.time) > missWindow) {
                n.missed = true;
                combo = 0;
                misses++;
                flubEvents++;
                guitarMuted = true;
                rockMeter = std::max(0, rockMeter - 3);
                if (n.isStarPower && n.spPhraseId > 0) {
                    cancelStarPowerPhrase(i, n.spPhraseId);
                }
            }
        }
    }
};
