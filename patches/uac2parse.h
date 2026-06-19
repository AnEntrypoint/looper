// uac2parse.h - pure USB Audio Class 2.0 configuration-descriptor parser.
//
// No Circle/firmware dependencies (plain u8 buffer in, struct out) so it is
// host-testable (scripts/test-uac2-parse.cpp) and reused by the firmware UAC2
// host path (patches/usbaudiodevice.cpp). UAC2 needs more than UAC1's "select
// the streaming alt + grab the iso EP": the sample rate is set on a Clock Source
// ENTITY in the Audio Control interface (not an endpoint request), and the
// sample format (subslot size / bit resolution / channels) lives in the AS
// interface's class-specific descriptors. This walks the whole config once and
// extracts everything the host path needs, generically (no per-device hardcode).
#ifndef _uac2parse_h
#define _uac2parse_h

#ifdef __cplusplus
#include <stdint.h>
#endif

// USB descriptor type / UAC2 subtype constants (USB Audio 2.0 spec).
#define UAC2_DT_INTERFACE     0x04
#define UAC2_DT_ENDPOINT      0x05
#define UAC2_DT_CS_INTERFACE  0x24
#define UAC2_SUBTYPE_CLOCK_SOURCE  0x0A
#define UAC2_SUBTYPE_AS_GENERAL    0x01
#define UAC2_SUBTYPE_FORMAT_TYPE   0x02
#define UAC2_CLASS_AUDIO           0x01
#define UAC2_SUBCLASS_CONTROL      0x01
#define UAC2_SUBCLASS_STREAMING    0x02
#define UAC2_PROTOCOL_IP_2_0       0x20
// Endpoint bmAttributes: bits1-0 transfer type (01=iso), bits5-4 usage
// (00=data, 01=feedback).
#define UAC2_EP_XFER_ISO           0x01
#define UAC2_EP_USAGE_MASK         0x30
#define UAC2_EP_USAGE_FEEDBACK     0x10

typedef struct {
    int  valid;          // an operational (alt>0, has iso data EP) AS found
    int  interfaceNum;   // AS bInterfaceNumber
    int  altSetting;     // operational alt (the one with the iso data EP)
    int  channels;       // bNrChannels from AS_GENERAL
    int  subslotSize;    // bytes per sample (3 = 24-bit)
    int  bitResolution;  // 24, 16, 32 ...
    int  dataEp;         // iso data endpoint address (0x80 bit = IN)
    int  feedbackEp;     // explicit feedback endpoint address (0 if none)
    int  maxPacket;      // data EP wMaxPacketSize
} Uac2Stream;

typedef struct {
    int        acInterfaceNum;   // Audio Control interface number (for clock req)
    int        clockId;          // first Clock Source bClockID (0 if none)
    Uac2Stream in;               // input stream (device->host, dataEp & 0x80)
    Uac2Stream out;              // output stream (host->device)
} Uac2Info;

// Parse a full configuration descriptor. Returns 1 if at least one UAC2 (proto
// 0x20) audio-streaming interface was found, else 0. Safe on truncated/garbage
// input (bounds-checked, zero-length-desc guard).
static inline int uac2ParseConfig(const uint8_t *cfg, unsigned len, Uac2Info *out)
{
    if (!cfg || !out) return 0;
    out->acInterfaceNum = -1;
    out->clockId = 0;
    out->in.valid = out->in.feedbackEp = 0;
    out->out.valid = out->out.feedbackEp = 0;

    int curClass = -1, curSub = -1, curProto = -1, curIfNum = -1, curAlt = -1;
    int sawUac2 = 0;
    // Per-AS-interface pending state (AS_GENERAL + FORMAT_TYPE + the data/feedback
    // endpoints all belong to the current alt). Reset on each INTERFACE desc.
    // curStream is bound to in/out by the DATA endpoint's direction; the feedback
    // endpoint (always IN-direction physically) belongs to the SAME stream as the
    // data EP, not to the IN stream -- so route it by curStream, not by addr&0x80.
    int pendChannels = 2, pendSubslot = 2, pendBitres = 16;
    Uac2Stream *curStream = 0;
    int pendFeedbackEp = 0;

    unsigned i = 0;
    while (i + 2 <= len) {
        uint8_t dlen = cfg[i];
        uint8_t dtyp = cfg[i + 1];
        if (dlen == 0) break;
        if (i + dlen > len) break;

        if (dtyp == UAC2_DT_INTERFACE && dlen >= 9) {
            curIfNum = cfg[i + 2];
            curAlt   = cfg[i + 3];
            curClass = cfg[i + 5];
            curSub   = cfg[i + 6];
            curProto = cfg[i + 7];
            pendChannels = 2; pendSubslot = 2; pendBitres = 16;
            curStream = 0; pendFeedbackEp = 0;
            if (curClass == UAC2_CLASS_AUDIO && curSub == UAC2_SUBCLASS_CONTROL)
                out->acInterfaceNum = curIfNum;
            if (curClass == UAC2_CLASS_AUDIO && curSub == UAC2_SUBCLASS_STREAMING
                && curProto == UAC2_PROTOCOL_IP_2_0)
                sawUac2 = 1;
        } else if (dtyp == UAC2_DT_CS_INTERFACE && dlen >= 3) {
            uint8_t sub = cfg[i + 2];
            if (curClass == UAC2_CLASS_AUDIO && curSub == UAC2_SUBCLASS_CONTROL
                && sub == UAC2_SUBTYPE_CLOCK_SOURCE && dlen >= 4) {
                if (out->clockId == 0) out->clockId = cfg[i + 3];
            }
            if (curClass == UAC2_CLASS_AUDIO && curSub == UAC2_SUBCLASS_STREAMING
                && curProto == UAC2_PROTOCOL_IP_2_0) {
                if (sub == UAC2_SUBTYPE_AS_GENERAL && dlen >= 11)
                    pendChannels = cfg[i + 10];
                else if (sub == UAC2_SUBTYPE_FORMAT_TYPE && dlen >= 6) {
                    pendSubslot = cfg[i + 4];
                    pendBitres  = cfg[i + 5];
                }
            }
        } else if (dtyp == UAC2_DT_ENDPOINT && dlen >= 7) {
            if (curClass == UAC2_CLASS_AUDIO && curSub == UAC2_SUBCLASS_STREAMING
                && curProto == UAC2_PROTOCOL_IP_2_0) {
                uint8_t addr = cfg[i + 2];
                uint8_t attr = cfg[i + 3];
                int     mps  = cfg[i + 4] | (cfg[i + 5] << 8);
                if ((attr & 0x03) == UAC2_EP_XFER_ISO) {
                    int isFeedback = (attr & UAC2_EP_USAGE_MASK) == UAC2_EP_USAGE_FEEDBACK;
                    if (isFeedback) {
                        // Belongs to the current stream (set by its data EP). If the
                        // feedback EP precedes the data EP, stash and apply later.
                        if (curStream) curStream->feedbackEp = addr;
                        else pendFeedbackEp = addr;
                    } else {
                        Uac2Stream *s = (addr & 0x80) ? &out->in : &out->out;
                        s->valid         = 1;
                        s->interfaceNum  = curIfNum;
                        s->altSetting    = curAlt;
                        s->dataEp        = addr;
                        s->maxPacket     = mps;
                        s->channels      = pendChannels;
                        s->subslotSize   = pendSubslot;
                        s->bitResolution = pendBitres;
                        if (pendFeedbackEp) s->feedbackEp = pendFeedbackEp;
                        curStream = s;
                    }
                }
            }
        }
        i += dlen;
    }
    return sawUac2;
}

#endif
