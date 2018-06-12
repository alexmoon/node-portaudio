/* Copyright 2017 Streampunk Media Ltd.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/

#include <nan.h>
#include "common.h"
#include "AudioOut.h"
#include "Persist.h"
#include "Params.h"
#include "ChunkQueue.h"
#include <mutex>
#include <condition_variable>
#include <portaudio.h>

using namespace v8;

namespace streampunk {

extern bool DEBUG;

class AudioChunk {
public:
  AudioChunk (Local<Object> chunk)
    : mPersistentChunk(new Persist(chunk)),
      mChunk(Memory::makeNew((uint8_t *)node::Buffer::Data(chunk), (uint32_t)node::Buffer::Length(chunk)))
    { }
  ~AudioChunk() { }
  
  std::shared_ptr<Memory> chunk() const { return mChunk; }

private:
  std::unique_ptr<Persist> mPersistentChunk;
  std::shared_ptr<Memory> mChunk;
};

class OutContext {
public:
  OutContext(std::shared_ptr<AudioOptions> audioOptions, PaStreamCallback *cb)
    : mAudioOptions(audioOptions), mChunkQueue(mAudioOptions->maxQueue()), 
      mCurOffset(0) {

    // set DEBUG flag based on options
    DEBUG = audioOptions->debugMode();

    PaError errCode = Pa_Initialize();
    if (errCode != paNoError) {
      std::string err = std::string("Could not initialize PortAudio: ") + Pa_GetErrorText(errCode);
      Nan::ThrowError(err.c_str());
    }

    DEBUG_PRINT_ERR("Output %s\n", mAudioOptions->toString().c_str());

    PaStreamParameters outParams;
    memset(&outParams, 0, sizeof(PaStreamParameters));

    int32_t deviceID = (int32_t)mAudioOptions->deviceID();
    if ((deviceID >= 0) && (deviceID < Pa_GetDeviceCount()))
      outParams.device = (PaDeviceIndex)deviceID;
    else
      outParams.device = Pa_GetDefaultOutputDevice();
    if (outParams.device == paNoDevice)
      Nan::ThrowError("No default output device");
    DEBUG_PRINT_ERR("Output device name is %s\n", Pa_GetDeviceInfo(outParams.device)->name);

    outParams.channelCount = mAudioOptions->channelCount();
    if (outParams.channelCount > Pa_GetDeviceInfo(outParams.device)->maxOutputChannels)
      Nan::ThrowError("Channel count exceeds maximum number of output channels for device");

    uint32_t sampleFormat = mAudioOptions->sampleFormat();
    switch(sampleFormat) {
    case 8: outParams.sampleFormat = paInt8; break;
    case 16: outParams.sampleFormat = paInt16; break;
    case 24: outParams.sampleFormat = paInt24; break;
    case 32: outParams.sampleFormat = paInt32; break;
    default: Nan::ThrowError("Invalid sampleFormat");
    }

    outParams.suggestedLatency = mAudioOptions->suggestedLatency();
    if (outParams.suggestedLatency == 0.0) {
      outParams.suggestedLatency = Pa_GetDeviceInfo(outParams.device)->defaultLowOutputLatency;
    }
    outParams.hostApiSpecificStreamInfo = NULL;

    uint32_t framesPerBuffer = paFramesPerBufferUnspecified;

    errCode = Pa_OpenStream(&mStream, NULL, &outParams, mAudioOptions->sampleRate(),
                            framesPerBuffer, paNoFlag, cb, this);
    if (errCode != paNoError) {
      std::string err = std::string("Could not open stream: ") + Pa_GetErrorText(errCode);
      Nan::ThrowError(err.c_str());
    }
  }
  
  ~OutContext() {
    Pa_Terminate();
  }

  void start() {
    PaError errCode = Pa_StartStream(mStream);
    if (errCode != paNoError) {
      std::string err = std::string("Could not start output stream: ") + Pa_GetErrorText(errCode);
      return Nan::ThrowError(err.c_str());
    }
  }

  PaError stop() {
    if (Pa_IsStreamStopped(mStream) == 1) {
      return paNoError;
    } else if (Pa_IsStreamActive(mStream) == 1) {
      std::unique_lock<std::mutex> lk(m);
      while(Pa_IsStreamActive(mStream) == 1) { cv.wait(lk); }
    }
    return Pa_StopStream(mStream);
  }

  void close() {
    Pa_CloseStream(mStream);
  }

  PaError addChunk(std::shared_ptr<AudioChunk> audioChunk) {
    mChunkQueue.enqueue(audioChunk);
    if (Pa_IsStreamActive(mStream) == 0) {
      if (Pa_IsStreamStopped(mStream) == 0) {
        Pa_StopStream(mStream);
      }
      return Pa_StartStream(mStream);
    } else {
      return paNoError;
    }
  }

  bool fillBuffer(void *buf, uint32_t frameCount) {
    uint8_t *dst = (uint8_t *)buf;
    uint32_t bytesRemaining = frameCount * mAudioOptions->channelCount() * mAudioOptions->sampleFormat() / 8;

    while (bytesRemaining) {
      if (mCurChunk && mCurOffset < mCurChunk->chunk()->numBytes()) {
        uint32_t bytesCopied = doCopy(mCurChunk->chunk(), dst, bytesRemaining);
        bytesRemaining -= bytesCopied;
        dst += bytesCopied;
        mCurOffset += bytesCopied;
      } else if (mChunkQueue.size() > 0) {
        mCurChunk = mChunkQueue.dequeue();
        mCurOffset = 0;
      } else {
        memset(dst, 0, bytesRemaining);
        std::lock_guard<std::mutex> lk(m);
        cv.notify_all();
        return false;
      }
    }

    return true;
  }

  void checkStatus(uint32_t statusFlags) {
    if (statusFlags) {
      std::string err = std::string("portAudio status - ");
      if (statusFlags & paOutputUnderflow)
        err += "output underflow ";
      if (statusFlags & paOutputOverflow)
        err += "output overflow ";
      if (statusFlags & paPrimingOutput)
        err += "priming output ";

      std::lock_guard<std::mutex> lk(m);
      mErrStr = err;
    }
  }

  bool getErrStr(std::string& errStr) {
    std::lock_guard<std::mutex> lk(m);
    errStr = mErrStr;
    mErrStr = std::string();
    return errStr.size() > 0;
  }

private:
  std::shared_ptr<AudioOptions> mAudioOptions;
  ChunkQueue<std::shared_ptr<AudioChunk> > mChunkQueue;
  std::shared_ptr<AudioChunk> mCurChunk;
  PaStream* mStream;
  uint32_t mCurOffset;
  std::string mErrStr;
  mutable std::mutex m;
  std::condition_variable cv;

  uint32_t doCopy(std::shared_ptr<Memory> chunk, void *dst, uint32_t numBytes) {
    uint32_t curChunkBytes = chunk->numBytes() - mCurOffset;
    uint32_t thisChunkBytes = std::min<uint32_t>(curChunkBytes, numBytes);
    memcpy(dst, chunk->buf() + mCurOffset, thisChunkBytes);
    return thisChunkBytes;
  }
};

int OutCallback(const void *input, void *output, unsigned long frameCount, 
               const PaStreamCallbackTimeInfo *timeInfo, 
               PaStreamCallbackFlags statusFlags, void *userData) {
  OutContext *context = (OutContext *)userData;
  context->checkStatus(statusFlags);
  return context->fillBuffer(output, frameCount) ? paContinue : paComplete;
}

class OutWorker : public Nan::AsyncWorker {
  public:
    OutWorker(std::shared_ptr<OutContext> OutContext, Nan::Callback *callback, std::shared_ptr<AudioChunk> audioChunk)
      : AsyncWorker(callback), mOutContext(OutContext), mAudioChunk(audioChunk) 
    { }
    ~OutWorker() {}

    void Execute() {
      std::string errStr;
      PaError errCode = mOutContext->addChunk(mAudioChunk);
      if (errCode != paNoError) {
        SetErrorMessage(Pa_GetErrorText(errCode));
      } else if (mOutContext->getErrStr(errStr)) {
        SetErrorMessage(errStr.c_str());
      }
    }

  private:
    std::shared_ptr<OutContext> mOutContext;
    std::shared_ptr<AudioChunk> mAudioChunk;
};

class StopOutWorker : public Nan::AsyncWorker {
  public:
    StopOutWorker(std::shared_ptr<OutContext> OutContext, Nan::Callback *callback)
      : AsyncWorker(callback), mOutContext(OutContext)
    { }
    ~StopOutWorker() {}

    void Execute() {
      PaError errCode = mOutContext->stop();
      if (errCode != paNoError) {
        SetErrorMessage(Pa_GetErrorText(errCode));
      }
    }

  private:
    std::shared_ptr<OutContext> mOutContext;
};

AudioOut::AudioOut(Local<Object> options) { 
  mOutContext = std::make_shared<OutContext>(std::make_shared<AudioOptions>(options), OutCallback);
}
AudioOut::~AudioOut() {}

NAN_METHOD(AudioOut::Write) {
  if (info.Length() != 2)
    return Nan::ThrowError("AudioOut Write expects 2 arguments");
  if (!info[0]->IsObject())
    return Nan::ThrowError("AudioOut Write requires a valid chunk buffer as the first parameter");
  if (!info[1]->IsFunction())
    return Nan::ThrowError("AudioOut Write requires a valid callback as the second parameter");

  Local<Object> chunkObj = Local<Object>::Cast(info[0]);
  Local<Function> callback = Local<Function>::Cast(info[1]);
  AudioOut* obj = Nan::ObjectWrap::Unwrap<AudioOut>(info.Holder());

  AsyncQueueWorker(new OutWorker(obj->getContext(), new Nan::Callback(callback), std::make_shared<AudioChunk>(chunkObj)));
  info.GetReturnValue().SetUndefined();
}

NAN_METHOD(AudioOut::Stop) {
  if (info.Length() != 1)
    return Nan::ThrowError("AudioOut Stop expects 1 argument");
  if (!info[0]->IsFunction())
    return Nan::ThrowError("AudioOut Stop requires a valid callback as the parameter");

  Local<Function> callback = Local<Function>::Cast(info[0]);
  AudioOut* obj = Nan::ObjectWrap::Unwrap<AudioOut>(info.Holder());

  AsyncQueueWorker(new StopOutWorker(obj->getContext(), new Nan::Callback(callback)));
  info.GetReturnValue().SetUndefined();
}

NAN_METHOD(AudioOut::Close) {
  AudioOut* obj = Nan::ObjectWrap::Unwrap<AudioOut>(info.Holder());
  obj->mOutContext->close();
  info.GetReturnValue().SetUndefined();
}

NAN_MODULE_INIT(AudioOut::Init) {
  Local<FunctionTemplate> tpl = Nan::New<FunctionTemplate>(New);
  tpl->SetClassName(Nan::New("AudioOut").ToLocalChecked());
  tpl->InstanceTemplate()->SetInternalFieldCount(1);

  SetPrototypeMethod(tpl, "write", Write);
  SetPrototypeMethod(tpl, "stop", Stop);
  SetPrototypeMethod(tpl, "close", Close);

  constructor().Reset(Nan::GetFunction(tpl).ToLocalChecked());
  Nan::Set(target, Nan::New("AudioOut").ToLocalChecked(),
    Nan::GetFunction(tpl).ToLocalChecked());
}

} // namespace streampunk