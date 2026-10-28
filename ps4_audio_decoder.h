#pragma once
// =================================================================================
// PS4 CLONE HERO - UNIVERSAL MULTI-FORMAT 48kHz STEREO AUDIO DECODER
// =================================================================================
// Native PS4 zero-stutter decoding for:
//   1. .opus (Ogg Opus via libopus 1.4 + RFC 7845 Ogg packet demuxer, native 48kHz)
//      - Uses a pre-allocated 65,536-frame lock-free power-of-2 ring buffer
//      - Fast 27-byte direct Ogg page header reads (no per-byte fgetc overhead)
//      - Zero heap allocations or vector memmoves during real-time playback!
//   2. .ogg  (Ogg Vorbis via stb_vorbis, with automatic fallback if .ogg holds Opus)
//   3. .mp3  (MPEG-1/2/2.5 Layer III via dr_mp3.h, resampled to 48kHz stereo)
//   4. .flac (Free Lossless Audio Codec via dr_flac.h, resampled to 48kHz stereo)
//   5. .wav  (16/24/32-bit PCM & IEEE float WAV via dr_wav.h, resampled to 48kHz)
// =================================================================================

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>
#include <string>
#include <algorithm>

#include <opus.h>
#include <opus_multistream.h>

#define DR_MP3_NO_SIMD
#define DR_MP3_NO_WCHAR
#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"

#define DR_FLAC_NO_SIMD
#define DR_FLAC_NO_WCHAR
#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"

#define DR_WAV_NO_WCHAR
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

extern "C" {
typedef struct stb_vorbis stb_vorbis;
typedef struct {
    unsigned int sample_rate;
    int channels;
    unsigned int setup_memory_required;
    unsigned int setup_temp_memory_required;
    unsigned int temp_memory_required;
    int max_frame_size;
} stb_vorbis_info;

stb_vorbis* stb_vorbis_open_filename(const char* filename, int* error, const void* alloc_buffer);
stb_vorbis* stb_vorbis_open_memory(const unsigned char *data, int len, int *error, const void *alloc_buffer);
stb_vorbis_info stb_vorbis_get_info(stb_vorbis* f);
int stb_vorbis_get_samples_short_interleaved(stb_vorbis* f, int channels, short* buffer, int num_shorts);
int stb_vorbis_seek_start(stb_vorbis* f);
int stb_vorbis_seek(stb_vorbis* f, unsigned int sample_number);
unsigned int stb_vorbis_stream_length_in_samples(stb_vorbis* f);
void stb_vorbis_close(stb_vorbis* f);
}

// =================================================================================
// NATIVE OGG OPUS ZERO-STUTTER STREAMING DECODER (RFC 7845 + libopus)
// =================================================================================
class OggOpusFileDecoder {
public:
    static const size_t RING_CAP_FRAMES = 131072; // Power of 2 (~2.73 sec @ 48kHz)
    static const size_t RING_FRAME_MASK = RING_CAP_FRAMES - 1;

    FILE* fp = nullptr;
    const uint8_t* memData = nullptr;
    size_t memSize = 0;
    size_t memOffset = 0;
    size_t memFirstAudioPageOffset = 0;

    OpusDecoder* dec = nullptr;
    OpusMSDecoder* msDec = nullptr;
    int channels = 2;
    int mappingFamily = 0;
    uint16_t preSkip = 0;
    uint16_t remainingPreSkip = 0;
    int16_t gainQ8 = 0;
    float gainScale = 1.0f;
    uint64_t totalSamples48k = 0;
    long firstAudioPageOffset = 0;
    bool eofReached = false;

    // Fast Page Buffer (Reads entire Ogg page body in 1 sequential read, eliminating 50+ tiny freadds per page!)
    uint8_t segTable[256];
    int numSegs = 0;
    int segIdx = 0;
    std::vector<uint8_t> pageBody;
    size_t pageBodyLen = 0;
    size_t pageBodyOffset = 0;
    std::vector<uint8_t> packetBuf;
    std::vector<uint8_t> workPkt;

    // Pre-allocated 48kHz stereo interleaved power-of-2 ring buffer (zero erase/memmove!)
    std::vector<int16_t> pcmRing;
    size_t ringReadFrame = 0;
    size_t ringWriteFrame = 0;
    std::vector<int16_t> decodeScratch;

    OggOpusFileDecoder() {
        packetBuf.reserve(65536);
        workPkt.reserve(65536);
        pageBody.resize(65536);
        pcmRing.resize(RING_CAP_FRAMES * 2, 0);
        decodeScratch.resize(5760 * 8);
    }

    ~OggOpusFileDecoder() {
        close();
    }

    void close() {
        if (dec) { opus_decoder_destroy(dec); dec = nullptr; }
        if (msDec) { opus_multistream_decoder_destroy(msDec); msDec = nullptr; }
        if (fp) { fclose(fp); fp = nullptr; }
        memData = nullptr;
        memSize = 0;
        memOffset = 0;
        memFirstAudioPageOffset = 0;
        ringReadFrame = 0;
        ringWriteFrame = 0;
        packetBuf.clear();
        workPkt.clear();
        numSegs = 0;
        segIdx = 0;
        pageBodyLen = 0;
        pageBodyOffset = 0;
        eofReached = false;
    }

    inline size_t availRingFrames() const {
        return ringWriteFrame - ringReadFrame;
    }

    bool readNextPage() {
        if (memData) {
            if (memOffset + 27 > memSize) return false;
            const uint8_t* hdr = memData + memOffset;
            if (hdr[0] == 'O' && hdr[1] == 'g' && hdr[2] == 'g' && hdr[3] == 'S') {
                numSegs = hdr[26];
                segIdx = 0;
                pageBodyLen = 0;
                pageBodyOffset = 0;
                memOffset += 27;
                if (numSegs > 0) {
                    if (memOffset + numSegs > memSize) return false;
                    memcpy(segTable, memData + memOffset, numSegs);
                    memOffset += numSegs;
                    for (int s = 0; s < numSegs; s++) pageBodyLen += segTable[s];
                    if (pageBodyLen > 0) {
                        if (memOffset + pageBodyLen > memSize) return false;
                        if (pageBody.size() < pageBodyLen) pageBody.resize(pageBodyLen);
                        memcpy(pageBody.data(), memData + memOffset, pageBodyLen);
                        memOffset += pageBodyLen;
                    }
                }
                return true;
            }
            // Memory resync scan
            while (memOffset + 4 <= memSize) {
                if (memData[memOffset] == 'O' && memData[memOffset+1] == 'g' && memData[memOffset+2] == 'g' && memData[memOffset+3] == 'S') {
                    return readNextPage();
                }
                memOffset++;
            }
            return false;
        }

        if (!fp) return false;
        uint8_t hdr[27];

        // Fast path: 99.999% of Ogg pages start immediately at the current file position
        size_t got = fread(hdr, 1, 27, fp);
        if (got == 27 && hdr[0] == 'O' && hdr[1] == 'g' && hdr[2] == 'g' && hdr[3] == 'S') {
            numSegs = hdr[26];
            segIdx = 0;
            pageBodyLen = 0;
            pageBodyOffset = 0;
            if (numSegs > 0) {
                if (fread(segTable, 1, numSegs, fp) != (size_t)numSegs) return false;
                for (int i = 0; i < numSegs; i++) pageBodyLen += segTable[i];
                if (pageBodyLen > 0) {
                    if (pageBody.size() < pageBodyLen) pageBody.resize(pageBodyLen);
                    if (fread(pageBody.data(), 1, pageBodyLen, fp) != pageBodyLen) return false;
                }
            }
            return true;
        }
        if (got < 4) return false;

        // Slow fallback path: scan ahead in blocks if mid-stream padding exists
        fseek(fp, -(long)got, SEEK_CUR);
        uint8_t scanBuf[4096];
        while (true) {
            long basePos = ftell(fp);
            size_t n = fread(scanBuf, 1, sizeof(scanBuf), fp);
            if (n < 27) return false;
            for (size_t i = 0; i + 4 <= n; ++i) {
                if (scanBuf[i] == 'O' && scanBuf[i + 1] == 'g' && scanBuf[i + 2] == 'g' && scanBuf[i + 3] == 'S') {
                    fseek(fp, basePos + (long)i, SEEK_SET);
                    if (fread(hdr, 1, 27, fp) != 27) return false;
                    numSegs = hdr[26];
                    segIdx = 0;
                    pageBodyLen = 0;
                    pageBodyOffset = 0;
                    if (numSegs > 0) {
                        if (fread(segTable, 1, numSegs, fp) != (size_t)numSegs) return false;
                        for (int s = 0; s < numSegs; s++) pageBodyLen += segTable[s];
                        if (pageBodyLen > 0) {
                            if (pageBody.size() < pageBodyLen) pageBody.resize(pageBodyLen);
                            if (fread(pageBody.data(), 1, pageBodyLen, fp) != pageBodyLen) return false;
                        }
                    }
                    return true;
                }
            }
            fseek(fp, basePos + (long)n - 3, SEEK_SET);
        }
    }

    bool readNextPacket(std::vector<uint8_t>& outPkt) {
        outPkt.clear();
        while (true) {
            while (segIdx < numSegs) {
                uint8_t segLen = segTable[segIdx++];
                if (segLen > 0) {
                    size_t oldSz = packetBuf.size();
                    packetBuf.resize(oldSz + segLen);
                    if (pageBodyOffset + segLen <= pageBodyLen) {
                        memcpy(packetBuf.data() + oldSz, pageBody.data() + pageBodyOffset, segLen);
                        pageBodyOffset += segLen;
                    }
                }
                if (segLen < 255) {
                    outPkt.swap(packetBuf);
                    packetBuf.clear();
                    return true;
                }
            }
            if (!readNextPage()) {
                if (!packetBuf.empty()) {
                    outPkt.swap(packetBuf);
                    packetBuf.clear();
                    return true;
                }
                return false;
            }
        }
    }

    uint64_t findLastGranulePosition() {
        if (memData) {
            if (memSize <= 27) return 0;
            size_t scanLen = (memSize > 65536) ? 65536 : memSize;
            size_t start = memSize - scanLen;
            for (size_t i = memSize - 27; i >= start; --i) {
                if (memData[i] == 'O' && memData[i+1] == 'g' && memData[i+2] == 'g' && memData[i+3] == 'S' && memData[i+4] == 0) {
                    uint64_t gp = 0;
                    for (int b = 0; b < 8; ++b) {
                        gp |= ((uint64_t)memData[i + 6 + b]) << (b * 8);
                    }
                    if (gp != 0xFFFFFFFFFFFFFFFFULL && gp > 0) return gp;
                }
                if (i == 0) break;
            }
            return 0;
        }

        if (!fp) return 0;
        long savedPos = ftell(fp);
        fseek(fp, 0, SEEK_END);
        long fileSize = ftell(fp);
        if (fileSize <= 27) {
            fseek(fp, savedPos, SEEK_SET);
            return 0;
        }
        long scanLen = (fileSize > 65536L) ? 65536L : fileSize;
        fseek(fp, fileSize - scanLen, SEEK_SET);
        std::vector<uint8_t> tail((size_t)scanLen);
        size_t got = fread(tail.data(), 1, (size_t)scanLen, fp);
        fseek(fp, savedPos, SEEK_SET);

        uint64_t lastGranule = 0;
        if (got >= 27) {
            for (long i = (long)got - 27; i >= 0; --i) {
                if (tail[i] == 'O' && tail[i + 1] == 'g' && tail[i + 2] == 'g' && tail[i + 3] == 'S' && tail[i + 4] == 0) {
                    uint64_t gp = 0;
                    for (int b = 0; b < 8; ++b) {
                        gp |= ((uint64_t)tail[i + 6 + b]) << (b * 8);
                    }
                    if (gp != 0xFFFFFFFFFFFFFFFFULL && gp > 0) {
                        lastGranule = gp;
                        break;
                    }
                }
            }
        }
        return lastGranule;
    }

    bool openMemory(const uint8_t* data, size_t size) {
        close();
        if (!data || size < 27) return false;
        memData = data;
        memSize = size;
        memOffset = 0;

        if (!readNextPacket(workPkt)) { close(); return false; }
        if (workPkt.size() < 19 || memcmp(workPkt.data(), "OpusHead", 8) != 0) {
            close();
            return false;
        }

        channels = workPkt[9];
        if (channels < 1 || channels > 8) { close(); return false; }
        preSkip = (uint16_t)(workPkt[10] | (workPkt[11] << 8));
        remainingPreSkip = preSkip;
        gainQ8 = (int16_t)(workPkt[16] | (workPkt[17] << 8));
        gainScale = (gainQ8 != 0) ? powf(10.0f, (float)gainQ8 / (20.0f * 256.0f)) : 1.0f;
        mappingFamily = workPkt[18];

        int err = OPUS_OK;
        if (mappingFamily == 0 || workPkt.size() < 21) {
            int decCh = (channels >= 2) ? 2 : 1;
            dec = opus_decoder_create(48000, decCh, &err);
            if (err != OPUS_OK || !dec) { close(); return false; }
            channels = decCh;
        } else {
            int nbStreams = workPkt[19];
            int nbCoupled = workPkt[20];
            if (nbStreams < 1 || workPkt.size() < (size_t)(21 + channels)) { close(); return false; }
            const unsigned char* mapping = workPkt.data() + 21;
            msDec = opus_multistream_decoder_create(48000, channels, nbStreams, nbCoupled, mapping, &err);
            if (err != OPUS_OK || !msDec) { close(); return false; }
        }

        if (!readNextPacket(workPkt)) { close(); return false; }
        memFirstAudioPageOffset = memOffset;

        uint64_t lastGp = findLastGranulePosition();
        if (lastGp > preSkip) totalSamples48k = lastGp - preSkip;
        else totalSamples48k = 48000ULL * 180ULL;

        // Pre-fill 96,000 stereo frames (2.0s) in RAM
        ensureBufferFilled(96000, 128);
        return true;
    }

    bool open(const char* path) {
        close();
        fp = fopen(path, "rb");
        if (!fp) return false;
        setvbuf(fp, NULL, _IOFBF, 262144); // 256KB read-ahead cache eliminates PS4 HDD/USB I/O stalls

        if (!readNextPacket(workPkt)) { close(); return false; }

        // Packet 0 must be "OpusHead" (at least 19 bytes)
        if (workPkt.size() < 19 || memcmp(workPkt.data(), "OpusHead", 8) != 0) {
            close();
            return false;
        }

        channels = workPkt[9];
        if (channels < 1 || channels > 8) { close(); return false; }
        preSkip = (uint16_t)(workPkt[10] | (workPkt[11] << 8));
        remainingPreSkip = preSkip;
        gainQ8 = (int16_t)(workPkt[16] | (workPkt[17] << 8));
        gainScale = (gainQ8 != 0) ? powf(10.0f, (float)gainQ8 / (20.0f * 256.0f)) : 1.0f;
        mappingFamily = workPkt[18];

        int err = OPUS_OK;
        if (mappingFamily == 0 || workPkt.size() < 21) {
            int decCh = (channels >= 2) ? 2 : 1;
            dec = opus_decoder_create(48000, decCh, &err);
            if (err != OPUS_OK || !dec) { close(); return false; }
            channels = decCh;
        } else {
            int nbStreams = workPkt[19];
            int nbCoupled = workPkt[20];
            if (nbStreams < 1 || workPkt.size() < (size_t)(21 + channels)) { close(); return false; }
            const unsigned char* mapping = workPkt.data() + 21;
            msDec = opus_multistream_decoder_create(48000, channels, nbStreams, nbCoupled, mapping, &err);
            if (err != OPUS_OK || !msDec) { close(); return false; }
        }

        // Packet 1 is "OpusTags"
        if (!readNextPacket(workPkt)) { close(); return false; }
        firstAudioPageOffset = ftell(fp);

        uint64_t lastGp = findLastGranulePosition();
        if (lastGp > preSkip) totalSamples48k = lastGp - preSkip;
        else totalSamples48k = 48000ULL * 180ULL;

        // Pre-fill 96,000 stereo frames (~2000ms) right at open time for zero-stutter startup
        ensureBufferFilled(96000, 128);
        return true;
    }

    inline void pushFrameToRing(int16_t L, int16_t R) {
        size_t slot = (ringWriteFrame & RING_FRAME_MASK) * 2;
        pcmRing[slot]     = L;
        pcmRing[slot + 1] = R;
        ringWriteFrame++;
    }

    void ensureBufferFilled(size_t targetFrames = 8192, int maxPacketsPerCall = 4) {
        int packetsDecoded = 0;
        while (!eofReached && packetsDecoded < maxPacketsPerCall) {
            size_t avail = availRingFrames();
            if (avail >= targetFrames || avail + 5760 >= RING_CAP_FRAMES) break;

            if (!readNextPacket(workPkt)) {
                eofReached = true;
                break;
            }
            if (workPkt.empty()) continue;
            if (workPkt.size() >= 8 && (memcmp(workPkt.data(), "OpusHead", 8) == 0 || memcmp(workPkt.data(), "OpusTags", 8) == 0)) {
                continue;
            }

            int decodedFrames = 0;
            if (dec) {
                decodedFrames = opus_decode(dec, workPkt.data(), (opus_int32)workPkt.size(), decodeScratch.data(), 5760, 0);
            } else if (msDec) {
                decodedFrames = opus_multistream_decode(msDec, workPkt.data(), (opus_int32)workPkt.size(), decodeScratch.data(), 5760, 0);
            }
            if (decodedFrames <= 0) continue;
            packetsDecoded++;

            int startFrame = 0;
            if (remainingPreSkip > 0) {
                int skip = std::min((int)remainingPreSkip, decodedFrames);
                startFrame = skip;
                remainingPreSkip -= (uint16_t)skip;
            }
            int validFrames = decodedFrames - startFrame;
            if (validFrames <= 0) continue;

            const int16_t* src = decodeScratch.data() + startFrame * channels;

            if (channels == 1) {
                for (int i = 0; i < validFrames; ++i) {
                    int32_t s = src[i];
                    if (gainQ8 != 0) {
                        s = (int32_t)(s * gainScale);
                        if (s > 32767) s = 32767;
                        else if (s < -32768) s = -32768;
                    }
                    pushFrameToRing((int16_t)s, (int16_t)s);
                }
            } else if (channels == 2) {
                if (gainQ8 == 0) {
                    for (int i = 0; i < validFrames; ++i) {
                        pushFrameToRing(src[i * 2], src[i * 2 + 1]);
                    }
                } else {
                    for (int i = 0; i < validFrames; ++i) {
                        int32_t L = (int32_t)(src[i * 2] * gainScale);
                        int32_t R = (int32_t)(src[i * 2 + 1] * gainScale);
                        if (L > 32767) L = 32767; else if (L < -32768) L = -32768;
                        if (R > 32767) R = 32767; else if (R < -32768) R = -32768;
                        pushFrameToRing((int16_t)L, (int16_t)R);
                    }
                }
            } else {
                for (int i = 0; i < validFrames; ++i) {
                    int32_t L = src[i * channels + 0];
                    int32_t R = src[i * channels + 1];
                    if (channels >= 3) {
                        int32_t C = src[i * channels + 2] / 2;
                        L += C; R += C;
                    }
                    if (gainQ8 != 0) {
                        L = (int32_t)(L * gainScale);
                        R = (int32_t)(R * gainScale);
                    }
                    if (L > 32767) L = 32767; else if (L < -32768) L = -32768;
                    if (R > 32767) R = 32767; else if (R < -32768) R = -32768;
                    pushFrameToRing((int16_t)L, (int16_t)R);
                }
            }
        }
    }

    void seekStart() {
        if (memData) {
            memOffset = memFirstAudioPageOffset;
            numSegs = 0;
            segIdx = 0;
            packetBuf.clear();
            workPkt.clear();
            ringReadFrame = 0;
            ringWriteFrame = 0;
            eofReached = false;
            remainingPreSkip = preSkip;
            if (dec) opus_decoder_ctl(dec, OPUS_RESET_STATE);
            if (msDec) opus_multistream_decoder_ctl(msDec, OPUS_RESET_STATE);
            ensureBufferFilled(96000, 128);
            return;
        }

        if (!fp) return;
        fseek(fp, 0, SEEK_SET);
        numSegs = 0;
        segIdx = 0;
        packetBuf.clear();
        workPkt.clear();
        ringReadFrame = 0;
        ringWriteFrame = 0;
        eofReached = false;
        remainingPreSkip = preSkip;

        readNextPacket(workPkt); // Skip OpusHead
        readNextPacket(workPkt); // Skip OpusTags
        if (dec) opus_decoder_ctl(dec, OPUS_RESET_STATE);
        if (msDec) opus_multistream_decoder_ctl(msDec, OPUS_RESET_STATE);
        ensureBufferFilled(96000, 128);
    }

    // Read interleaved 48kHz stereo samples (returns number of stereo frames read)
    int readStereoFrames(int16_t* outStereo, int framesWanted) {
        if (availRingFrames() < 96000 && !eofReached) {
            ensureBufferFilled(96000, 1);
        }
        size_t avail = availRingFrames();
        int take = (int)std::min((size_t)framesWanted, avail);
        for (int i = 0; i < take; ++i) {
            size_t slot = (ringReadFrame & RING_FRAME_MASK) * 2;
            outStereo[i * 2]     = pcmRing[slot];
            outStereo[i * 2 + 1] = pcmRing[slot + 1];
            ringReadFrame++;
        }
        return take;
    }

    void skipFrames(uint64_t framesToSkip) {
        int16_t discard[1024 * 2];
        uint64_t rem = framesToSkip;
        while (rem > 0 && !eofReached) {
            ensureBufferFilled(32768, 16);
            int chunk = (int)std::min(rem, (uint64_t)1024);
            int got = readStereoFrames(discard, chunk);
            if (got <= 0) break;
            rem -= (uint64_t)got;
        }
    }
};

// =================================================================================
// UNIFIED MULTI-FORMAT 48kHz STEREO STREAMING DECODER
// =================================================================================
enum Ps4AudioFormat {
    PS4_AUDIO_NONE = 0,
    PS4_AUDIO_OPUS,
    PS4_AUDIO_VORBIS,
    PS4_AUDIO_MP3,
    PS4_AUDIO_FLAC,
    PS4_AUDIO_WAV
};

class Ps4AudioStream {
public:
    Ps4AudioFormat fmt = PS4_AUDIO_NONE;
    uint32_t srcSampleRate = 48000;
    uint32_t srcChannels = 2;
    uint64_t totalSamples48k = 0;

    std::vector<uint8_t> fileBytes;
    OggOpusFileDecoder* opusDec = nullptr;
    stb_vorbis* vorbisDec = nullptr;
    drmp3 mp3Dec;
    bool mp3Open = false;
    drflac* flacDec = nullptr;
    drwav wavDec;
    bool wavOpen = false;

    // Linear resampler state when srcSampleRate != 48000
    double resamplePos = 0.0;
    std::vector<int16_t> srcBuf;
    size_t srcFramesAvail = 0;

    Ps4AudioStream() {
        memset(&mp3Dec, 0, sizeof(mp3Dec));
        memset(&wavDec, 0, sizeof(wavDec));
        srcBuf.resize(4096 * 2);
    }

    ~Ps4AudioStream() {
        close();
    }

    void close() {
        if (opusDec) { delete opusDec; opusDec = nullptr; }
        if (vorbisDec) { stb_vorbis_close(vorbisDec); vorbisDec = nullptr; }
        if (mp3Open) { drmp3_uninit(&mp3Dec); mp3Open = false; }
        if (flacDec) { drflac_close(flacDec); flacDec = nullptr; }
        if (wavOpen) { drwav_uninit(&wavDec); wavOpen = false; }
        fileBytes.clear();
        fmt = PS4_AUDIO_NONE;
        resamplePos = 0.0;
        srcFramesAvail = 0;
    }

    static std::string getLowerExt(const std::string& path) {
        size_t dot = path.find_last_of('.');
        if (dot == std::string::npos) return "";
        std::string ext = path.substr(dot);
        for (char& c : ext) {
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        }
        return ext;
    }

    bool open(const std::string& path) {
        close();
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) return false;
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (fsize <= 0) { fclose(f); return false; }
        fileBytes.resize((size_t)fsize);
        size_t rd = fread(fileBytes.data(), 1, fileBytes.size(), f);
        fclose(f);
        if (rd != fileBytes.size()) { fileBytes.clear(); return false; }

        std::string ext = getLowerExt(path);

        // 1. Try Ogg Opus via fast RAM stream
        if (ext == ".opus") {
            opusDec = new OggOpusFileDecoder();
            if (opusDec->openMemory(fileBytes.data(), fileBytes.size())) {
                fmt = PS4_AUDIO_OPUS;
                srcSampleRate = 48000;
                srcChannels = 2;
                totalSamples48k = opusDec->totalSamples48k;
                return true;
            }
            delete opusDec;
            opusDec = nullptr;
        }

        // 2. Try Ogg Vorbis via stb_vorbis_open_memory (zero disk I/O during playback!)
        if (ext == ".ogg" || ext == ".opus") {
            int err = 0;
            vorbisDec = stb_vorbis_open_memory(fileBytes.data(), (int)fileBytes.size(), &err, nullptr);
            if (vorbisDec) {
                stb_vorbis_info info = stb_vorbis_get_info(vorbisDec);
                if (info.sample_rate > 0 && info.channels > 0) {
                    fmt = PS4_AUDIO_VORBIS;
                    srcSampleRate = info.sample_rate;
                    srcChannels = (uint32_t)info.channels;
                    uint64_t rawLen = stb_vorbis_stream_length_in_samples(vorbisDec);
                    totalSamples48k = (rawLen > 0) ? (rawLen * 48000ULL) / srcSampleRate : (48000ULL * 180ULL);
                    return true;
                }
                stb_vorbis_close(vorbisDec);
                vorbisDec = nullptr;
            }
            // Fallback: some songs name an Opus stream "song.ogg"!
            opusDec = new OggOpusFileDecoder();
            if (opusDec->openMemory(fileBytes.data(), fileBytes.size())) {
                fmt = PS4_AUDIO_OPUS;
                srcSampleRate = 48000;
                srcChannels = 2;
                totalSamples48k = opusDec->totalSamples48k;
                return true;
            }
            delete opusDec;
            opusDec = nullptr;
        }

        // 3. Try MP3 via dr_mp3 memory
        if (ext == ".mp3" || fmt == PS4_AUDIO_NONE) {
            if (ext == ".mp3" && drmp3_init_memory(&mp3Dec, fileBytes.data(), fileBytes.size(), nullptr)) {
                mp3Open = true;
                fmt = PS4_AUDIO_MP3;
                srcSampleRate = mp3Dec.sampleRate > 0 ? mp3Dec.sampleRate : 48000;
                srcChannels = mp3Dec.channels > 0 ? mp3Dec.channels : 2;
                uint64_t pcmFrames = drmp3_get_pcm_frame_count(&mp3Dec);
                totalSamples48k = (pcmFrames > 0) ? (pcmFrames * 48000ULL) / srcSampleRate : (48000ULL * 180ULL);
                return true;
            }
        }

        // 4. Try FLAC via dr_flac memory
        if (ext == ".flac") {
            flacDec = drflac_open_memory(fileBytes.data(), fileBytes.size(), nullptr);
            if (flacDec) {
                fmt = PS4_AUDIO_FLAC;
                srcSampleRate = flacDec->sampleRate > 0 ? flacDec->sampleRate : 48000;
                srcChannels = flacDec->channels > 0 ? flacDec->channels : 2;
                totalSamples48k = (flacDec->totalPCMFrameCount > 0)
                    ? (flacDec->totalPCMFrameCount * 48000ULL) / srcSampleRate
                    : (48000ULL * 180ULL);
                return true;
            }
        }

        // 5. Try WAV via dr_wav memory
        if (ext == ".wav") {
            if (drwav_init_memory(&wavDec, fileBytes.data(), fileBytes.size(), nullptr)) {
                wavOpen = true;
                fmt = PS4_AUDIO_WAV;
                srcSampleRate = wavDec.sampleRate > 0 ? wavDec.sampleRate : 48000;
                srcChannels = wavDec.channels > 0 ? wavDec.channels : 2;
                totalSamples48k = (wavDec.totalPCMFrameCount > 0)
                    ? (wavDec.totalPCMFrameCount * 48000ULL) / srcSampleRate
                    : (48000ULL * 180ULL);
                return true;
            }
        }

        fileBytes.clear();
        return false;
    }

    void seekStart() {
        resamplePos = 0.0;
        srcFramesAvail = 0;
        if (fmt == PS4_AUDIO_OPUS && opusDec) opusDec->seekStart();
        else if (fmt == PS4_AUDIO_VORBIS && vorbisDec) stb_vorbis_seek_start(vorbisDec);
        else if (fmt == PS4_AUDIO_MP3 && mp3Open) drmp3_seek_to_pcm_frame(&mp3Dec, 0);
        else if (fmt == PS4_AUDIO_FLAC && flacDec) drflac_seek_to_pcm_frame(flacDec, 0);
        else if (fmt == PS4_AUDIO_WAV && wavOpen) drwav_seek_to_pcm_frame(&wavDec, 0);
    }

    void seekSample48k(uint64_t targetSample48k) {
        resamplePos = 0.0;
        srcFramesAvail = 0;
        uint64_t targetSrc = (targetSample48k * srcSampleRate) / 48000ULL;
        if (fmt == PS4_AUDIO_OPUS && opusDec) {
            opusDec->seekStart();
            if (targetSample48k > 0) opusDec->skipFrames(targetSample48k);
        } else if (fmt == PS4_AUDIO_VORBIS && vorbisDec) {
            stb_vorbis_seek(vorbisDec, (unsigned int)targetSrc);
        } else if (fmt == PS4_AUDIO_MP3 && mp3Open) {
            drmp3_seek_to_pcm_frame(&mp3Dec, targetSrc);
        } else if (fmt == PS4_AUDIO_FLAC && flacDec) {
            drflac_seek_to_pcm_frame(flacDec, targetSrc);
        } else if (fmt == PS4_AUDIO_WAV && wavOpen) {
            drwav_seek_to_pcm_frame(&wavDec, targetSrc);
        }
    }

    // Reads raw stereo frames at srcSampleRate
    int readRawStereoFrames(int16_t* dstStereo, int framesWanted) {
        if (fmt == PS4_AUDIO_OPUS && opusDec) {
            return opusDec->readStereoFrames(dstStereo, framesWanted);
        }
        if (fmt == PS4_AUDIO_VORBIS && vorbisDec) {
            return stb_vorbis_get_samples_short_interleaved(vorbisDec, 2, dstStereo, framesWanted * 2);
        }
        if (fmt == PS4_AUDIO_MP3 && mp3Open) {
            if (srcChannels == 2) {
                return (int)drmp3_read_pcm_frames_s16(&mp3Dec, (drmp3_uint64)framesWanted, dstStereo);
            } else {
                std::vector<int16_t> tmp((size_t)framesWanted * srcChannels);
                int got = (int)drmp3_read_pcm_frames_s16(&mp3Dec, (drmp3_uint64)framesWanted, tmp.data());
                for (int i = 0; i < got; ++i) {
                    dstStereo[i * 2] = tmp[i * srcChannels];
                    dstStereo[i * 2 + 1] = tmp[i * srcChannels + (srcChannels > 1 ? 1 : 0)];
                }
                return got;
            }
        }
        if (fmt == PS4_AUDIO_FLAC && flacDec) {
            if (srcChannels == 2) {
                return (int)drflac_read_pcm_frames_s16(flacDec, (drflac_uint64)framesWanted, dstStereo);
            } else {
                std::vector<int16_t> tmp((size_t)framesWanted * srcChannels);
                int got = (int)drflac_read_pcm_frames_s16(flacDec, (drflac_uint64)framesWanted, tmp.data());
                for (int i = 0; i < got; ++i) {
                    dstStereo[i * 2] = tmp[i * srcChannels];
                    dstStereo[i * 2 + 1] = tmp[i * srcChannels + (srcChannels > 1 ? 1 : 0)];
                }
                return got;
            }
        }
        if (fmt == PS4_AUDIO_WAV && wavOpen) {
            if (srcChannels == 2) {
                return (int)drwav_read_pcm_frames_s16(&wavDec, (drwav_uint64)framesWanted, dstStereo);
            } else {
                std::vector<int16_t> tmp((size_t)framesWanted * srcChannels);
                int got = (int)drwav_read_pcm_frames_s16(&wavDec, (drwav_uint64)framesWanted, tmp.data());
                for (int i = 0; i < got; ++i) {
                    dstStereo[i * 2] = tmp[i * srcChannels];
                    dstStereo[i * 2 + 1] = tmp[i * srcChannels + (srcChannels > 1 ? 1 : 0)];
                }
                return got;
            }
        }
        return 0;
    }

    // Reads resampled 48,000 Hz stereo frames into outStereo (2 shorts per frame)
    int read48kStereoFrames(int16_t* outStereo, int frames48kWanted) {
        if (srcSampleRate == 48000 || srcSampleRate == 0) {
            return readRawStereoFrames(outStereo, frames48kWanted);
        }

        // Resample from srcSampleRate (e.g. 44100 Hz) to 48000 Hz
        double step = (double)srcSampleRate / 48000.0;
        int written = 0;

        while (written < frames48kWanted) {
            size_t idx = (size_t)resamplePos;
            if (idx + 1 >= srcFramesAvail) {
                // Keep last frame if available for interpolation continuity
                int keep = (srcFramesAvail > 0 && idx < srcFramesAvail) ? 1 : 0;
                if (keep == 1) {
                    srcBuf[0] = srcBuf[(srcFramesAvail - 1) * 2];
                    srcBuf[1] = srcBuf[(srcFramesAvail - 1) * 2 + 1];
                    resamplePos -= (double)(srcFramesAvail - 1);
                    if (resamplePos < 0.0) resamplePos = 0.0;
                } else {
                    resamplePos = 0.0;
                }
                int got = readRawStereoFrames(srcBuf.data() + keep * 2, 1024);
                if (got <= 0) break;
                srcFramesAvail = (size_t)(keep + got);
                continue;
            }

            double frac = resamplePos - (double)idx;
            int16_t L0 = srcBuf[idx * 2];
            int16_t R0 = srcBuf[idx * 2 + 1];
            int16_t L1 = srcBuf[(idx + 1) * 2];
            int16_t R1 = srcBuf[(idx + 1) * 2 + 1];

            outStereo[written * 2]     = (int16_t)(L0 + (L1 - L0) * frac);
            outStereo[written * 2 + 1] = (int16_t)(R0 + (R1 - R0) * frac);
            written++;
            resamplePos += step;
        }
        return written;
    }
};
