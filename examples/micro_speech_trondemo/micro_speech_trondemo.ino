// This demo of micro speech uses the timer to check if the button is pressed.
// When pressed, the button will record audio (neopixels turns red). Once released
// the tensorflow lite runtime will pick up the audio samples and perform the analysis
// We display an animated GIF and play matching audio clip on detection.
// This example uses Arcada to handle buttons, graphics, and filesystem stuff
// If compiled with TinyUSB the disk drive will  be exposed, but its incredibly slow
// because TFLite hogs the CPU, so its not recommended for file transfer

/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

// Include an empty header so that Arduino knows to build the TF Lite library.
#include <TensorFlowLite.h>
#include "tensorflow/lite/experimental/micro/examples/micro_speech/command_responder.h"
#include "tensorflow/lite/experimental/micro/examples/micro_speech/micro_features/micro_model_settings.h"
#include <Adafruit_Arcada.h>
#include "introwav.h"
#include "yes-16k-mono-8bit.h"
#include "no-16k-mono-8bit.h"

bool loadTFConfigFile(const char *filename="/tron/tflite_config.json");
StaticJsonDocument<256> TFconfigJSON;  ///< The object to store our various settings

#define LED_OUT       13
#define AUDIO_IN      A8  // Stemma D2
#define AUDIO_OUT     A0

Adafruit_Arcada arcada;
// GIF stuff!
void initGIFDecode();
bool playGIF(const char *filename, uint8_t times=1);

#define BUFFER_SIZE (kAudioSampleFrequency*3) // up to 3 seconds of audio
volatile uint32_t audio_idx = 0;
volatile uint32_t recording_length = 0;
int16_t *recording_buffer;
volatile uint32_t audio_timestamp_ms;
const uint8_t *play_buffer;
volatile uint32_t play_length;

uint8_t kCategoryCount = 0;
char **kCategoryLabels;
unsigned char *g_tiny_conv_micro_features_model_data;
int g_tiny_conv_micro_features_model_data_len;

volatile bool val;
volatile bool isRecording, isPlaying;
volatile uint8_t button_counter = 0;
void TimerCallback() {
  if (isRecording) {
    digitalWrite(LED_OUT, val);  // tick tock test
    val = !val;
    int16_t sample = analogRead(AUDIO_IN);
    sample -= 2047; // 12 bit audio unsigned  0-4095 to signed -2048-~2047
    sample *= 16;   // convert 12 bit to 16 bit
    recording_buffer[audio_idx] = sample;
    audio_idx++;
    if (audio_idx == BUFFER_SIZE) {
      isRecording = false;
    }
  }
  if (isPlaying) {
    digitalWrite(LED_OUT, val);
    val = !val;
    int16_t sample = play_buffer[audio_idx];
    analogWrite(AUDIO_OUT, sample);
    audio_idx++;
    if (audio_idx == play_length) {
      isPlaying = false;
    }
  }
  button_counter++;
  if (button_counter == 160) { // this is called once every 10 millisecond!
    button_counter = 0;
    
    uint8_t pressed_buttons = arcada.readButtons();
    
    if (!isRecording && (pressed_buttons & ARCADA_BUTTONMASK_A)) {
      // button was pressed!
      isRecording = true;
      // NeoPixel red to indicate recording
      arcada.pixels.fill(arcada.pixels.Color(20, 0, 0));
      arcada.pixels.show();
      // reset audio to beginning of buffer
      recording_length = audio_idx = 0;
      // erase the buffer
      memset(recording_buffer, 0, BUFFER_SIZE * sizeof(int16_t));
      Serial.printf("Recording @ %d...", millis());
    }
    if (isRecording && !(pressed_buttons & ARCADA_BUTTONMASK_A)) {
      isRecording = false;  // definitely done!
      arcada.pixels.clear();
      arcada.pixels.show();
    
      recording_length = audio_idx;
      Serial.printf("Done! Recorded %d samples\n", recording_length);  
      // we have to track when this happened
      audio_timestamp_ms = millis();
    }
  }
}

extern int tflite_micro_main(int argc, char* argv[]);

void setup() {
  arcada.begin();
  arcada.filesysBeginMSD();

  Serial.begin(115200);
  while(!Serial) delay(10);                // Wait for Serial monitor before continuing

  arcada.displayBegin();
  Serial.println("Arcada display begin");
  arcada.fillScreen(ARCADA_BLACK);
  arcada.setBacklight(255);

  recording_buffer = (int16_t *)malloc(BUFFER_SIZE * sizeof(int16_t));
  if (!recording_buffer) {
    Serial.println("Unable to allocate recording buffer");
    while (1);
  }
  memset(recording_buffer, 0, BUFFER_SIZE * sizeof(int16_t));
  recording_length = 0;

  analogWriteResolution(8);
  analogReadResolution(12);

  if (arcada.filesysBegin()) {
    Serial.println("Found filesystem!");
  } else {
    arcada.haltBox("No filesystem found! For QSPI flash, load CircuitPython. For SD cards, format with FAT");
  }


  if (!loadTFConfigFile()) {
    arcada.haltBox("Failed to load TFLite config");
  }
  const char *modelname = TFconfigJSON["model_name"];
  Serial.print("Model name: "); Serial.println(modelname);
  
  const char *tfilename = TFconfigJSON["file_name"];
  Serial.print("File name: "); Serial.println(tfilename);

  File tflite_file = arcada.open(tfilename);
  if (! tflite_file) {
    arcada.haltBox("TFLite model file not found");
  }
  g_tiny_conv_micro_features_model_data_len = tflite_file.fileSize();
  Serial.printf("Found tflite model of size %d\n", g_tiny_conv_micro_features_model_data_len);
  g_tiny_conv_micro_features_model_data = (unsigned char *)malloc(g_tiny_conv_micro_features_model_data_len+15);
  if (! g_tiny_conv_micro_features_model_data) {
    arcada.haltBox("Could not malloc model space");
  } else {
    Serial.printf("Model address = %08x\n", &g_tiny_conv_micro_features_model_data);
  }
  
  ssize_t ret = tflite_file.read(g_tiny_conv_micro_features_model_data, g_tiny_conv_micro_features_model_data_len);
  if (ret != g_tiny_conv_micro_features_model_data_len) {
    arcada.haltBox("Could not load model");
  }
  Serial.println("\nSuccess!");

  JsonArray labelArray = TFconfigJSON["category_labels"].as<JsonArray>();
  Serial.print("Category label count: "); Serial.println(labelArray.size());
  kCategoryCount = labelArray.size();
  kCategoryLabels = (char **)malloc(kCategoryCount * sizeof(char *));
  if (! kCategoryLabels) {
    arcada.haltBox("Failed to allocate category labels array");
  }
  for (int s=0; s<kCategoryCount; s++) {
    const char *label = TFconfigJSON["category_labels"][s];
    kCategoryLabels[s] = (char *)malloc((strlen(label)+1) * sizeof(char));
    if (! kCategoryLabels) {
      arcada.haltBox("Failed to allocate category label");
    }
    memcpy(kCategoryLabels[s], label, strlen(label)+1);
    Serial.printf("Label #%d: ", s); Serial.println(kCategoryLabels[s]);
  }

  for (uint8_t x=0; x<32; x++) {
    Serial.printf("0x%02X, ", g_tiny_conv_micro_features_model_data[x]);
  }

  // get the gif decoder lined up
  initGIFDecode();
  // start our 16khz timer
  isPlaying = isRecording = false;
  arcada.timerCallback(kAudioSampleFrequency, TimerCallback);

  // Play introgif
  playSoundAndGIF("/tron/intro.gif", introAudioData, introSamples);

  // display instruction screen
  arcada.drawBMP("/tron/screen.bmp", 0, 0);

  Serial.println("Waiting for button press A to record...");
  Serial.println("-----------ARCADA TFLITE----------");

  tflite_micro_main(0, NULL);
}

void loop() {

}


bool loadTFConfigFile(const char *filename) {
  // Open file for reading
  File file = arcada.open(filename);
  if (!file) {
    Serial.print("Failed to open file");
    Serial.println(filename);
    return false;
  }

  // Deserialize the JSON document
  DeserializationError error = deserializeJson(TFconfigJSON, file);
  if (error) {
    Serial.println(F("Failed to read file"));
    return false;
  }

  // Close the file (File's destructor doesn't close the file)
  file.close();

  return true;
}


// Our little helper to play a GIF and matching audio file at the same time
void playSoundAndGIF(const char *gifname, const uint8_t *audio, uint32_t audiolen) {
  play_buffer = audio;
  play_length = audiolen;
  audio_idx = 0;
  isPlaying = true;
  arcada.enableSpeaker(true);
  if (!playGIF(gifname)) {
    arcada.haltBox("Couldn't play gif");
  }
  Serial.println("Done playing gif");
  arcada.enableSpeaker(false);
}

// The callback from TFLite with whatever it detected!
void RespondToCommand(tflite::ErrorReporter* error_reporter,
                      int32_t current_time, const char* found_command,
                      uint8_t score, bool is_new_command) {
  if (is_new_command) {
    error_reporter->Report("I heard %s (score %d) @%dms (realtime %d)", found_command, score,
                           current_time, millis());
    if (found_command[0] == 'y') {      // yes!
      playSoundAndGIF("/tron/yes.gif", yes_16k_mono_8bitAudioData, yes_16k_mono_8bitSamples);
      arcada.fillScreen(ARCADA_BLACK);
      arcada.drawBMP("/tron/screen.bmp", 0, 0);
    } else if (found_command[0] == 'n') {
      playSoundAndGIF("/tron/no.gif", no_16k_mono_8bitAudioData, no_16k_mono_8bitSamples);
      arcada.fillScreen(ARCADA_BLACK);
      arcada.drawBMP("/tron/screen.bmp", 0, 0);
    } else {
      arcada.fillScreen(ARCADA_BLACK);
    }
  }
}
