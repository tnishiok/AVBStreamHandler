/*
 * Copyright (C) 2018 Intel Corporation. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
*/
/**
 * @file    IasAvbVideoRingBufferShm.cpp
 * @brief   Implementation of the video ring buffer that is used to exchange video data via shared memory.
 * @details See header file for details.
 *
 * @date    2018
 */

#include "internal/audio/common/IasIntProcCondVar.hpp"
#include "internal/audio/common/IasIntProcMutex.hpp"
#include "avb_video_common/IasAvbVideoRingBufferShm.hpp"
#include "avb_helper/ias_debug.h"

#include <dlt/dlt_cpp_extension.hpp>

// using IasLockGuard from audio/common
using IasAudio::IasLockGuard;


namespace IasMediaTransportAvb {

static const std::string cClassName = "IasAvbVideoRingBufferShm::";
#define LOG_PREFIX cClassName + __func__ + "(" + std::to_string(__LINE__) + "):"

#define NSEC_PER_SEC 1000000000

// TODO this should be configurable
#define READER_TIMEOUT_NS 2 * NSEC_PER_SEC

IasAvbVideoRingBufferShm::IasAvbVideoRingBufferShm()
  : mBufferSize(0u)
  , mNumBuffers(0u)
  , mReadOffset(0u)
  , mWriteOffset(0u)
  , mBufferLevel(0u)      //!< fill level in TODO: ??? bytes or packets
  , mShared(false)        //!< flag to indicate if the buffer is in shared memory
  , mInitialized(false)
  , mReadInProgress(false)
  , mWriteInProgress(false)
  , mDataBuf(nullptr)
  , mMutex()
  , mMutexReadInProgress()
  , mMutexWriteInProgress()
  , mCondRead()
  , mCondWrite()
  , mReadWaitLevel(0)
  , mWriteWaitLevel(0)
  , mAllowedToWrite(0)
  , mWriterLastAccess(0)
  , mMutexReaders()
  , mReaders{}
{
  // Nothing to do here
}

IasAvbVideoRingBufferShm::~IasAvbVideoRingBufferShm()
{
  // Nothing to do here
}

IasVideoRingBufferResult IasAvbVideoRingBufferShm::init(uint32_t packetSize, uint32_t numBuffers, void* dataBuf, bool shared)
{
  IasVideoRingBufferResult result = eIasRingBuffOk;
  mShared = shared; // not used ATM

  if (0u == packetSize || 0u == numBuffers || nullptr == dataBuf)
  {
    result = eIasRingBuffInvalidParam;
  }
  else
  {
    mBufferSize = packetSize;
    mNumBuffers = numBuffers;
    mDataBuf = dataBuf;
    mInitialized = true;
  }

  DLT_LOG_CXX(getLogContext(), DLT_LOG_DEBUG, LOG_PREFIX, "mBufferSize:", mBufferSize, "mNumBuffers:", mNumBuffers);

  return result;
}


IasVideoRingBufferResult IasAvbVideoRingBufferShm::updateAvailable(IasRingBufferAccess access, pid_t pid, uint32_t *numBuffers)
{
  IasVideoRingBufferResult result = eIasRingBuffOk;

  if (nullptr == numBuffers || eIasRingBufferAccessUndef == access)
  {
    result = eIasRingBuffInvalidParam;
  }
  else if (false == mInitialized)
  {
    result = eIasRingBuffNotInitialized;
  }
  else if (access == eIasRingBufferAccessRead)
  {
    RingBufferReader *reader = findReader(pid);

    if (reader == nullptr)
    {
      result = eIasRingBuffInvalidParam;
    }
    else
    {
      *numBuffers = calculateReaderBufferLevel(reader);
    }
  }
  else
  {
    *numBuffers = mNumBuffers - mBufferLevel;
  }

  return result;
}


IasVideoRingBufferResult IasAvbVideoRingBufferShm::beginAccess(IasRingBufferAccess access, pid_t pid, uint32_t* offset, uint32_t* numBuffers)
{
  IasVideoRingBufferResult result = eIasRingBuffOk;

  if (nullptr == offset || nullptr == numBuffers || eIasRingBufferAccessUndef == access)
  {
    result = eIasRingBuffInvalidParam;
  }
  else if (false == mInitialized)
  {
    result = eIasRingBuffNotInitialized;
  }
  else if (eIasRingBufferAccessRead == access)
  {
    RingBufferReader *reader = findReader(pid);
    if (reader == nullptr)
    {
      result = eIasRingBuffInvalidParam;
    }
    else
    {
      uint32_t bufferLevel = calculateReaderBufferLevel(reader);
      *offset = reader->offset;

      if (*numBuffers > bufferLevel)
      {
        *numBuffers = bufferLevel;
      }
      if ((*offset + *numBuffers) >= mNumBuffers)
      {
        *numBuffers = mNumBuffers - *offset;
      }

      reader->allowedToRead = *numBuffers;
      updateReaderAccess(reader);

      DLT_LOG_CXX(getLogContext(), DLT_LOG_DEBUG, LOG_PREFIX, "Begin read access (", pid, ") *numBuffers:", *numBuffers, "*offset:", *offset, "reader->offset:", reader->offset, "mBufferLevel:", mBufferLevel);
    }
  }
  else // write access
  {
    if (mWriteInProgress)
    {
      result = eIasRingBuffNotAllowed;
    }
    else
    {
      mWriteInProgress.exchange(true);
      mMutexWriteInProgress.lock();
      *offset = mWriteOffset;
      /* mBufferLevel could be changed by any reader process. Loading locally
       * avoid issues caused by its value changing during this function.
       * Using an "old" mBufferLevel should not be a problem, as readers processes
       * will only make it smaller - so we could miss writing some packets now,
       * but that's not a problem. */
      uint32_t bufferLevel = mBufferLevel;

      if ((*numBuffers) > (mNumBuffers - bufferLevel))
      {
        *numBuffers = mNumBuffers - bufferLevel;
      }
      if ((mWriteOffset + *numBuffers) >= mNumBuffers)
      {
        *numBuffers = mNumBuffers - mWriteOffset;
      }
      if (mWriteOffset < mReadOffset)
      {
        *numBuffers = mReadOffset - mWriteOffset - 1;
      }

      mAllowedToWrite = *numBuffers;

      updateWriterAccess();

      DLT_LOG_CXX(getLogContext(), DLT_LOG_DEBUG, LOG_PREFIX, "Begin write access (", pid, ") *numBuffers:", *numBuffers, "*offset:", *offset, "mBufferLevel:", mBufferLevel);
    }
  }

  return result;
}

IasVideoRingBufferResult IasAvbVideoRingBufferShm::endAccess(IasRingBufferAccess access, pid_t pid, uint32_t offset, uint32_t numBuffers)
{
  IasVideoRingBufferResult result = eIasRingBuffOk;
  (void)offset;

  if (eIasRingBufferAccessUndef == access)
  {
    result = eIasRingBuffInvalidParam;
  }
  else if (eIasRingBufferAccessRead == access)
  {
    RingBufferReader *reader = findReader(pid);
    if (reader == nullptr)
    {
      result = eIasRingBuffInvalidParam;
    }
    else if (numBuffers > reader->allowedToRead)
    {
      result = eIasRingBuffInvalidParam;

      DLT_LOG_CXX(getLogContext(), DLT_LOG_INFO, LOG_PREFIX, "End access FAIL: mBufferLevel", mBufferLevel, "numBuffers", numBuffers, "offset", offset, "allowedToRead", reader->allowedToRead);
    }
    else
    {
      reader->allowedToRead = 0;
      reader->offset += numBuffers;
      aggregateReaderOffset();

      if (mBufferLevel <= mWriteWaitLevel)
      {
        mCondWrite.broadcast();
      }

      updateReaderAccess(reader);

      DLT_LOG_CXX(getLogContext(), DLT_LOG_DEBUG, LOG_PREFIX, "End read access (", pid, ") numBuffers:", numBuffers, "offset:", offset, "reader->offset", reader->offset);
    }
  }
  else
  {
    if (mWriteInProgress)
    {
      if (numBuffers > mAllowedToWrite)
      {
        result = eIasRingBuffInvalidParam;
      }
      else
      {
        mAllowedToWrite = 0;

        IasLockGuard lock(&mMutex);
        if ((mWriteOffset + numBuffers) == mNumBuffers)
        {
          mWriteOffset = 0;
        }
        else if ((mWriteOffset + numBuffers) > mNumBuffers)
        {
          return eIasRingBuffInvalidParam;
        }
        else
        {
          mWriteOffset += numBuffers;
        }
        mBufferLevel += numBuffers;

        mWriteInProgress.exchange(false);
        mMutexWriteInProgress.unlock();
        if (mBufferLevel >= mReadWaitLevel)
        {
          mCondRead.broadcast();
        }

        updateWriterAccess();
        purgeUnresponsiveReaders();
      }
    }
  }

  return result;
}


IasVideoRingBufferResult IasAvbVideoRingBufferShm::waitWrite(uint32_t numBuffers, uint32_t timeout_ms)
{
  IasVideoRingBufferResult result = eIasRingBuffOk;

  if ((numBuffers > mNumBuffers) || 0u == numBuffers || 0u == timeout_ms)
  {
    result = eIasRingBuffInvalidParam;
  }
  else
  {
    mWriteWaitLevel = mNumBuffers - numBuffers;
    IasAvbVideoCondVar::IasResult cndres = IasAvbVideoCondVar::eIasOk;

    while (mBufferLevel > mWriteWaitLevel)
    {
      cndres = mCondWrite.wait(timeout_ms);
      if (cndres == IasAvbVideoCondVar::eIasTimeout)
      {
        // Timeout happened, but if our predicate for ending wait is now true, just return OK
        result = mBufferLevel > mWriteWaitLevel ? eIasRingBuffTimeOut : eIasRingBuffOk;
        break;
      }
      else if (cndres != IasAvbVideoCondVar::eIasOk)
      {
        result = eIasRingBuffCondWaitFailed;
        break;
      }
    }
  }

  return result;
}


IasVideoRingBufferResult IasAvbVideoRingBufferShm::waitRead(pid_t pid, uint32_t numBuffers, uint32_t timeout_ms)
{
  IasVideoRingBufferResult result = eIasRingBuffOk;
  RingBufferReader *reader = findReader(pid);

  if ((numBuffers > mNumBuffers) || 0u == numBuffers || 0u == timeout_ms || reader == nullptr)
  {
    result = eIasRingBuffInvalidParam;
  }
  else
  {
    // mReadWaitLevel should have the smaller level for all readers
    /* mMutex protects mReadWaitLevel from being (mis)updated by other readers */
    mMutex.lock();
    if (numBuffers < mReadWaitLevel)
    {
      mReadWaitLevel = numBuffers;
    }
    mMutex.unlock();

    updateReaderAccess(reader);
    IasAvbVideoCondVar::IasResult cndres = IasAvbVideoCondVar::eIasOk;
    while (calculateReaderBufferLevel(reader) < numBuffers)
    {
      cndres = mCondRead.wait(timeout_ms);
      updateReaderAccess(reader);
      if (cndres == IasAvbVideoCondVar::eIasTimeout)
      {
        // Timeout happened, but if our predicate for ending wait is now true, just return OK
        result = calculateReaderBufferLevel(reader) < numBuffers ? eIasRingBuffTimeOut : eIasRingBuffOk;
        break;
      }
      else if (cndres != IasAvbVideoCondVar::eIasOk)
      {
        result = eIasRingBuffCondWaitFailed;
        break;
      }
    }
  }

  return result;
}

IasVideoRingBufferResult IasAvbVideoRingBufferShm::addReader(pid_t pid)
{
  IasVideoRingBufferResult result = eIasRingBuffTooManyReaders;

  if (pid <= 0)
  {
    result = eIasRingBuffInvalidParam;
  }
  else
  {
    mMutexReaders.lock();
    for (int i = 0; i < cIasVideoRingBufferShmMaxReaders; i++)
    {
      if (mReaders[i].pid == 0)
      {
        mReaders[i].pid = pid;
        mReaders[i].offset = mReadOffset;
        updateReaderAccess(&mReaders[i]);
        result = eIasRingBuffOk;
        break;
      }
    }
    mMutexReaders.unlock();
  }

  return result;
}

IasVideoRingBufferResult IasAvbVideoRingBufferShm::removeReader(pid_t pid)
{
  IasVideoRingBufferResult result = eIasRingBuffInvalidParam;

  if (pid > 0)
  {
    mMutexReaders.lock();
    for (int i = 0; i < cIasVideoRingBufferShmMaxReaders; i++)
    {
      if (mReaders[i].pid == pid)
      {
        mReaders[i].pid = 0;
        mReaders[i].offset = 0;
        mReaders[i].lastAccess = 0;
        mReaders[i].allowedToRead = 0;
        result = eIasRingBuffOk;
      }
    }
    mMutexReaders.unlock();
  }

  return result;
}

uint32_t IasAvbVideoRingBufferShm::updateSmallerReaderOffset()
{
  IasLockGuard lock(&mMutexReaders);
  uint32_t smallerOffset = UINT32_MAX;

  // First, find out until where the slower reader read
  for (int i = 0; i < cIasVideoRingBufferShmMaxReaders; i++)
  {
    if (mReaders[i].pid != 0)
    {
      DLT_LOG_CXX(getLogContext(), DLT_LOG_DEBUG, LOG_PREFIX, "reader: ", mReaders[i].pid, "offset:", mReaders[i].offset);

      if (mReaders[i].offset < smallerOffset)
      {
        smallerOffset = mReaders[i].offset;
      }
    }
  }

  if (smallerOffset == UINT32_MAX)
  {
    // No readers
    return smallerOffset;
  }

  DLT_LOG_CXX(getLogContext(), DLT_LOG_DEBUG, LOG_PREFIX, "smallerOffset:", smallerOffset, "mBufferLevel:", mBufferLevel);

  // When all readers read everything, time to reset their offsets
  if (smallerOffset == mNumBuffers)
  {
    for (int i = 0; i < cIasVideoRingBufferShmMaxReaders; i++)
    {
      if (mReaders[i].pid != 0)
      {
        mReaders[i].offset = 0;
      }
    }
  }

  return smallerOffset;
}

void IasAvbVideoRingBufferShm::aggregateReaderOffset()
{
  uint32_t smallerOffset = updateSmallerReaderOffset();

  IasLockGuard lock(&mMutex);

  // Fill level decreased by the difference between current offset and previous
  mBufferLevel -= (smallerOffset - mReadOffset);

  if (smallerOffset == mNumBuffers)
  {
    mReadOffset = 0;
  }
  else if (smallerOffset < mNumBuffers)
  {
    mReadOffset = smallerOffset;
  }

  DLT_LOG_CXX(getLogContext(), DLT_LOG_DEBUG, LOG_PREFIX, "mBufferLevel:", mBufferLevel, "mReadOffset:", mReadOffset, "mWriteOffset:", mWriteOffset);
}

uint32_t IasAvbVideoRingBufferShm::calculateReaderBufferLevel(RingBufferReader *reader)
{
    // mBufferLevel has the overall buffer level, relative to the slowest reader.
    // Other readers should have a smaller buffer level, or, a small number of
    // buffers available to read
    // TODO world would be a better place if `mNumBuffers` was a power of two. Can we enforce that?
    // So the calculation would be simply:
    // bufferLevel = (mWriteOffset - reader->offset) % mNumBuffers;
    uint32_t bufferLevel;

    /* mWriteOffset could be changed by writter process. Loading locally avoid
     * issues caused by its value changing during this function. Using an "old"
     * mWriteOffset is not an issue, as it only grows - so, we could miss
     * reading some packets now, but that's not a problem. The case when
     * mWriteOffset goes back to zero is because it reached end of ringbuffer
     * - again, not a problem, as we'll eventually catch up */
    uint32_t writeOffset = mWriteOffset;

    if (writeOffset >= reader->offset)
    {
      bufferLevel = writeOffset - reader->offset;
    }
    else
    {
      bufferLevel = mNumBuffers - reader->offset + writeOffset;
    }

    DLT_LOG_CXX(getLogContext(), DLT_LOG_DEBUG, LOG_PREFIX, "Buffer level for pid", reader->pid, ", ", bufferLevel);

    return bufferLevel;
}

void IasAvbVideoRingBufferShm::updateReaderAccess(RingBufferReader *reader)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);

  reader->lastAccess = ts.tv_sec * NSEC_PER_SEC + ts.tv_nsec;
}

void IasAvbVideoRingBufferShm::updateWriterAccess()
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);

  mWriterLastAccess = ts.tv_sec * NSEC_PER_SEC + ts.tv_nsec;
}

void IasAvbVideoRingBufferShm::purgeUnresponsiveReaders()
{
  struct timespec ts;
  uint64_t now;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  now = ts.tv_sec * NSEC_PER_SEC + ts.tv_nsec;

  IasLockGuard readersLock(&mMutexReaders);
  for (int i = 0; i < cIasVideoRingBufferShmMaxReaders; i++)
  {
    if (mReaders[i].pid != 0)
    {
      uint64_t lastAccess = mReaders[i].lastAccess;
      if ((now > lastAccess) && ((now - lastAccess) > READER_TIMEOUT_NS)) {
        DLT_LOG_CXX(getLogContext(), DLT_LOG_INFO, "Purging reader", mReaders[i].pid, "after", (now - lastAccess), "ns");
        mReaders[i].pid = 0;
        mReaders[i].offset = 0;
        mReaders[i].lastAccess = 0;
        mReaders[i].allowedToRead = 0;
      }
    }
  }
}

} // namespace IasMediaTransportAvb



