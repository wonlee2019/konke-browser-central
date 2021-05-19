/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef TRACING_H
#define TRACING_H

#include <algorithm>
#include <cstdint>
#include <cstdio>

#include "AsyncLogger.h"

#include "mozilla/Attributes.h"
#include "mozilla/UniquePtr.h"

#if defined(_MSC_VER)
// MSVC
#  define FUNCTION_SIGNATURE __FUNCSIG__
#elif defined(__GNUC__)
// gcc, clang
#  define FUNCTION_SIGNATURE __PRETTY_FUNCTION__
#endif

extern mozilla::AsyncLogger gAudioCallbackTraceLogger;

// This is no-op if tracing is not enabled, and is idempotent.
void StartAudioCallbackTracing();
void StopAudioCallbackTracing();

#ifdef TRACING
/* TRACE is for use in the real-time audio rendering thread.
 * It would be better to always pass in the thread id. However, the thread an
 * audio callback runs on can change when the underlying audio device change,
 * and also it seems to be called from a thread pool in a round-robin fashion
 * when audio remoting is activated, making the traces unreadable.
 * The thread on which the AudioCallbackDriver::DataCallback is to always
 * be thread 0, and the budget is set to always be thread 1. This allows
 * displaying those elements in two separate lanes.
 * The other thread have "normal" tid. Hashing allows being able to get a
 * string representation that is unique and guaranteed to be portable. */
#  define TRACE_AUDIO_CALLBACK() \
    AutoTracer trace(gAudioCallbackTraceLogger, FUNCTION_SIGNATURE);
#  define TRACE_AUDIO_CALLBACK_BUDGET(aFrames, aSampleRate)          \
    AutoTracer budget(gAudioCallbackTraceLogger, "Real-time budget", \
                      AutoTracer::EventType::BUDGET, aFrames, aSampleRate);
#  define TRACE_AUDIO_CALLBACK_COMMENT(aFmt, ...)                   \
    AutoTracer trace(gAudioCallbackTraceLogger, FUNCTION_SIGNATURE, \
                     AutoTracer::EventType::DURATION, aFmt, ##__VA_ARGS__);
#  define TRACE() \
    AutoTracer trace(gAudioCallbackTraceLogger, FUNCTION_SIGNATURE);
#  define TRACE_COMMENT(aFmt, ...)                                  \
    AutoTracer trace(gAudioCallbackTraceLogger, FUNCTION_SIGNATURE, \
                     AutoTracer::EventType::DURATION, aFmt, ##__VA_ARGS__);
#else
#  define TRACE()
#  define TRACE_AUDIO_CALLBACK_BUDGET(aFrames, aSampleRate)
#  define TRACE_COMMENT(aFmt, ...)
#endif

class MOZ_RAII AutoTracer {
 public:
  static const int32_t BUFFER_SIZE = 256;

  enum class EventType { DURATION, BUDGET };

  AutoTracer(mozilla::AsyncLogger& aLogger, const char* aLocation,
             EventType aEventType = EventType::DURATION,
             const char* aComment = nullptr);

  template <typename... Args>
  AutoTracer(mozilla::AsyncLogger& aLogger, const char* aLocation,
             EventType aEventType, const char* aFormat, Args... aArgs)
      : mLogger(aLogger),
        mLocation(aLocation),
        mComment(mBuffer),
        mEventType(aEventType) {
    MOZ_ASSERT(aEventType == EventType::DURATION);
    if (aLogger.Enabled()) {
      int32_t size = snprintf(mBuffer, BUFFER_SIZE, aFormat, aArgs...);
      size = std::min(size, BUFFER_SIZE - 1);
      mBuffer[size] = 0;
      PrintEvent(aLocation, "perf", mComment,
                 mozilla::AsyncLogger::TracingPhase::BEGIN);
    }
  }

  AutoTracer(mozilla::AsyncLogger& aLogger, const char* aLocation,
             EventType aEventType, uint64_t aFrames, uint64_t aSampleRate);

  ~AutoTracer();

 private:
  void PrintEvent(const char* aName, const char* aCategory,
                  const char* aComment,
                  mozilla::AsyncLogger::TracingPhase aPhase);

  void PrintBudget(const char* aName, const char* aCategory, uint64_t aDuration,
                   uint64_t aFrames, uint64_t aSampleRate);

  // The logger to use. It musdt have a lifetime longer than the block an
  // instance of this class traces.
  mozilla::AsyncLogger& mLogger;
  // The location for this trace point, arbitrary string literal, often the
  // name of the calling function, with a static lifetime.
  const char* mLocation;
  // A comment for this trace point, abitrary string literal with a static
  // lifetime.
  const char* mComment;
  // A buffer used to hold string-formatted traces.
  char mBuffer[BUFFER_SIZE];
  // The event type, for now either a budget or a duration.
  const EventType mEventType;
};

#endif /* TRACING_H */