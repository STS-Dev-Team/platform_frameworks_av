/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "FastMixer"
//#define LOG_NDEBUG 0

#include <sys/atomics.h>
#include <time.h>
#include <utils/Log.h>
#include <system/audio.h>
#ifdef FAST_MIXER_STATISTICS
#include <cpustats/CentralTendencyStatistics.h>
#endif
#include "AudioMixer.h"
#include "FastMixer.h"

#define FAST_HOT_IDLE_NS     1000000L   // 1 ms: time to sleep while hot idling
#define FAST_DEFAULT_NS    999999999L   // ~1 sec: default time to sleep

namespace android {

// Fast mixer thread
bool FastMixer::threadLoop()
{
    static const FastMixerState initial;
    const FastMixerState *previous = &initial, *current = &initial;
    FastMixerState preIdle; // copy of state before we went into idle
    struct timespec oldTs = {0, 0};
    bool oldTsValid = false;
    long slopNs = 0;    // accumulated time we've woken up too early (> 0) or too late (< 0)
    long sleepNs = -1;  // -1: busy wait, 0: sched_yield, > 0: nanosleep
    int fastTrackNames[FastMixerState::kMaxFastTracks]; // handles used by mixer to identify tracks
    int generations[FastMixerState::kMaxFastTracks];    // last observed mFastTracks[i].mGeneration
    unsigned i;
    for (i = 0; i < FastMixerState::kMaxFastTracks; ++i) {
        fastTrackNames[i] = -1;
        generations[i] = 0;
    }
    NBAIO_Sink *outputSink = NULL;
    int outputSinkGen = 0;
    AudioMixer* mixer = NULL;
    short *mixBuffer = NULL;
    enum {UNDEFINED, MIXED, ZEROED} mixBufferState = UNDEFINED;
    NBAIO_Format format = Format_Invalid;
    unsigned sampleRate = 0;
    int fastTracksGen = 0;
    long periodNs = 0;      // expected period; the time required to render one mix buffer
    long underrunNs = 0;    // an underrun is likely if an actual cycle is greater than this value
    long overrunNs = 0;     // an overrun is likely if an actual cycle if less than this value
    FastMixerDumpState dummyDumpState, *dumpState = &dummyDumpState;
    bool ignoreNextOverrun = true;  // used to ignore initial overrun and first after an underrun
#ifdef FAST_MIXER_STATISTICS
    CentralTendencyStatistics cts;  // cycle times in seconds
    static const unsigned kMaxSamples = 1000;
#endif
    unsigned coldGen = 0;   // last observed mColdGen

    for (;;) {

        // either nanosleep, sched_yield, or busy wait
        if (sleepNs >= 0) {
            if (sleepNs > 0) {
                ALOG_ASSERT(sleepNs < 1000000000);
                const struct timespec req = {0, sleepNs};
                nanosleep(&req, NULL);
            } else {
                sched_yield();
            }
        }
        // default to long sleep for next cycle
        sleepNs = FAST_DEFAULT_NS;

        // poll for state change
        const FastMixerState *next = mSQ.poll();
        if (next == NULL) {
            // continue to use the default initial state until a real state is available
            ALOG_ASSERT(current == &initial && previous == &initial);
            next = current;
        }

        FastMixerState::Command command = next->mCommand;
        if (next != current) {

            // As soon as possible of learning of a new dump area, start using it
            dumpState = next->mDumpState != NULL ? next->mDumpState : &dummyDumpState;

            // We want to always have a valid reference to the previous (non-idle) state.
            // However, the state queue only guarantees access to current and previous states.
            // So when there is a transition from a non-idle state into an idle state, we make a
            // copy of the last known non-idle state so it is still available on return from idle.
            // The possible transitions are:
            //  non-idle -> non-idle    update previous from current in-place
            //  non-idle -> idle        update previous from copy of current
            //  idle     -> idle        don't update previous
            //  idle     -> non-idle    don't update previous
            if (!(current->mCommand & FastMixerState::IDLE)) {
                if (command & FastMixerState::IDLE) {
                    preIdle = *current;
                    current = &preIdle;
                    oldTsValid = false;
                    ignoreNextOverrun = true;
                }
                previous = current;
            }
            current = next;
        }
#if !LOG_NDEBUG
        next = NULL;    // not referenced again
#endif

        dumpState->mCommand = command;

        switch (command) {
        case FastMixerState::INITIAL:
        case FastMixerState::HOT_IDLE:
            sleepNs = FAST_HOT_IDLE_NS;
            continue;
        case FastMixerState::COLD_IDLE:
            // only perform a cold idle command once
            if (current->mColdGen != coldGen) {
                int32_t *coldFutexAddr = current->mColdFutexAddr;
                ALOG_ASSERT(coldFutexAddr != NULL);
                int32_t old = android_atomic_dec(coldFutexAddr);
                if (old <= 0) {
                    __futex_syscall4(coldFutexAddr, FUTEX_WAIT_PRIVATE, old - 1, NULL);
                }
                sleepNs = -1;
                coldGen = current->mColdGen;
            } else {
                sleepNs = FAST_HOT_IDLE_NS;
            }
            continue;
        case FastMixerState::EXIT:
            delete mixer;
            delete[] mixBuffer;
            return false;
        case FastMixerState::MIX:
        case FastMixerState::WRITE:
        case FastMixerState::MIX_WRITE:
            break;
        default:
            LOG_FATAL("bad command %d", command);
        }

        // there is a non-idle state available to us; did the state change?
        size_t frameCount = current->mFrameCount;
        if (current != previous) {

            // handle state change here, but since we want to diff the state,
            // we're prepared for previous == &initial the first time through
            unsigned previousTrackMask;

            // check for change in output HAL configuration
            NBAIO_Format previousFormat = format;
            if (current->mOutputSinkGen != outputSinkGen) {
                outputSink = current->mOutputSink;
                outputSinkGen = current->mOutputSinkGen;
                if (outputSink == NULL) {
                    format = Format_Invalid;
                    sampleRate = 0;
                } else {
                    format = outputSink->format();
                    sampleRate = Format_sampleRate(format);
                    ALOG_ASSERT(Format_channelCount(format) == 2);
                }
            }

            if ((format != previousFormat) || (frameCount != previous->mFrameCount)) {
                // FIXME to avoid priority inversion, don't delete here
                delete mixer;
                mixer = NULL;
                delete[] mixBuffer;
                mixBuffer = NULL;
                if (frameCount > 0 && sampleRate > 0) {
                    // FIXME new may block for unbounded time at internal mutex of the heap
                    //       implementation; it would be better to have normal mixer allocate for us
                    //       to avoid blocking here and to prevent possible priority inversion
                    mixer = new AudioMixer(frameCount, sampleRate, FastMixerState::kMaxFastTracks);
                    mixBuffer = new short[frameCount * 2];
                    periodNs = (frameCount * 1000000000LL) / sampleRate;    // 1.00
                    underrunNs = (frameCount * 1750000000LL) / sampleRate;  // 1.75
                    overrunNs = (frameCount * 250000000LL) / sampleRate;    // 0.25
                } else {
                    periodNs = 0;
                    underrunNs = 0;
                    overrunNs = 0;
                }
                mixBufferState = UNDEFINED;
#if !LOG_NDEBUG
                for (i = 0; i < FastMixerState::kMaxFastTracks; ++i) {
                    fastTrackNames[i] = -1;
                }
#endif
                // we need to reconfigure all active tracks
                previousTrackMask = 0;
                fastTracksGen = current->mFastTracksGen - 1;
            } else {
                previousTrackMask = previous->mTrackMask;
            }

            // check for change in active track set
            unsigned currentTrackMask = current->mTrackMask;
            if (current->mFastTracksGen != fastTracksGen) {
                ALOG_ASSERT(mixBuffer != NULL);
                int name;

                // process removed tracks first to avoid running out of track names
                unsigned removedTracks = previousTrackMask & ~currentTrackMask;
                while (removedTracks != 0) {
                    i = __builtin_ctz(removedTracks);
                    removedTracks &= ~(1 << i);
                    const FastTrack* fastTrack = &current->mFastTracks[i];
                    if (mixer != NULL) {
                        name = fastTrackNames[i];
                        ALOG_ASSERT(name >= 0);
                        mixer->deleteTrackName(name);
                    }
#if !LOG_NDEBUG
                    fastTrackNames[i] = -1;
#endif
                    generations[i] = fastTrack->mGeneration;
                }

                // now process added tracks
                unsigned addedTracks = currentTrackMask & ~previousTrackMask;
                while (addedTracks != 0) {
                    i = __builtin_ctz(addedTracks);
                    addedTracks &= ~(1 << i);
                    const FastTrack* fastTrack = &current->mFastTracks[i];
                    AudioBufferProvider *bufferProvider = fastTrack->mBufferProvider;
                    ALOG_ASSERT(bufferProvider != NULL && fastTrackNames[i] == -1);
                    if (mixer != NULL) {
                        name = mixer->getTrackName();
                        ALOG_ASSERT(name >= 0);
                        fastTrackNames[i] = name;
                        mixer->setBufferProvider(name, bufferProvider);
                        mixer->setParameter(name, AudioMixer::TRACK, AudioMixer::MAIN_BUFFER,
                                (void *) mixBuffer);
                        // newly allocated track names default to full scale volume
                        mixer->enable(name);
                    }
                    generations[i] = fastTrack->mGeneration;
                }

                // finally process modified tracks; these use the same slot
                // but may have a different buffer provider or volume provider
                unsigned modifiedTracks = currentTrackMask & previousTrackMask;
                while (modifiedTracks != 0) {
                    i = __builtin_ctz(modifiedTracks);
                    modifiedTracks &= ~(1 << i);
                    const FastTrack* fastTrack = &current->mFastTracks[i];
                    if (fastTrack->mGeneration != generations[i]) {
                        AudioBufferProvider *bufferProvider = fastTrack->mBufferProvider;
                        ALOG_ASSERT(bufferProvider != NULL);
                        if (mixer != NULL) {
                            name = fastTrackNames[i];
                            ALOG_ASSERT(name >= 0);
                            mixer->setBufferProvider(name, bufferProvider);
                            if (fastTrack->mVolumeProvider == NULL) {
                                mixer->setParameter(name, AudioMixer::VOLUME, AudioMixer::VOLUME0,
                                        (void *)0x1000);
                                mixer->setParameter(name, AudioMixer::VOLUME, AudioMixer::VOLUME1,
                                        (void *)0x1000);
                            }
                            // already enabled
                        }
                        generations[i] = fastTrack->mGeneration;
                    }
                }

                fastTracksGen = current->mFastTracksGen;

                dumpState->mNumTracks = popcount(currentTrackMask);
            }

#if 1   // FIXME shouldn't need this
            // only process state change once
            previous = current;
#endif
        }

        // do work using current state here
        if ((command & FastMixerState::MIX) && (mixer != NULL)) {
            ALOG_ASSERT(mixBuffer != NULL);
            // update volumes
            unsigned volumeTracks = current->mTrackMask;
            while (volumeTracks != 0) {
                i = __builtin_ctz(volumeTracks);
                volumeTracks &= ~(1 << i);
                const FastTrack* fastTrack = &current->mFastTracks[i];
                int name = fastTrackNames[i];
                ALOG_ASSERT(name >= 0);
                if (fastTrack->mVolumeProvider != NULL) {
                    uint32_t vlr = fastTrack->mVolumeProvider->getVolumeLR();
                    mixer->setParameter(name, AudioMixer::VOLUME, AudioMixer::VOLUME0,
                            (void *)(vlr & 0xFFFF));
                    mixer->setParameter(name, AudioMixer::VOLUME, AudioMixer::VOLUME1,
                            (void *)(vlr >> 16));
                }
            }
            // process() is CPU-bound
            mixer->process(AudioBufferProvider::kInvalidPTS);
            mixBufferState = MIXED;
        } else if (mixBufferState == MIXED) {
            mixBufferState = UNDEFINED;
        }
        if ((command & FastMixerState::WRITE) && (outputSink != NULL) && (mixBuffer != NULL)) {
            if (mixBufferState == UNDEFINED) {
                memset(mixBuffer, 0, frameCount * 2 * sizeof(short));
                mixBufferState = ZEROED;
            }
            // FIXME write() is non-blocking and lock-free for a properly implemented NBAIO sink,
            //       but this code should be modified to handle both non-blocking and blocking sinks
            dumpState->mWriteSequence++;
            ssize_t framesWritten = outputSink->write(mixBuffer, frameCount);
            dumpState->mWriteSequence++;
            if (framesWritten >= 0) {
                dumpState->mFramesWritten += framesWritten;
            } else {
                dumpState->mWriteErrors++;
            }
            // FIXME count # of writes blocked excessively, CPU usage, etc. for dump
        }

        // To be exactly periodic, compute the next sleep time based on current time.
        // This code doesn't have long-term stability when the sink is non-blocking.
        // FIXME To avoid drift, use the local audio clock or watch the sink's fill status.
        struct timespec newTs;
        int rc = clock_gettime(CLOCK_MONOTONIC, &newTs);
        if (rc == 0) {
            if (oldTsValid) {
                time_t sec = newTs.tv_sec - oldTs.tv_sec;
                long nsec = newTs.tv_nsec - oldTs.tv_nsec;
                if (nsec < 0) {
                    --sec;
                    nsec += 1000000000;
                }
                if (sec > 0 || nsec > underrunNs) {
                    // FIXME only log occasionally
                    ALOGV("underrun: time since last cycle %d.%03ld sec",
                            (int) sec, nsec / 1000000L);
                    dumpState->mUnderruns++;
                    sleepNs = -1;
                    ignoreNextOverrun = true;
                } else if (nsec < overrunNs) {
                    if (ignoreNextOverrun) {
                        ignoreNextOverrun = false;
                    } else {
                        // FIXME only log occasionally
                        ALOGV("overrun: time since last cycle %d.%03ld sec",
                                (int) sec, nsec / 1000000L);
                        dumpState->mOverruns++;
                    }
                    sleepNs = periodNs - overrunNs;
                } else {
                    sleepNs = -1;
                    ignoreNextOverrun = false;
                }
#ifdef FAST_MIXER_STATISTICS
                // long-term statistics
                cts.sample(sec + nsec * 1e-9);
                if (cts.n() >= kMaxSamples) {
                    dumpState->mMean = cts.mean();
                    dumpState->mMinimum = cts.minimum();
                    dumpState->mMaximum = cts.maximum();
                    dumpState->mStddev = cts.stddev();
                    cts.reset();
                }
#endif
            } else {
                // first time through the loop
                oldTsValid = true;
                sleepNs = periodNs;
                ignoreNextOverrun = true;
            }
            oldTs = newTs;
        } else {
            // monotonic clock is broken
            oldTsValid = false;
            sleepNs = periodNs;
        }

    }   // for (;;)

    // never return 'true'; Thread::_threadLoop() locks mutex which can result in priority inversion
}

FastMixerDumpState::FastMixerDumpState() :
    mCommand(FastMixerState::INITIAL), mWriteSequence(0), mFramesWritten(0),
    mNumTracks(0), mWriteErrors(0), mUnderruns(0), mOverruns(0)
#ifdef FAST_MIXER_STATISTICS
    , mMean(0.0), mMinimum(0.0), mMaximum(0.0), mStddev(0.0)
#endif
{
}

FastMixerDumpState::~FastMixerDumpState()
{
}

void FastMixerDumpState::dump(int fd)
{
#define COMMAND_MAX 32
    char string[COMMAND_MAX];
    switch (mCommand) {
    case FastMixerState::INITIAL:
        strcpy(string, "INITIAL");
        break;
    case FastMixerState::HOT_IDLE:
        strcpy(string, "HOT_IDLE");
        break;
    case FastMixerState::COLD_IDLE:
        strcpy(string, "COLD_IDLE");
        break;
    case FastMixerState::EXIT:
        strcpy(string, "EXIT");
        break;
    case FastMixerState::MIX:
        strcpy(string, "MIX");
        break;
    case FastMixerState::WRITE:
        strcpy(string, "WRITE");
        break;
    case FastMixerState::MIX_WRITE:
        strcpy(string, "MIX_WRITE");
        break;
    default:
        snprintf(string, COMMAND_MAX, "%d", mCommand);
        break;
    }
    fdprintf(fd, "FastMixer command=%s writeSequence=%u framesWritten=%u\n"
                 "          numTracks=%u writeErrors=%u underruns=%u overruns=%u\n",
                 string, mWriteSequence, mFramesWritten,
                 mNumTracks, mWriteErrors, mUnderruns, mOverruns);
#ifdef FAST_MIXER_STATISTICS
    fdprintf(fd, "          cycle time in ms: mean=%.1f min=%.1f max=%.1f stddev=%.1f\n",
                 mMean*1e3, mMinimum*1e3, mMaximum*1e3, mStddev*1e3);
#endif
}

}   // namespace android