
#include "nodePortAudio.h"
#include <portaudio.h>

#define FRAMES_PER_BUFFER  (64)

int g_initialized = false;

struct PortAudioData {
  unsigned char* buffer;
  int bufferLen;
  int readIdx;
  int writeIdx;
  int sampleFormat;
  v8::Persistent<v8::Object> v8Stream;
};

static int nodePortAudioCallback(
  const void *inputBuffer,
  void *outputBuffer,
  unsigned long framesPerBuffer,
  const PaStreamCallbackTimeInfo* timeInfo,
  PaStreamCallbackFlags statusFlags,
  void *userData);

v8::Handle<v8::Value> stream_writeByte(const v8::Arguments& args);

v8::Handle<v8::Value> Open(const v8::Arguments& args) {
  v8::HandleScope scope;
  PaError err;
  PaStreamParameters outputParameters;
  v8::Local<v8::Value> v8Val;
  v8::Local<v8::Object> v8Buffer;
  v8::Local<v8::Object> v8Stream;
  PortAudioData* data;
  PaStream* stream;
  int sampleRate;
  v8::Local<v8::FunctionTemplate> fnTemplate;
  char str[1000];

  v8::Handle<v8::Value> argv[2];
  argv[0] = v8::Undefined();
  argv[1] = v8::Undefined();

  // options
  if(!args[0]->IsObject()) {
    return scope.Close(v8::ThrowException(v8::Exception::TypeError(v8::String::New("First argument must be an object"))));
  }
  v8::Local<v8::Object> options = args[0]->ToObject();

  // callback
  if(!args[1]->IsFunction()) {
    return scope.Close(v8::ThrowException(v8::Exception::TypeError(v8::String::New("Second argument must be a function"))));
  }
  v8::Local<v8::Value> callback = args[1];

  if(!g_initialized) {
    err = Pa_Initialize();
    if(err != paNoError) {
      sprintf(str, "Could not initialize PortAudio %d", err);
      argv[0] = v8::Exception::TypeError(v8::String::New(str));
      goto openDone;
    };
    g_initialized = true;
  }

  memset(&outputParameters, 0, sizeof(PaStreamParameters));

  outputParameters.device = Pa_GetDefaultOutputDevice();
  if (outputParameters.device == paNoDevice) {
    sprintf(str, "No default output device");
    argv[0] = v8::Exception::TypeError(v8::String::New(str));
    goto openDone;
  }

  v8Val = options->Get(v8::String::New("channelCount"));
  outputParameters.channelCount = v8Val->ToInt32()->Value();

  v8Val = options->Get(v8::String::New("sampleFormat"));
  switch(v8Val->ToInt32()->Value()) {
  case 8:
    outputParameters.sampleFormat = paInt8;
    break;
  default:
    argv[0] = v8::Exception::TypeError(v8::String::New("Invalid sampleFormat"));
    goto openDone;
  }
  outputParameters.suggestedLatency = Pa_GetDeviceInfo( outputParameters.device )->defaultLowOutputLatency;
  outputParameters.hostApiSpecificStreamInfo = NULL;

  v8Val = options->Get(v8::String::New("sampleRate"));
  sampleRate = v8Val->ToInt32()->Value();

  data = new PortAudioData();
  data->readIdx = 0;
  data->writeIdx = 0;
  data->sampleFormat = outputParameters.sampleFormat;
  v8Stream = options->Get(v8::String::New("stream"))->ToObject();

  // TODO: must be a better way
  sprintf(str, "%p", data);
  v8Stream->Set(v8::String::New("_data"), v8::String::New(str));

  v8Buffer = v8Stream->Get(v8::String::New("buffer"))->ToObject();
  data->v8Stream = v8::Persistent<v8::Object>::New(v8Stream);
  // TODO: make week and close on complete

  data->buffer = (unsigned char*)node::Buffer::Data(v8Buffer);
  data->bufferLen = node::Buffer::Length(v8Buffer);

  err = Pa_OpenStream(
    &stream,
    NULL, // no input
    &outputParameters,
    sampleRate,
    FRAMES_PER_BUFFER,
    paClipOff, // we won't output out of range samples so don't bother clipping them
    nodePortAudioCallback,
    data);
  if(err != paNoError) {
    sprintf(str, "Could not open stream %d", err);
    argv[0] = v8::Exception::TypeError(v8::String::New(str));
    goto openDone;
  }

  err = Pa_StartStream(stream);
  if(err != paNoError) {
    sprintf(str, "Could not start stream %d", err);
    argv[0] = v8::Exception::TypeError(v8::String::New(str));
    goto openDone;
  }

  fnTemplate = v8::FunctionTemplate::New(stream_writeByte);
  v8Stream->Set(v8::String::New("writeByte"), fnTemplate->GetFunction());

  argv[1] = v8Stream;

openDone:
  v8::Function::Cast(*callback)->Call(v8::Context::GetCurrent()->Global(), 2, argv);
  return scope.Close(v8::Undefined());
}

v8::Handle<v8::Value> stream_writeByte(const v8::Arguments& args) {
  v8::HandleScope scope;
  PortAudioData* data;

  v8::String::AsciiValue v(args.This()->Get(v8::String::New("_data")));
  sscanf(*v, "%p", &data);

  int val = args[0]->ToInt32()->Value();
  data->buffer[data->writeIdx++] = val;

  return scope.Close(v8::Undefined());
}

static int nodePortAudioCallback(
  const void *inputBuffer,
  void *outputBuffer,
  unsigned long framesPerBuffer,
  const PaStreamCallbackTimeInfo* timeInfo,
  PaStreamCallbackFlags statusFlags,
  void *userData)
{
  PortAudioData* data = (PortAudioData*)userData;
  unsigned long i;

  switch(data->sampleFormat) {
  case paInt8:
    {
      unsigned char* out = (unsigned char*)outputBuffer;
      for(i = 0; i < framesPerBuffer; i++) {
        *out++ = data->buffer[data->readIdx++];
      }
    }
    break;
  }

  return paContinue;
}
