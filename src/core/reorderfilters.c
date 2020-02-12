/*
* Copyright (c) 2012-2016 Fredrik Mellbin
*
* This file is part of VapourSynth.
*
* VapourSynth is free software; you can redistribute it and/or
* modify it under the terms of the GNU Lesser General Public
* License as published by the Free Software Foundation; either
* version 2.1 of the License, or (at your option) any later version.
*
* VapourSynth is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
* Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public
* License along with VapourSynth; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
*/

#include "internalfilters.h"
#include "VSHelper.h"
#include "filtershared.h"
#include <stdlib.h>

//////////////////////////////////////////
// Shared

enum MismatchCauses {
    DifferentDimensions = 1,
    DifferentFormats,
    DifferentFrameRates,
    DifferentLengths
};

static int findCommonVi(VSNodeRef **nodes, int num, VSVideoInfo *outvi, int ignorelength, const VSAPI *vsapi) {
    int mismatch = 0;
    const VSVideoInfo *vi;
    *outvi = *vsapi->getVideoInfo(nodes[0]);

    for (int i = 1; i < num; i++) {
        vi = vsapi->getVideoInfo(nodes[i]);

        if (outvi->width != vi->width || outvi->height != vi->height) {
            outvi->width = 0;
            outvi->height = 0;
            mismatch = DifferentDimensions;
        }

        if (outvi->format != vi->format) {
            outvi->format = 0;
            mismatch = DifferentFormats;
        }

        if (outvi->fpsNum != vi->fpsNum || outvi->fpsDen != vi->fpsDen) {
            outvi->fpsDen = 0;
            outvi->fpsNum = 0;
            mismatch = DifferentFrameRates;
        }

        if (outvi->numFrames < vi->numFrames) {
            outvi->numFrames = vi->numFrames;

            if (!ignorelength)
                mismatch = DifferentLengths;
        }
    }

    return mismatch;
}

static int compareInts(const void* a, const void* b) {
    int arg1 = *((const int*)a);
    int arg2 = *((const int*)b);
    if (arg1 < arg2)
        return -1;
    if (arg1 > arg2)
        return 1;
    return 0;
}

//////////////////////////////////////////
// Trim

typedef struct {
    VSNodeRef *node;
    int first;
} TrimData;

static const VSFrameRef *VS_CC trimGetframe(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    TrimData *d = (TrimData *) * instanceData;

    if (activationReason == arInitial) {
        vsapi->requestFrameFilter(n + d->first, d->node, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        return vsapi->getFrameFilter(n + d->first, d->node, frameCtx);
    }

    return 0;
}

static void VS_CC trimCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    TrimData d;
    TrimData *data;
    int err;
    int trimlen;

    d.first = int64ToIntS(vsapi->propGetInt(in, "first", 0, &err));
    int firstset = !err;
    int last = int64ToIntS(vsapi->propGetInt(in, "last", 0, &err));
    int lastset = !err;
    int length = int64ToIntS(vsapi->propGetInt(in, "length", 0, &err));
    int lengthset = !err;

    if (lastset && lengthset)
        RETERROR("Trim: both last frame and length specified");

    if (lastset && last < d.first)
        RETERROR("Trim: invalid last frame specified (last is less than first)");

    if (lengthset && length < 1)
        RETERROR("Trim: invalid length specified (less than 1)");

    if (d.first < 0)
        RETERROR("Trim: invalid first frame specified (less than 0)");

    d.node = vsapi->propGetNode(in, "clip", 0, 0);

    VSVideoInfo vi = *vsapi->getVideoInfo(d.node);

    if ((lastset && last >= vi.numFrames) || (lengthset && (d.first + length) > vi.numFrames) || (vi.numFrames <= d.first)) {
        vsapi->freeNode(d.node);
        RETERROR("Trim: last frame beyond clip end");
    }

    if (lastset) {
        trimlen = last - d.first + 1;
    } else if (lengthset) {
        trimlen = length;
    } else {
        trimlen = vi.numFrames - d.first;
    }

    // obvious nop() so just pass through the input clip
    if ((!firstset && !lastset && !lengthset) || (trimlen && trimlen == vi.numFrames)) {
        vsapi->propSetNode(out, "clip", d.node, paReplace);
        vsapi->freeNode(d.node);
        return;
    }

    vi.numFrames = trimlen;

    data = malloc(sizeof(d));
    *data = d;

    vsapi->createVideoFilter(in, out, "Trim", &vi, 1, trimGetframe, singleClipFree, fmParallel, nfNoCache, data, core);
}

//////////////////////////////////////////
// AudioTrim

typedef struct {
    VSNodeRef *node;
    VSAudioInfo ai;
    int64_t first;
} AudioTrimData;

static const VSFrameRef *VS_CC audioTrimGetframe(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    AudioTrimData *d = (AudioTrimData *) *instanceData;

    int64_t startSample = n * (int64_t)d->ai.format->samplesPerFrame + d->first;
    int startFrame = (int)(startSample / d->ai.format->samplesPerFrame);
    int length = VSMIN(d->ai.numSamples - n * (int64_t)d->ai.format->samplesPerFrame, d->ai.format->samplesPerFrame);

    if (startSample % d->ai.format->samplesPerFrame == 0 && n != d->ai.numFrames - 1) { // pass through audio frames when possible
        if (activationReason == arInitial) {
            vsapi->requestFrameFilter(startFrame, d->node, frameCtx);
        } else if (activationReason == arAllFramesReady) {
            VSFrameRef *src = vsapi->getFrameFilter(startFrame, d->node, frameCtx);
            if (length == vsapi->getFrameLength(src))
                return src;
            VSFrameRef *dst = vsapi->newAudioFrame(d->ai.format, d->ai.sampleRate, length, src, core);
            for (int channel = 0; channel < d->ai.format->numChannels; channel++)             
                memcpy(vsapi->getWritePtr(dst, channel), vsapi->getReadPtr(src, channel), length * d->ai.format->bytesPerSample);
            vsapi->freeFrame(src);
            return dst;
        }
    } else {
        int numSrc1Samples = d->ai.format->samplesPerFrame - (startSample % d->ai.format->samplesPerFrame);
        if (activationReason == arInitial) {
            vsapi->requestFrameFilter(startFrame, d->node, frameCtx);
            if (numSrc1Samples < length)
                vsapi->requestFrameFilter(startFrame + 1, d->node, frameCtx);
        } else if (activationReason == arAllFramesReady) {
            const VSFrameRef *src1 = vsapi->getFrameFilter(startFrame, d->node, frameCtx);
            VSFrameRef *dst = vsapi->newAudioFrame(d->ai.format, d->ai.sampleRate, length, src1, core);
            for (int channel = 0; channel < d->ai.format->numChannels; channel++)             
                memcpy(vsapi->getWritePtr(dst, channel), vsapi->getReadPtr(src1, channel) + (d->ai.format->samplesPerFrame - numSrc1Samples) * d->ai.format->bytesPerSample, numSrc1Samples * d->ai.format->bytesPerSample);
            vsapi->freeFrame(src1);

            if (length > numSrc1Samples) {
                const VSFrameRef *src2 = vsapi->getFrameFilter(startFrame + 1, d->node, frameCtx);
                for (int channel = 0; channel < d->ai.format->numChannels; channel++)             
                    memcpy(vsapi->getWritePtr(dst, channel) + numSrc1Samples * d->ai.format->bytesPerSample, vsapi->getReadPtr(src2, channel), (length - numSrc1Samples) * d->ai.format->bytesPerSample);
                vsapi->freeFrame(src2);
            }

            return dst;
        }
    }

    return 0;
}

static void VS_CC audioTrimCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    AudioTrimData d = { 0 };
    AudioTrimData *data;
    int err;
    int64_t trimlen;

    d.first = int64ToIntS(vsapi->propGetInt(in, "first", 0, &err));
    int firstset = !err;
    int64_t last = int64ToIntS(vsapi->propGetInt(in, "last", 0, &err));
    int lastset = !err;
    int64_t length = int64ToIntS(vsapi->propGetInt(in, "length", 0, &err));
    int lengthset = !err;

    if (lastset && lengthset)
        RETERROR("AudioTrim: both last sample and length specified");

    if (lastset && last < d.first)
        RETERROR("AudioTrim: invalid last sample specified (last is less than first)");

    if (lengthset && length < 1)
        RETERROR("AudioTrim: invalid length specified (less than 1)");

    if (d.first < 0)
        RETERROR("Trim: invalid first frame specified (less than 0)");

    d.node = vsapi->propGetNode(in, "clip", 0, 0);

    d.ai = *vsapi->getAudioInfo(d.node);

    if ((lastset && last >= d.ai.numSamples) || (lengthset && (d.first + length) > d.ai.numSamples) || (d.ai.numSamples <= d.first)) {
        vsapi->freeNode(d.node);
        RETERROR("AudioTrim: last sample beyond clip end");
    }

    if (lastset) {
        trimlen = last - d.first + 1;
    } else if (lengthset) {
        trimlen = length;
    } else {
        trimlen = d.ai.numSamples - d.first;
    }

    // obvious nop() so just pass through the input clip
    if ((!firstset && !lastset && !lengthset) || (trimlen && trimlen == d.ai.numSamples)) {
        vsapi->propSetNode(out, "clip", d.node, paReplace);
        vsapi->freeNode(d.node);
        return;
    }

    d.ai.numSamples = trimlen;

    data = malloc(sizeof(d));
    *data = d;

    vsapi->createAudioFilter(in, out, "AudioTrim", &d.ai, 1, audioTrimGetframe, singleClipFree, fmParallel, nfNoCache, data, core);
}

//////////////////////////////////////////
// Interleave

typedef struct {
    VSNodeRef **node;
    VSVideoInfo vi;
    int numclips;
    int modifyDuration;
} InterleaveData;

static void VS_CC interleaveInit(VSMap *in, VSMap *out, void **instanceData, VSNode *node, VSCore *core, const VSAPI *vsapi) {
    InterleaveData *d = (InterleaveData *) * instanceData;
    vsapi->setVideoInfo(&d->vi, 1, node);
}

static const VSFrameRef *VS_CC interleaveGetframe(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    InterleaveData *d = (InterleaveData *) * instanceData;

    if (activationReason == arInitial) {
        vsapi->requestFrameFilter(n / d->numclips, d->node[n % d->numclips], frameCtx);
    } else if (activationReason == arAllFramesReady) {
        const VSFrameRef *src = vsapi->getFrameFilter(n / d->numclips, d->node[n % d->numclips], frameCtx);
        if (d->modifyDuration) {
            VSFrameRef *dst = vsapi->copyFrame(src, core);
            vsapi->freeFrame(src);

            VSMap *dst_props = vsapi->getFramePropsRW(dst);
            int errNum, errDen;
            int64_t durationNum = vsapi->propGetInt(dst_props, "_DurationNum", 0, &errNum);
            int64_t durationDen = vsapi->propGetInt(dst_props, "_DurationDen", 0, &errDen);
            if (!errNum && !errDen) {
                muldivRational(&durationNum, &durationDen, 1, d->numclips);
                vsapi->propSetInt(dst_props, "_DurationNum", durationNum, paReplace);
                vsapi->propSetInt(dst_props, "_DurationDen", durationDen, paReplace);
            }
            return dst;
        } else {
            return src;
        }
    }

    return 0;
}

static void VS_CC interleaveFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    InterleaveData *d = (InterleaveData *)instanceData;
    for (int i = 0; i < d->numclips; i++)
        vsapi->freeNode(d->node[i]);

    free(d->node);
    free(d);
}

static void VS_CC interleaveCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    InterleaveData d;
    InterleaveData *data;
    int err;

    int mismatch = !!vsapi->propGetInt(in, "mismatch", 0, &err);
    int extend = !!vsapi->propGetInt(in, "extend", 0, &err);
    d.modifyDuration = !!vsapi->propGetInt(in, "modify_duration", 0, &err);
    if (err)
        d.modifyDuration = 1;
    d.numclips = vsapi->propNumElements(in, "clips");

    if (d.numclips == 1) { // passthrough for the special case with only one clip
        VSNodeRef *cref = vsapi->propGetNode(in, "clips", 0, 0);
        vsapi->propSetNode(out, "clip", cref, paReplace);
        vsapi->freeNode(cref);
    } else {
        d.node = malloc(sizeof(d.node[0]) * d.numclips);
        int compat = 0;

        for (int i = 0; i < d.numclips; i++) {
            d.node[i] = vsapi->propGetNode(in, "clips", i, 0);

            if (isCompatFormat(vsapi->getVideoInfo(d.node[i])))
                compat = 1;
        }

        int mismatchCause = findCommonVi(d.node, d.numclips, &d.vi, 1, vsapi);
        if (mismatchCause && (!mismatch || compat)) {
            for (int i = 0; i < d.numclips; i++)
                vsapi->freeNode(d.node[i]);

            free(d.node);

            if (mismatchCause == DifferentDimensions)
                RETERROR("Interleave: the clips' dimensions don't match");
            else if (mismatchCause == DifferentFormats)
                RETERROR("Interleave: the clips' formats don't match");
            else if (mismatchCause == DifferentFrameRates)
                RETERROR("Interleave: the clips' frame rates don't match");
            else if (mismatchCause == DifferentLengths)
                RETERROR("Interleave: the clips' lengths don't match");
        }

        int overflow = 0;

        if (extend) {
            if (d.vi.numFrames > INT_MAX / d.numclips)
                overflow = 1;
            d.vi.numFrames *= d.numclips;
        } else if (d.vi.numFrames) {
            // this is exactly how avisynth does it
            d.vi.numFrames = (vsapi->getVideoInfo(d.node[0])->numFrames - 1) * d.numclips + 1;
            for (int i = 0; i < d.numclips; i++) {
                if (vsapi->getVideoInfo(d.node[i])->numFrames > ((INT_MAX - i - 1) / d.numclips + 1))
                    overflow = 1;
                d.vi.numFrames = VSMAX(d.vi.numFrames, (vsapi->getVideoInfo(d.node[i])->numFrames - 1) * d.numclips + i + 1);
            }
        }

        if (overflow) {
            for (int i = 0; i < d.numclips; i++)
                vsapi->freeNode(d.node[i]);

            free(d.node);
            RETERROR("Interleave: resulting clip is too long");
        }

        if (d.modifyDuration)
            muldivRational(&d.vi.fpsNum, &d.vi.fpsDen, d.numclips, 1);

        data = malloc(sizeof(d));
        *data = d;

        vsapi->createFilter(in, out, "Interleave", interleaveInit, interleaveGetframe, interleaveFree, fmParallel, nfNoCache, data, core);
    }
}

//////////////////////////////////////////
// Reverse

static const VSFrameRef *VS_CC reverseGetframe(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    SingleClipData *d = (SingleClipData *) * instanceData;

    if (activationReason == arInitial) {
        vsapi->requestFrameFilter(VSMAX(d->vi->numFrames - n - 1, 0), d->node, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        return vsapi->getFrameFilter(VSMAX(d->vi->numFrames - n - 1, 0), d->node, frameCtx);
    }

    return 0;
}

static void VS_CC reverseCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    SingleClipData d;
    SingleClipData *data;

    d.node = vsapi->propGetNode(in, "clip", 0, 0);
    d.vi = vsapi->getVideoInfo(d.node);

    data = malloc(sizeof(d));
    *data = d;

    vsapi->createFilter(in, out, "Reverse", singleClipInit, reverseGetframe, singleClipFree, fmParallel, nfNoCache, data, core);
}

//////////////////////////////////////////
// Loop

typedef struct {
    VSNodeRef *node;
    VSVideoInfo vi;
    int numFramesIn;
} LoopData;

static void VS_CC loopInit(VSMap *in, VSMap *out, void **instanceData, VSNode *node, VSCore *core, const VSAPI *vsapi) {
    LoopData *d = (LoopData *) * instanceData;
    vsapi->setVideoInfo(&d->vi, 1, node);
}

static const VSFrameRef *VS_CC loopGetframe(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    LoopData *d = (LoopData *) * instanceData;

    if (activationReason == arInitial) {
        vsapi->requestFrameFilter(n % d->numFramesIn, d->node, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        return vsapi->getFrameFilter(n % d->numFramesIn, d->node, frameCtx);
    }

    return 0;
}

static void VS_CC loopCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    LoopData d;
    LoopData *data;
    int err;
    int times = int64ToIntS(vsapi->propGetInt(in, "times", 0, &err));
    if (times < 0)
        RETERROR("Loop: cannot repeat clip a negative number of times");

    d.node = vsapi->propGetNode(in, "clip", 0, 0);
    d.vi = *vsapi->getVideoInfo(d.node);
    d.numFramesIn = d.vi.numFrames;

    // early termination for the trivial case
    if (times == 1) {
        vsapi->propSetNode(out, "clip", d.node, paReplace);
        vsapi->freeNode(d.node);
        return;
    }

    if (times > 0) {
        if (d.vi.numFrames > INT_MAX / times) {
            vsapi->freeNode(d.node);
            RETERROR("Loop: resulting clip is too long");
        }

        d.vi.numFrames *= times;
    } else { // loop for maximum duration
        d.vi.numFrames = INT_MAX;
    }

    data = malloc(sizeof(d));
    *data = d;

    vsapi->createFilter(in, out, "Loop", loopInit, loopGetframe, singleClipFree, fmParallel, nfNoCache, data, core);
}

//////////////////////////////////////////
// SelectEvery

typedef struct {
    VSNodeRef *node;
    VSVideoInfo vi;
    int cycle;
    int *offsets;
    int num;
    int modifyDuration;
} SelectEveryData;

static void VS_CC selectEveryInit(VSMap *in, VSMap *out, void **instanceData, VSNode *node, VSCore *core, const VSAPI *vsapi) {
    SelectEveryData *d = (SelectEveryData *) * instanceData;
    vsapi->setVideoInfo(&d->vi, 1, node);
}

static const VSFrameRef *VS_CC selectEveryGetframe(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    SelectEveryData *d = (SelectEveryData *) * instanceData;

    if (activationReason == arInitial) {
        *frameData = (void *)(intptr_t)((n / d->num) * d->cycle + d->offsets[n % d->num]);
        vsapi->requestFrameFilter((intptr_t)*frameData, d->node, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        const VSFrameRef *src = vsapi->getFrameFilter((intptr_t)(*frameData), d->node, frameCtx);
        if (d->modifyDuration) {
            VSFrameRef *dst = vsapi->copyFrame(src, core);
            VSMap *dst_props = vsapi->getFramePropsRW(dst);
            int errNum, errDen;
            int64_t durationNum = vsapi->propGetInt(dst_props, "_DurationNum", 0, &errNum);
            int64_t durationDen = vsapi->propGetInt(dst_props, "_DurationDen", 0, &errDen);
            if (!errNum && !errDen) {
                muldivRational(&durationNum, &durationDen, d->cycle, d->num);
                vsapi->propSetInt(dst_props, "_DurationNum", durationNum, paReplace);
                vsapi->propSetInt(dst_props, "_DurationDen", durationDen, paReplace);
            }
            vsapi->freeFrame(src);
            return dst;
        } else {
            return src;
        }
    }

    return 0;
}

static void VS_CC selectEveryFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    SelectEveryData *d = (SelectEveryData *)instanceData;
    free(d->offsets);
    vsapi->freeNode(d->node);
    free(d);
}

static void VS_CC selectEveryCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    SelectEveryData d;
    SelectEveryData *data;
    int err;

    d.cycle = int64ToIntS(vsapi->propGetInt(in, "cycle", 0, 0));

    if (d.cycle <= 1)
        RETERROR("SelectEvery: invalid cycle size (must be greater than 1)");

    d.num = vsapi->propNumElements(in, "offsets");
    d.modifyDuration = !!vsapi->propGetInt(in, "modify_duration", 0, &err);
    if (err)
        d.modifyDuration = 1;

    d.offsets = malloc(sizeof(d.offsets[0]) * d.num);

    for (int i = 0; i < d.num; i++) {
        d.offsets[i] = int64ToIntS(vsapi->propGetInt(in, "offsets", i, 0));

        if (d.offsets[i] < 0 || d.offsets[i] >= d.cycle) {
            free(d.offsets);
            RETERROR("SelectEvery: invalid offset specified");
        }
    }

    d.node = vsapi->propGetNode(in, "clip", 0, 0);

    d.vi = *vsapi->getVideoInfo(d.node);
    int inputnframes = d.vi.numFrames;
    if (inputnframes) {
        d.vi.numFrames = (inputnframes / d.cycle) * d.num;
        for (int i = 0; i < d.num; i++)
            if (d.offsets[i] < inputnframes % d.cycle)
                d.vi.numFrames++;
    }

    if (d.vi.numFrames == 0) {
        vsapi->freeNode(d.node);
        free(d.offsets);
        RETERROR("SelectEvery: no frames to output, all offsets outside available frames");
    }

    if (d.modifyDuration)
        muldivRational(&d.vi.fpsNum, &d.vi.fpsDen, d.num, d.cycle);

    data = malloc(sizeof(d));
    *data = d;

    vsapi->createFilter(in, out, "SelectEvery", selectEveryInit, selectEveryGetframe, selectEveryFree, fmParallel, nfNoCache, data, core);
}

//////////////////////////////////////////
// Splice

typedef struct {
    VSNodeRef **node;
    int *numframes;
    int numclips;
} SpliceData;

typedef struct {
    int f;
    int idx;
} SpliceCtx;

static const VSFrameRef *VS_CC spliceGetframe(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    SpliceData *d = (SpliceData *) * instanceData;

    if (activationReason == arInitial) {
        int frame = 0;
        int idx = 0;
        int cumframe = 0;
        SpliceCtx *s = malloc(sizeof(SpliceCtx));

        for (int i = 0; i < d->numclips; i++) {
            if ((n >= cumframe && n < cumframe + d->numframes[i]) || i == d->numclips - 1) {
                idx = i;
                frame = n - cumframe;
                break;
            }

            cumframe += d->numframes[i];
        }

        *frameData = s;
        s->f = frame;
        s->idx = idx;
        vsapi->requestFrameFilter(frame, d->node[idx], frameCtx);
    } else if (activationReason == arAllFramesReady) {
        SpliceCtx *s = (SpliceCtx *) * frameData;
        const VSFrameRef *f = vsapi->getFrameFilter(s->f, d->node[s->idx], frameCtx);
        free(s);
        return f;
    }

    return 0;
}

static void VS_CC spliceFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    SpliceData *d = (SpliceData *)instanceData;
    for (int i = 0; i < d->numclips; i++)
        vsapi->freeNode(d->node[i]);

    free(d->node);
    free(d->numframes);
    free(d);
}

static void VS_CC spliceCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    SpliceData d;
    SpliceData *data;
    int err;
    VSVideoInfo vi;

    d.numclips = vsapi->propNumElements(in, "clips");
    int mismatch = !!vsapi->propGetInt(in, "mismatch", 0, &err);

    if (d.numclips == 1) { // passthrough for the special case with only one clip
        VSNodeRef *cref = vsapi->propGetNode(in, "clips", 0, 0);
        vsapi->propSetNode(out, "clip", cref, paReplace);
        vsapi->freeNode(cref);
    } else {
        int compat = 0;
        d.node = malloc(sizeof(d.node[0]) * d.numclips);

        for (int i = 0; i < d.numclips; i++) {
            d.node[i] = vsapi->propGetNode(in, "clips", i, 0);

            if (isCompatFormat(vsapi->getVideoInfo(d.node[i])))
                compat = 1;
        }

        int mismatchCause = findCommonVi(d.node, d.numclips, &vi, 1, vsapi);
        if (mismatchCause && (!mismatch || compat) && !isSameFormat(&vi, vsapi->getVideoInfo(d.node[0]))) {
            for (int i = 0; i < d.numclips; i++)
                vsapi->freeNode(d.node[i]);

            free(d.node);

            if (mismatchCause == DifferentDimensions)
                RETERROR("Splice: the clips' dimensions don't match");
            else if (mismatchCause == DifferentFormats)
                RETERROR("Splice: the clips' formats don't match");
            else if (mismatchCause == DifferentFrameRates)
                RETERROR("Splice: the clips' frame rates don't match");
            else if (mismatchCause == DifferentLengths)
                RETERROR("Splice: the clips' lengths don't match");
        }

        d.numframes = malloc(sizeof(d.numframes[0]) * d.numclips);
        vi.numFrames = 0;

        for (int i = 0; i < d.numclips; i++) {
            d.numframes[i] = (vsapi->getVideoInfo(d.node[i]))->numFrames;
            vi.numFrames += d.numframes[i];

            // did it overflow?
            if (vi.numFrames < d.numframes[i]) {
                for (int j = 0; j < d.numclips; i++)
                    vsapi->freeNode(d.node[j]);

                free(d.node);
                free(d.numframes);

                RETERROR("Splice: the resulting clip is too long");
            }
        }

        data = malloc(sizeof(d));
        *data = d;

        vsapi->createVideoFilter(in, out, "Splice", &vi, 1, spliceGetframe, spliceFree, fmParallel, nfNoCache, data, core);
    }
}

//////////////////////////////////////////
// AudioSplice2

typedef struct {
    VSAudioInfo ai;
    VSNodeRef *node1;
    VSNodeRef *node2;
    int numFrames1;
    int64_t numSamples1;
    int64_t numSamples2;
} AudioSplice2Data;

static const VSFrameRef *VS_CC audioSplice2PassthroughGetframe(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    AudioSplice2Data *d = (AudioSplice2Data *) *instanceData;
    if (activationReason == arInitial) {
            if (n < d->numFrames1)
                vsapi->requestFrameFilter(n, d->node1, frameCtx);
            else
                vsapi->requestFrameFilter(n - d->numFrames1, d->node2, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        if (n < d->numFrames1)
            return vsapi->getFrameFilter(n, d->node1, frameCtx);
        else
            return vsapi->getFrameFilter(n - d->numFrames1, d->node2, frameCtx);
    }

    return 0;
}

static const VSFrameRef *VS_CC audioSplice2Getframe(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    AudioSplice2Data *d = (AudioSplice2Data *) *instanceData;

    if (activationReason == arInitial) {
        if (n < d->numFrames1 - 1) {
            vsapi->requestFrameFilter(n, d->node1, frameCtx);
        } else if (n == d->numFrames1 - 1) {
            vsapi->requestFrameFilter(n, d->node1, frameCtx);
            vsapi->requestFrameFilter(0, d->node2, frameCtx);
        } else {
            vsapi->requestFrameFilter(n - d->numFrames1, d->node2, frameCtx);
            vsapi->requestFrameFilter(n - d->numFrames1 + 1, d->node2, frameCtx);
        }
    } else if (activationReason == arAllFramesReady) {
        const VSFrameRef *f1 = NULL;
        const VSFrameRef *f2 = NULL;

        if (n < d->numFrames1 - 1) {
            return vsapi->getFrameFilter(n, d->node1, frameCtx);
        } else if (n == d->numFrames1 - 1) {
            f1 = vsapi->getFrameFilter(n, d->node1, frameCtx);
            f2 = vsapi->getFrameFilter(0, d->node2, frameCtx);
        } else {
            f1 = vsapi->getFrameFilter(n - d->numFrames1, d->node2, frameCtx);
            f2 = vsapi->getFrameFilter(n - d->numFrames1 + 1, d->node2, frameCtx);
        }

        int samplesOut = VSMIN(d->ai.format->samplesPerFrame, d->ai.numSamples - n * (int64_t)d->ai.format->samplesPerFrame);

        VSFrameRef *f = vsapi->newAudioFrame(d->ai.format, d->ai.sampleRate, samplesOut, f1, core);

        //////////////

        if (n == d->numFrames1 - 1) {
            // handle the seam between clip 1 and 2 
            int f1copy = VSMIN(samplesOut, vsapi->getFrameLength(f1));
            int f2copy = samplesOut - f1copy;
            f1copy *= d->ai.format->bytesPerSample;
            f2copy *= d->ai.format->bytesPerSample;

            for (int channel = 0; channel < d->ai.format->numChannels; channel++) {
                memcpy(vsapi->getWritePtr(f, channel), vsapi->getReadPtr(f1, channel), f1copy);
                memcpy(vsapi->getWritePtr(f, channel) + f1copy, vsapi->getReadPtr(f2, channel), f2copy);
            }
        } else {
            int f1offset = d->ai.format->samplesPerFrame - ((d->numSamples1 - 1) % d->ai.format->samplesPerFrame) - 1;
            int f1copy = VSMIN(samplesOut, vsapi->getFrameLength(f1) - f1offset);
            int f2copy = samplesOut - f1copy;
            assert(f1copy > 0 && (f2copy > 0 || (f2copy >= 0 && n == d->ai.numFrames - 1)));
            f1copy *= d->ai.format->bytesPerSample;
            f2copy *= d->ai.format->bytesPerSample;
            f1offset *= d->ai.format->bytesPerSample;

            for (int channel = 0; channel < d->ai.format->numChannels; channel++) {
                memcpy(vsapi->getWritePtr(f, channel), vsapi->getReadPtr(f1, channel) + f1offset, f1copy);
                memcpy(vsapi->getWritePtr(f, channel) + f1copy, vsapi->getReadPtr(f2, channel), f2copy);
            }
        }

        ////////////////////

        vsapi->freeFrame(f1);
        vsapi->freeFrame(f2);

        return f;
    }

    return 0;
}

static void VS_CC audioSplice2Free(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    AudioSplice2Data *d = (AudioSplice2Data *)instanceData;
    vsapi->freeNode(d->node1);
    vsapi->freeNode(d->node2);
    free(d);
}

static void VS_CC audioSplice2Create(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    AudioSplice2Data d;
    AudioSplice2Data *data;

    d.node1 = vsapi->propGetNode(in, "clip1", 0, 0);
    d.node2 = vsapi->propGetNode(in, "clip2", 0, 0);
    const VSAudioInfo *ai1 = vsapi->getAudioInfo(d.node1);
    const VSAudioInfo *ai2 = vsapi->getAudioInfo(d.node2);

    d.numFrames1 = ai1->numFrames;

    d.numSamples1 = ai1->numSamples;
    d.numSamples2 = ai2->numSamples;

    if (!isSameAudioFormat(ai1, ai2)) {
        vsapi->freeNode(d.node1);
        vsapi->freeNode(d.node2);
        RETERROR("AudioSplice2: format mismatch");
    }

    d.ai = *ai1;
    d.ai.numSamples += d.numSamples2;
    d.ai.numFrames = (d.ai.numSamples + d.ai.format->samplesPerFrame - 1) / d.ai.format->samplesPerFrame;

    if (d.ai.numSamples < d.numSamples1 || d.ai.numSamples < d.numSamples2) {
        vsapi->freeNode(d.node1);
        vsapi->freeNode(d.node2);
        RETERROR("AudioSplice2: the resulting clip is too long");
    }

    data = malloc(sizeof(d));
    *data = d;

    vsapi->createAudioFilter(in, out, "AudioSplice2", &d.ai, 1, (d.numSamples1 % d.ai.format->samplesPerFrame) ? audioSplice2Getframe : audioSplice2PassthroughGetframe, audioSplice2Free, fmParallel, nfNoCache, data, core);
}


//////////////////////////////////////////
// AudioSplice2Wrapper

static void VS_CC audioSplice2Wrapper(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    int numnodes = vsapi->propNumElements(in, "clips");

    if (numnodes == 1) { // passthrough for the special case with only one clip
        VSNodeRef *cref = vsapi->propGetNode(in, "clips", 0, 0);
        vsapi->propSetNode(out, "clip", cref, paReplace);
        vsapi->freeNode(cref);
    }

    VSNodeRef *tmp = vsapi->propGetNode(in, "clips", 0, 0);
    VSMap *map = vsapi->createMap();
    VSPlugin *plugin = vsapi->getPluginById("com.vapoursynth.std", core);

    for (int i = 1; i < numnodes; i++) {
        vsapi->propSetNode(map, "clip1", tmp, paReplace);
        vsapi->freeNode(tmp);
        VSNodeRef *cref = vsapi->propGetNode(in, "clips", i, 0);
        vsapi->propSetNode(map, "clip2", cref, paReplace);
        vsapi->freeNode(cref);
        VSMap *result = vsapi->invoke(plugin, "AudioSplice2", map);
        if (vsapi->getError(result)) {
            vsapi->setError(out, vsapi->getError(result));
            vsapi->freeMap(map);    
            vsapi->freeMap(result);
            return;
        }

        tmp = vsapi->propGetNode(result, "clip", 0, 0);
        vsapi->freeMap(result);
    }

    vsapi->freeMap(map); 
    vsapi->propSetNode(out, "clip", tmp, paReplace);
    vsapi->freeNode(tmp);
}

//////////////////////////////////////////
// AudioSplice

/*
typedef struct {
    VSAudioInfo ai;
    VSNodeRef **node;
    int64_t *numsamples;
    int numnodes;
} AudioSpliceData;

static const VSFrameRef *VS_CC audioSpliceGetframe(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    AudioSpliceData *d = (AudioSpliceData *) * instanceData;

    if (activationReason == arInitial) {


    } else if (activationReason == arAllFramesReady) {

        return f;
    }

    return 0;
}

static void VS_CC audioSpliceFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    AudioSpliceData *d = (AudioSpliceData *)instanceData;
    for (int i = 0; i < d->numnodes; i++)
        vsapi->freeNode(d->node[i]);

    free(d->node);
    free(d->numsamples);
    free(d);
}

static void VS_CC audioSpliceCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    AudioSpliceData d;
    AudioSpliceData *data;

    d.numnodes = vsapi->propNumElements(in, "clips");

    if (d.numnodes == 1) { // passthrough for the special case with only one clip
        VSNodeRef *cref = vsapi->propGetNode(in, "clips", 0, 0);
        vsapi->propSetNode(out, "clip", cref, paReplace);
        vsapi->freeNode(cref);
    } else {
        d.node = malloc(sizeof(d.node[0]) * d.numnodes);

        for (int i = 0; i < d.numnodes; i++)
            d.node[i] = vsapi->propGetNode(in, "clips", i, 0);

        d.ai = *vsapi->getAudioInfo(d.node[0]);

        for (int i = 0; i < d.numnodes; i++) {
            if (!isSameAudioFormat(&d.ai, vsapi->getAudioInfo(d.node[i]))) {
                for (int i = 0; i < d.numnodes; i++)
                    vsapi->freeNode(d.node[i]);
                free(d.node);
                RETERROR("AudioSplice: format mismatch");
            }
        }
        d.numsamples = malloc(sizeof(d.numsamples[0]) * d.numnodes);
        d.ai.numSamples = 0;

        for (int i = 0; i < d.numnodes; i++) {
            d.numsamples[i] = (vsapi->getAudioInfo(d.node[i]))->numSamples;
            d.ai.numSamples += d.numsamples[i];

            if (d.ai.numSamples < d.numsamples[i]) {
                for (int j = 0; j < d.numnodes; i++)
                    vsapi->freeNode(d.node[j]);

                free(d.node);
                free(d.numsamples);

                RETERROR("AudioSplice: the resulting clip is too long");
            }
        }

        data = malloc(sizeof(d));
        *data = d;

        vsapi->createAudioFilter(in, out, "AudioSplice", &d.ai, 1, audioSpliceGetframe, audioSpliceFree, fmParallel, nfNoCache, data, core);
    }
}
*/

//////////////////////////////////////////
// DuplicateFrames

typedef struct {
    VSNodeRef *node;
    VSVideoInfo vi;

    int *dups;
    int num_dups;
} DuplicateFramesData;

static void VS_CC duplicateFramesInit(VSMap *in, VSMap *out, void **instanceData, VSNode *node, VSCore *core, const VSAPI *vsapi) {
    DuplicateFramesData *d = (DuplicateFramesData *) * instanceData;
    vsapi->setVideoInfo(&d->vi, 1, node);
}

static const VSFrameRef *VS_CC duplicateFramesGetFrame(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    DuplicateFramesData *d = (DuplicateFramesData *) * instanceData;

    if (activationReason == arInitial) {
        for (int i = 0; i < d->num_dups; i++)
            if (n > d->dups[i])
                n--;
            else
                break;

        *frameData = (void *)(intptr_t)n;

        vsapi->requestFrameFilter(n, d->node, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        return vsapi->getFrameFilter((intptr_t)(*frameData), d->node, frameCtx);
    }

    return 0;
}

static void VS_CC duplicateFramesFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    DuplicateFramesData *d = (DuplicateFramesData *)instanceData;

    vsapi->freeNode(d->node);
    free(d->dups);
    free(d);
}

static void VS_CC duplicateFramesCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    DuplicateFramesData d;
    DuplicateFramesData *data;

    d.node = vsapi->propGetNode(in, "clip", 0, 0);
    d.vi = *vsapi->getVideoInfo(d.node);

    d.num_dups = vsapi->propNumElements(in, "frames");

    d.dups = (int *)malloc(d.num_dups * sizeof(int));

    for (int i = 0; i < d.num_dups; i++) {
        d.dups[i] = int64ToIntS(vsapi->propGetInt(in, "frames", i, 0));

        if (d.dups[i] < 0 || (d.vi.numFrames && d.dups[i] > d.vi.numFrames - 1)) {
            vsapi->freeNode(d.node);
            free(d.dups);
            RETERROR("DuplicateFrames: out of bounds frame number");
        }
    }

    qsort(d.dups, d.num_dups, sizeof(int), compareInts);

    if (d.vi.numFrames + d.num_dups < d.vi.numFrames) {
        vsapi->freeNode(d.node);
        free(d.dups);
        RETERROR("DuplicateFrames: resulting clip is too long");
    }
    d.vi.numFrames += d.num_dups;

    data = malloc(sizeof(d));
    *data = d;

    vsapi->createFilter(in, out, "DuplicateFrames", duplicateFramesInit, duplicateFramesGetFrame, duplicateFramesFree, fmParallel, nfNoCache, data, core);
}

//////////////////////////////////////////
// DeleteFrames

typedef struct {
    VSNodeRef *node;
    VSVideoInfo vi;

    int *delete;
    int num_delete;
} DeleteFramesData;

static void VS_CC deleteFramesInit(VSMap *in, VSMap *out, void **instanceData, VSNode *node, VSCore *core, const VSAPI *vsapi) {
    DeleteFramesData *d = (DeleteFramesData *) * instanceData;
    vsapi->setVideoInfo(&d->vi, 1, node);
}

static const VSFrameRef *VS_CC deleteFramesGetFrame(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    DeleteFramesData *d = (DeleteFramesData *) * instanceData;

    if (activationReason == arInitial) {
        for (int i = 0; i < d->num_delete; i++)
            if (n >= d->delete[i])
                n++;
            else
                break;

        *frameData = (void *)(intptr_t)n;

        vsapi->requestFrameFilter(n, d->node, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        return vsapi->getFrameFilter((intptr_t)(*frameData), d->node, frameCtx);
    }

    return 0;
}

static void VS_CC deleteFramesFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    DeleteFramesData *d = (DeleteFramesData *)instanceData;

    vsapi->freeNode(d->node);
    free(d->delete);
    free(d);
}

static void VS_CC deleteFramesCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    DeleteFramesData d;
    DeleteFramesData *data;

    d.node = vsapi->propGetNode(in, "clip", 0, 0);
    d.vi = *vsapi->getVideoInfo(d.node);

    d.num_delete = vsapi->propNumElements(in, "frames");

    d.delete = (int *)malloc(d.num_delete * sizeof(int));

    for (int i = 0; i < d.num_delete; i++) {
        d.delete[i] = int64ToIntS(vsapi->propGetInt(in, "frames", i, 0));

        if (d.delete[i] < 0 || (d.vi.numFrames && d.delete[i] >= d.vi.numFrames)) {
            vsapi->freeNode(d.node);
            free(d.delete);
            RETERROR("DeleteFrames: out of bounds frame number");
        }
    }

    qsort(d.delete, d.num_delete, sizeof(int), compareInts);

    for (int i = 0; i < d.num_delete - 1; i++) {
        if (d.delete[i] == d.delete[i + 1]) {
            vsapi->freeNode(d.node);
            free(d.delete);
            RETERROR("DeleteFrames: can't delete a frame more than once");
        }
    }

    if (d.vi.numFrames) {
        d.vi.numFrames -= d.num_delete;
        if (!d.vi.numFrames) {
            vsapi->freeNode(d.node);
            free(d.delete);
            RETERROR("DeleteFrames: can't delete all frames");
        }
    }

    data = malloc(sizeof(d));
    *data = d;

    vsapi->createFilter(in, out, "DeleteFrames", deleteFramesInit, deleteFramesGetFrame, deleteFramesFree, fmParallel, nfNoCache, data, core);
}

//////////////////////////////////////////
// FreezeFrames

struct Freeze {
    int first;
    int last;
    int replacement;
};

typedef struct {
    VSNodeRef *node;
    const VSVideoInfo *vi;

    struct Freeze *freeze;
    int num_freeze;
} FreezeFramesData;

static void VS_CC freezeFramesInit(VSMap *in, VSMap *out, void **instanceData, VSNode *node, VSCore *core, const VSAPI *vsapi) {
    FreezeFramesData *d = (FreezeFramesData *) * instanceData;
    vsapi->setVideoInfo(d->vi, 1, node);
}

static const VSFrameRef *VS_CC freezeFramesGetFrame(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    FreezeFramesData *d = (FreezeFramesData *) * instanceData;

    if (activationReason == arInitial) {
        if (n >= d->freeze[0].first && n <= d->freeze[d->num_freeze - 1].last)
            for (int i = 0; i < d->num_freeze; i++)
                if (n >= d->freeze[i].first && n <= d->freeze[i].last) {
                    n = d->freeze[i].replacement;
                    break;
                }

        *frameData = (void *)(intptr_t)n;

        vsapi->requestFrameFilter(n, d->node, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        return vsapi->getFrameFilter((intptr_t)(*frameData), d->node, frameCtx);
    }

    return 0;
}

static void VS_CC freezeFramesFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    FreezeFramesData *d = (FreezeFramesData *)instanceData;

    vsapi->freeNode(d->node);
    free(d->freeze);
    free(d);
}

static int freezeFramesSort(const void *a, const void *b) {
    const struct Freeze *x = a;
    const struct Freeze *y = b;
    return x->first - y->first;
}

static void VS_CC freezeFramesCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    FreezeFramesData d;
    FreezeFramesData *data;

    d.num_freeze = vsapi->propNumElements(in, "first");
    if (d.num_freeze != vsapi->propNumElements(in, "last") || d.num_freeze != vsapi->propNumElements(in, "replacement")) {
        vsapi->setError(out, "FreezeFrames: 'first', 'last', and 'replacement' must have the same length.");
        return;
    }

    d.node = vsapi->propGetNode(in, "clip", 0, 0);
    d.vi = vsapi->getVideoInfo(d.node);

    d.freeze = (struct Freeze *)malloc(d.num_freeze * sizeof(struct Freeze));

    for (int i = 0; i < d.num_freeze; i++) {
        d.freeze[i].first = int64ToIntS(vsapi->propGetInt(in, "first", i, 0));
        d.freeze[i].last = int64ToIntS(vsapi->propGetInt(in, "last", i, 0));
        d.freeze[i].replacement = int64ToIntS(vsapi->propGetInt(in, "replacement", i, 0));

        if (d.freeze[i].first > d.freeze[i].last) {
            int tmp = d.freeze[i].first;
            d.freeze[i].first = d.freeze[i].last;
            d.freeze[i].last = tmp;
        }

        if (d.freeze[i].first < 0 || (d.vi->numFrames && d.freeze[i].last >= d.vi->numFrames) ||
            d.freeze[i].replacement < 0 || (d.vi->numFrames && d.freeze[i].replacement >= d.vi->numFrames)) {
            vsapi->setError(out, "FreezeFrames: out of bounds frame number(s)");
            vsapi->freeNode(d.node);
            free(d.freeze);
            return;
        }
    }

    qsort(d.freeze, d.num_freeze, sizeof(d.freeze[0]), freezeFramesSort);

    for (int i = 0; i < d.num_freeze - 1; i++)
        if (d.freeze[i].last >= d.freeze[i + 1].first) {
            vsapi->setError(out, "FreezeFrames: the frame ranges must not overlap");
            vsapi->freeNode(d.node);
            free(d.freeze);
            return;
        }

    data = malloc(sizeof(d));
    *data = d;

    vsapi->createFilter(in, out, "FreezeFrames", freezeFramesInit, freezeFramesGetFrame, freezeFramesFree, fmParallel, nfNoCache, data, core);
}

//////////////////////////////////////////
// Init

void VS_CC reorderInitialize(VSConfigPlugin configFunc, VSRegisterFunction registerFunc, VSPlugin *plugin) {
    //configFunc("com.vapoursynth.std", "std", "VapourSynth Core Functions", VAPOURSYNTH_API_VERSION, 1, plugin);
    registerFunc("Trim", "clip:clip;first:int:opt;last:int:opt;length:int:opt;", trimCreate, 0, plugin);
    registerFunc("AudioTrim", "clip:anode;first:int:opt;last:int:opt;length:int:opt;", audioTrimCreate, 0, plugin);
    registerFunc("Reverse", "clip:clip;", reverseCreate, 0, plugin);
    registerFunc("Loop", "clip:clip;times:int:opt;", loopCreate, 0, plugin);
    registerFunc("Interleave", "clips:clip[];extend:int:opt;mismatch:int:opt;modify_duration:int:opt;", interleaveCreate, 0, plugin);
    registerFunc("SelectEvery", "clip:clip;cycle:int;offsets:int[];modify_duration:int:opt;", selectEveryCreate, 0, plugin);
    registerFunc("Splice", "clips:clip[];mismatch:int:opt;", spliceCreate, 0, plugin);
    registerFunc("AudioSplice", "clips:anode[];", audioSplice2Wrapper, 0, plugin);
    registerFunc("AudioSplice2", "clip1:anode;clip2:anode;", audioSplice2Create, 0, plugin);
    registerFunc("DuplicateFrames", "clip:clip;frames:int[];", duplicateFramesCreate, 0, plugin);
    registerFunc("DeleteFrames", "clip:clip;frames:int[];", deleteFramesCreate, 0, plugin);
    registerFunc("FreezeFrames", "clip:clip;first:int[];last:int[];replacement:int[];", freezeFramesCreate, 0, plugin);
}
