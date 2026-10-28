// ============================================================================
// midi.h - Clone Hero / Guitar Hero .mid, .chart, and song.ini Parser for PS4
// Supports Easy / Medium / Hard / Expert + Smart Difficulty Downsampling
// + Section / Setlist Chapter Parsing
// ============================================================================
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include "game.h"

inline std::string trimStr(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) s.erase(s.begin());
    return s;
}

inline std::string cleanDisplayText(const std::string& raw) {
    // Strip HTML/CloneHero color tags like <color=#...> and convert UTF-8 special colons
    std::string out;
    bool inTag = false;
    for (size_t i = 0; i < raw.size(); i++) {
        unsigned char c = (unsigned char)raw[i];
        if (c == '<') { inTag = true; continue; }
        if (c == '>') { inTag = false; continue; }
        if (inTag) continue;
        if (c >= 32 && c < 127) {
            out.push_back((char)c);
        } else if (c == 0xEA && i + 2 < raw.size() && (unsigned char)raw[i+1] == 0x9E && (unsigned char)raw[i+2] == 0x89) {
            out.push_back(':');
            i += 2;
        }
    }
    return trimStr(out);
}

inline std::map<std::string, std::string> loadSongIni(const std::string& path) {
    std::map<std::string, std::string> res;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return res;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        std::string k = trimStr(line);
        std::string v = cleanDisplayText(eq + 1);
        for (char& c : k) c = (char)tolower((unsigned char)c);
        if (!k.empty()) res[k] = v;
    }
    fclose(f);
    return res;
}

// Clean section name from MIDI/Chart event e.g. "[section Iron Man]" -> "IRON MAN"
inline std::string parseSectionEvent(const std::string& raw) {
    std::string s = trimStr(raw);
    if (s.empty()) return "";
    if (s.front() == '"' && s.back() == '"' && s.size() >= 2) s = s.substr(1, s.size() - 2);
    if (s.front() == '[' && s.back() == ']' && s.size() >= 2) s = s.substr(1, s.size() - 2);
    s = trimStr(s);
    if (s.compare(0, 8, "section ") == 0 || s.compare(0, 8, "Section ") == 0) {
        s = trimStr(s.substr(8));
    } else if (s.compare(0, 4, "prc_") == 0) {
        s = trimStr(s.substr(4));
    } else {
        return "";
    }
    for (char& c : s) {
        if (c == '_') c = ' ';
    }
    return cleanDisplayText(s);
}

// Ensures Easy (0), Medium (1), Hard (2), and Expert (3) have distinct, playable note density & fret limits
inline void applyDifficultyFilter(Chart& chart, int diffIdx, bool wasFallbackFromExpert) {
    if (diffIdx >= 3 || chart.notes.empty()) return;

    // Minimum spacing between notes when downsampling or smoothing lower difficulties
    double minGap = 0.0;
    if (diffIdx == 0) minGap = wasFallbackFromExpert ? 0.38 : 0.24;
    else if (diffIdx == 1) minGap = wasFallbackFromExpert ? 0.24 : 0.14;
    else if (diffIdx == 2) minGap = wasFallbackFromExpert ? 0.14 : 0.0;

    std::vector<Note> filtered;
    filtered.reserve(chart.notes.size());
    double lastTime = -10.0;

    for (size_t i = 0; i < chart.notes.size(); i++) {
        Note n = chart.notes[i];
        if (minGap > 0.0 && (n.time - lastTime) < minGap) {
            continue;
        }

        int mask = n.mask & 0x1F;
        if (mask == 0) {
            // Open note -> convert to Green on Easy/Medium
            if (diffIdx <= 1) n.mask = 1;
        } else if (diffIdx == 0) {
            // EASY: Single notes only, folded into Green (0), Red (1), Yellow (2)
            int lane = 0;
            for (int l = 0; l < 5; l++) {
                if (mask & (1 << l)) { lane = l; break; }
            }
            if (lane == 3) lane = 1;
            else if (lane == 4) lane = 2;
            n.mask = (1 << lane);
            n.isHopo = false;
        } else if (diffIdx == 1) {
            // MEDIUM: Max 4 frets (Green..Blue), fold Orange (4) into Blue (3), max 2-note chords
            if (mask & 16) {
                mask = (mask & ~16) | 8;
            }
            int activeCount = 0;
            int newMask = 0;
            for (int l = 0; l < 4; l++) {
                if (mask & (1 << l)) {
                    if (activeCount < (wasFallbackFromExpert ? 1 : 2)) {
                        newMask |= (1 << l);
                        activeCount++;
                    }
                }
            }
            n.mask = newMask ? newMask : 1;
        } else if (diffIdx == 2) {
            // HARD: All 5 frets, cap chords to at most 2 notes
            int activeCount = 0;
            int newMask = 0;
            for (int l = 0; l < 5; l++) {
                if (mask & (1 << l)) {
                    if (activeCount < 2) {
                        newMask |= (1 << l);
                        activeCount++;
                    }
                }
            }
            n.mask = newMask ? newMask : 1;
        }

        filtered.push_back(n);
        lastTime = n.time;
    }

    if (!filtered.empty()) {
        chart.notes.swap(filtered);
    }
}

// Assigns Clone Hero Star Power phrases (clusters of 6-8 notes every ~15s)
inline void assignStarPowerPhrases(Chart& chart) {
    if (chart.notes.empty()) return;
    int phraseId = 1;
    double nextPhraseStart = chart.notes.front().time + 8.0;
    size_t i = 0;
    while (i < chart.notes.size()) {
        if (chart.notes[i].time >= nextPhraseStart) {
            double phraseEnd = chart.notes[i].time + 3.2;
            int count = 0;
            size_t j = i;
            while (j < chart.notes.size() && chart.notes[j].time <= phraseEnd && count < 9) {
                chart.notes[j].isStarPower = true;
                chart.notes[j].spPhraseId = phraseId;
                count++;
                j++;
            }
            if (count > 0) {
                phraseId++;
                nextPhraseStart = chart.notes[j - 1].time + 14.0;
                i = j;
                continue;
            }
        }
        i++;
    }
}


inline bool loadChart(const std::string& path, int diffIdx, Chart& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;

    const char* secNames[4] = {"EasySingle", "MediumSingle", "HardSingle", "ExpertSingle"};
    int clampedDiff = std::max(0, std::min(3, diffIdx));

    double res = 192.0, bpm = 120.0, offset = 0.0;
    struct Tempo { int tick; double bpm; double time; };
    std::vector<Tempo> tempos = {{0, 120.0, 0.0}};
    std::map<int, std::pair<int, int>> diffNotes[4];
    std::vector<std::pair<int, std::string>> rawSections;
    std::string curSec;
    char line[512];

    while (fgets(line, sizeof(line), f)) {
        char* s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '[') {
            char* cl = strchr(s, ']');
            if (cl) { *cl = 0; curSec = s + 1; }
            continue;
        }
        if (curSec == "Song") {
            int r = 0; double off = 0;
            if (sscanf(s, "Resolution = %d", &r) == 1 && r > 0) res = r;
            if (sscanf(s, "Offset = %lf", &off) == 1) offset = off;
        } else if (curSec == "SyncTrack") {
            int tick = 0, bval = 0;
            if (sscanf(s, "%d = B %d", &tick, &bval) == 2) {
                double nbpm = bval / 1000.0;
                if (tick == 0) { tempos[0].bpm = nbpm; bpm = nbpm; }
                else {
                    Tempo& prev = tempos.back();
                    double dt = (tick - prev.tick) * (60.0 / (res * prev.bpm));
                    tempos.push_back({tick, nbpm, prev.time + dt});
                }
            }
        } else if (curSec == "Events") {
            int tick = 0;
            char evBuf[256] = {0};
            if (sscanf(s, "%d = E %255[^\r\n]", &tick, evBuf) == 2) {
                std::string secName = parseSectionEvent(evBuf);
                if (!secName.empty()) rawSections.push_back({tick, secName});
            }
        } else {
            for (int d = 0; d < 4; d++) {
                if (curSec == secNames[d]) {
                    int tick = 0, lane = 0, sus = 0;
                    if (sscanf(s, "%d = N %d %d", &tick, &lane, &sus) == 3) {
                        if (lane >= 0 && lane <= 4) {
                            diffNotes[d][tick].first |= (1 << lane);
                            if (sus > diffNotes[d][tick].second) diffNotes[d][tick].second = sus;
                        } else if (lane == 7) {
                            diffNotes[d][tick].first |= (1 << 5);
                        }
                    }
                }
            }
        }
    }
    fclose(f);

    int chosenDiff = clampedDiff;
    bool wasFallback = false;
    if (diffNotes[chosenDiff].empty()) {
        for (int d = 3; d >= 0; d--) {
            if (!diffNotes[d].empty()) {
                chosenDiff = d;
                wasFallback = (d != clampedDiff);
                break;
            }
        }
    }
    if (diffNotes[chosenDiff].empty()) return false;

    auto tickToSec = [&](int t) {
        for (int i = (int)tempos.size() - 1; i >= 0; i--) {
            if (tempos[i].tick <= t) {
                return tempos[i].time + (t - tempos[i].tick) * (60.0 / (res * tempos[i].bpm)) + offset;
            }
        }
        return offset;
    };

    out.bpm = bpm;
    out.notes.clear();
    for (auto& kv : diffNotes[chosenDiff]) {
        if (kv.second.first == 0) continue;
        Note n;
        n.time = tickToSec(kv.first);
        n.mask = kv.second.first;
        n.sustain = (kv.second.second / (res * bpm)) * 60.0;
        if (n.sustain < 0.08) n.sustain = 0.0;
        out.notes.push_back(n);
    }

    out.sections.clear();
    for (auto& rs : rawSections) {
        double secTime = std::max(0.0, tickToSec(rs.first));
        if (out.sections.empty() || std::fabs(out.sections.back().time - secTime) > 1.0) {
            out.sections.push_back({secTime, rs.second});
        }
    }

    applyDifficultyFilter(out, clampedDiff, wasFallback);
    assignStarPowerPhrases(out);
    return !out.notes.empty();
}

inline uint32_t readBE32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
inline uint16_t readBE16(const uint8_t* p) {
    return ((uint16_t)p[0] << 8) | p[1];
}
inline uint32_t readVLQ(const uint8_t*& p, const uint8_t* end) {
    uint32_t v = 0;
    while (p < end) {
        uint8_t b = *p++;
        v = (v << 7) | (b & 0x7F);
        if (!(b & 0x80)) break;
    }
    return v;
}

inline bool loadMidi(const std::string& path, int diffIdx, Chart& out, std::string* usedTrack = nullptr) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 18) { fclose(f); return false; }
    std::vector<uint8_t> buf(sz);
    if (fread(buf.data(), 1, sz, f) != (size_t)sz) { fclose(f); return false; }
    fclose(f);

    const uint8_t* data = buf.data();
    if (memcmp(data, "MThd", 4) != 0) return false;
    uint16_t numTracks = readBE16(data + 10);
    uint16_t division = readBE16(data + 12);
    if (division == 0) division = 480;

    struct TrackSlice { const uint8_t* start; const uint8_t* end; std::string name; };
    std::vector<TrackSlice> tracks;
    const uint8_t* scan = data + 14;
    const uint8_t* fileEnd = data + sz;

    for (int t = 0; t < numTracks && scan + 8 <= fileEnd; t++) {
        uint32_t tlen = readBE32(scan + 4);
        const uint8_t* tstart = scan + 8;
        const uint8_t* tend = (tstart + tlen <= fileEnd) ? (tstart + tlen) : fileEnd;
        std::string tname = "Track";
        for (const uint8_t* p = tstart; p + 4 < tend; p++) {
            if (p[0] == 0xFF && p[1] == 0x03) {
                uint8_t nlen = p[2];
                if (p + 3 + nlen <= tend) tname.assign((const char*)(p + 3), nlen);
                break;
            }
        }
        tracks.push_back({tstart, tend, tname});
        scan = tend;
    }
    if (tracks.empty()) return false;

    // Pass 1: Tempo Map from Track 0
    struct Tempo { uint32_t tick; double bpm; double time; };
    std::vector<Tempo> tempos = {{0, 120.0, 0.0}};
    {
        const uint8_t* p = tracks[0].start;
        const uint8_t* end = tracks[0].end;
        uint32_t curTick = 0;
        double curTime = 0.0, curBpm = 120.0;
        uint8_t runSt = 0;
        while (p < end) {
            uint32_t delta = readVLQ(p, end);
            curTick += delta;
            curTime += delta * (60.0 / (division * curBpm));
            if (p >= end) break;
            uint8_t st = *p;
            if (st >= 0x80) { runSt = st; p++; } else st = runSt;
            uint8_t ev = st & 0xF0;
            if (st == 0xFF) {
                if (p >= end) break;
                uint8_t meta = *p++;
                uint32_t mlen = readVLQ(p, end);
                if (meta == 0x51 && mlen == 3 && p + 3 <= end) {
                    uint32_t us = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
                    if (us > 0) curBpm = 60000000.0 / us;
                    if (curTick == 0) tempos[0] = {0, curBpm, 0.0};
                    else tempos.push_back({curTick, curBpm, curTime});
                }
                p += mlen;
            } else if (st == 0xF0 || st == 0xF7) {
                uint32_t slen = readVLQ(p, end);
                p += slen;
            } else if (ev == 0x80 || ev == 0x90 || ev == 0xA0 || ev == 0xB0 || ev == 0xE0) p += 2;
            else if (ev == 0xC0 || ev == 0xD0) p += 1;
        }
    }

    auto tickToSec = [&](uint32_t t) {
        for (int i = (int)tempos.size() - 1; i >= 0; i--) {
            if (tempos[i].tick <= t) {
                return tempos[i].time + (t - tempos[i].tick) * (60.0 / (division * tempos[i].bpm));
            }
        }
        return 0.0;
    };

    // Pass 1.5: Extract Section / Setlist Markers from EVENTS / Track 0
    out.sections.clear();
    for (size_t ti = 0; ti < tracks.size(); ti++) {
        const uint8_t* p = tracks[ti].start;
        const uint8_t* end = tracks[ti].end;
        uint32_t curTick = 0;
        uint8_t runSt = 0;
        while (p < end) {
            curTick += readVLQ(p, end);
            if (p >= end) break;
            uint8_t st = *p;
            if (st >= 0x80) { runSt = st; p++; } else st = runSt;
            uint8_t ev = st & 0xF0;
            if (st == 0xFF) {
                if (p >= end) break;
                uint8_t meta = *p++;
                uint32_t mlen = readVLQ(p, end);
                if ((meta == 0x01 || meta == 0x06) && mlen > 0 && p + mlen <= end) {
                    std::string raw((const char*)p, mlen);
                    std::string secName = parseSectionEvent(raw);
                    if (!secName.empty()) {
                        double stime = tickToSec(curTick);
                        if (out.sections.empty() || std::fabs(out.sections.back().time - stime) > 1.0) {
                            out.sections.push_back({stime, secName});
                        }
                    }
                }
                p += mlen;
            } else if (st == 0xF0 || st == 0xF7) {
                uint32_t slen = readVLQ(p, end);
                p += slen;
            } else if (ev == 0x80 || ev == 0x90 || ev == 0xA0 || ev == 0xB0 || ev == 0xE0) p += 2;
            else if (ev == 0xC0 || ev == 0xD0) p += 1;
        }
    }
    std::sort(out.sections.begin(), out.sections.end(), [](const Section& a, const Section& b) {
        return a.time < b.time;
    });

    // Find PART GUITAR track
    int gIdx = -1;
    for (size_t i = 0; i < tracks.size(); i++) {
        if (tracks[i].name.find("PART GUITAR") != std::string::npos || tracks[i].name.find("T1 GEMS") != std::string::npos) {
            gIdx = (int)i; break;
        }
    }
    if (gIdx < 0 && tracks.size() > 1) gIdx = 1;
    if (gIdx < 0) return false;
    if (usedTrack) *usedTrack = tracks[gIdx].name;

    int clampedDiff = std::max(0, std::min(3, diffIdx));
    const int bases[4] = {60, 72, 84, 96};

    for (int attempt = 0; attempt < 4; attempt++) {
        int d = (attempt == 0) ? clampedDiff : (3 - (attempt - 1));
        int basePitch = bases[d];
        std::map<uint32_t, std::pair<int, uint32_t>> grouped;
        std::map<int, uint32_t> activePitches;

        const uint8_t* p = tracks[gIdx].start;
        const uint8_t* end = tracks[gIdx].end;
        uint32_t curTick = 0;
        uint8_t runSt = 0;

        while (p < end) {
            curTick += readVLQ(p, end);
            if (p >= end) break;
            uint8_t st = *p;
            if (st >= 0x80) { runSt = st; p++; } else st = runSt;
            uint8_t ev = st & 0xF0;

            if (st == 0xFF) {
                if (p >= end) break;
                p++;
                uint32_t mlen = readVLQ(p, end);
                p += mlen;
            } else if (st == 0xF0 || st == 0xF7) {
                uint32_t slen = readVLQ(p, end);
                p += slen;
            } else if (ev == 0x90 && p + 2 <= end) {
                int pitch = *p++;
                int vel = *p++;
                if (vel > 0) {
                    activePitches[pitch] = curTick;
                } else if (activePitches.count(pitch)) {
                    uint32_t sTick = activePitches[pitch];
                    activePitches.erase(pitch);
                    if (pitch >= basePitch && pitch <= basePitch + 4) {
                        grouped[sTick].first |= (1 << (pitch - basePitch));
                        grouped[sTick].second = std::max(grouped[sTick].second, curTick - sTick);
                    }
                }
            } else if (ev == 0x80 && p + 2 <= end) {
                int pitch = *p++;
                p++;
                if (activePitches.count(pitch)) {
                    uint32_t sTick = activePitches[pitch];
                    activePitches.erase(pitch);
                    if (pitch >= basePitch && pitch <= basePitch + 4) {
                        grouped[sTick].first |= (1 << (pitch - basePitch));
                        grouped[sTick].second = std::max(grouped[sTick].second, curTick - sTick);
                    }
                }
            } else if (ev == 0xA0 || ev == 0xB0 || ev == 0xE0) p += 2;
            else if (ev == 0xC0 || ev == 0xD0) p += 1;
        }

        // If native difficulty track has at least 15 notes (some charts put 1-2 dummy notes in lower diffs!), use it
        if (grouped.size() >= 15 || (attempt == 3 && !grouped.empty())) {
            bool wasFallback = (d != clampedDiff);
            out.bpm = tempos[0].bpm;
            out.notes.clear();
            uint32_t prevTick = 0;
            int prevMask = 0;
            for (auto& kv : grouped) {
                Note n;
                n.time = tickToSec(kv.first);
                n.mask = kv.second.first;
                double susSec = tickToSec(kv.first + kv.second.second) - n.time;
                n.sustain = (susSec > 0.12) ? susSec : 0.0;
                bool single = (n.mask & (n.mask - 1)) == 0;
                bool prevSingle = (prevMask & (prevMask - 1)) == 0;
                n.isHopo = single && prevSingle && n.mask != prevMask && (kv.first - prevTick <= (uint32_t)(division / 2));
                out.notes.push_back(n);
                prevTick = kv.first;
                prevMask = n.mask;
            }
            applyDifficultyFilter(out, clampedDiff, wasFallback);
            assignStarPowerPhrases(out);
            return true;
        }
    }
    return false;
}
