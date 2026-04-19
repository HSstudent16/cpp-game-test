#include <stdexcept>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <jsoncpp/json/json.h>

#include <chrono>
#include <thread>

#include "../lib/glad/gl.h"
#include <GLFW/glfw3.h>

#include <AL/al.h>
#include <AL/alc.h>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "../lib/stb/stb_image.h"

#define MINIMP3_IMPLEMENTATION
#include "../lib/minimp3/minimp3.h"
#include "../lib/minimp3/minimp3_ex.h"

#define CLAMP(a, b, c) a < b ? b : a > c ? c : a
#define MAX(a, b) a > b ? a : b
#define MIN(a, b) a < b ? a : b

GLFWwindow* window;

ALCdevice* audioDevice;
ALCcontext* audioContext;


struct Image {
  std::string src;
  int width;
  int height;
  GLuint texture = 0;
  bool ready = false;
  unsigned char* data;
};

struct AudioData {
  std::vector<short> samples;
  int sampleRate;
  int channels;
};

struct TransformLocations {
  GLint sourcePos;
  GLint sourceSize;
  GLint destPos;
  GLint destSize;
} imageTransform;

struct Player {
  float x = 0, y = 0, vx = 0, vy = 0;
  float width = 18*2, height = 31*2;
  bool onGround = false;

} player;

Image background, playerSprite, blockSprite, overlay;

int width, height;

// List of pressed keys
std::map<int, bool> keys;

// Mouse state
struct MouseState {
  bool pressed = false;
  bool clicked = false;

  double x = 0;
  double y = 0;
  double px = 0;
  double py = 0;

  int button = 0;
} mouse;

// Level struct
struct Level {
  int width;
  int height;
  float playerX;
  float playerY;
  char* data;
  ~Level () {
    delete[] data;
  }
  Level& operator=(const Level& other) {
    if (this == &other) return *this; // self-assignment check
    delete[] data;
    width = other.width;
    height = other.height;
    playerX = other.playerX;
    playerY = other.playerY;
    if (other.data) {
        data = new char[width * height];
        std::copy(other.data, other.data + width * height, data);
    } else {
        data = nullptr;
    }
    return *this;
  }
};

Level currentLevel;

bool loading = true;

/*
  Prints a message to stderr, and (hopefully) halts the program
*/
void onerror (const std::string text) {
  std::cout << text << "\n";
  throw std::runtime_error("Uh-oh!");
}
void errorCallback (int code, const char* text) {
  onerror ("GLFW Error! \n" + std::string(text));
}
void checkALerror (std::string message) {
  ALCenum error = alGetError();
  if (error != AL_NO_ERROR) {
    throw std::runtime_error(message);
  }
}

/*
  Reads a file, and dumps its contents into a char array
*/
std::vector<char> readFile (const std::string source) {
  std::ifstream file{source, std::ios::ate | std::ios::binary};

  if (!file.is_open()) {
    onerror ("Failed to open file " + source);
  }

  // Read file
  int size = file.tellg();
  std::vector<char> contents(size+1);

  file.seekg (0);
  file.read (contents.data(), size);
  file.close ();

  // Null terminate... cuz c strings are dumb
  contents[size] = '\0';

  return contents;
}

/*
  Load an image using stb
*/
Image loadImage (const char* src) {
  Image img;

  int realChannels;
  img.src = src;

  // So convenient :D
  img.data = stbi_load(src, &img.width, &img.height, &realChannels, 4);

  if (img.data == nullptr) {
    throw std::runtime_error("Failed to load image!");
  }

  return img;
};

/*
  Unload an image
*/
void unloadImage (Image *img) {
  glDeleteTextures(1, &img->texture);

  img->ready = false;
}

/*
  Load an mp3 file
*/
AudioData loadMP3 (const char* source) {
  mp3dec_ex_t dec;
  if (mp3dec_ex_open(&dec, source, MP3D_SEEK_TO_SAMPLE)) {
    onerror("Could not open MP3 file");
  }

  AudioData output;
  output.sampleRate = dec.info.hz;
  output.channels = dec.info.channels;

  size_t samples = dec.samples;

  // Set data size
  output.samples.resize(samples);

  size_t pos = mp3dec_ex_read(&dec, output.samples.data(), samples);

  if (pos != samples) {
    onerror("MP3 file is corrupt.");
  }

  mp3dec_ex_close(&dec);
  return output;
}

/*
  Read a level file (JSON) & parse to level struct
*/
Level readLevel (const std::string source) {
  Json::Reader reader;
  std::ifstream file{source};

  Json::Value data;
  reader.parse(file, data);

  file.close();

  Level output;

  output.playerX = data.get("playerX", 0).asInt() * 64.0 + 7.0;
  output.playerY = data.get("playerY", 0).asInt() * 64.0;

  std::cout << "Place player at " << output.playerX << ", " << output.playerY << "\n";
 
  int width = output.width = data.get("width", 0).asInt();
  int height = output.height = data.get("height", 0).asInt();
  output.data = new char[width * height];
  
  Json::Value bitmap = data.get("bitmap", {""});

  if (bitmap.type() != Json::arrayValue) {
    throw std::runtime_error("Failed to parse level! Invalid bitmap structure");
  }

  int rows = bitmap.size();
  // Manually paste level data in case rows are incomplete / don't reflect actual width & height
  for (int i = 0; i < height; i++) {
    Json::Value row = bitmap.get(i, "");
    std::string rowStr = row.asCString();
    int rowLength = rowStr.length();
    for (int j = 0; j < width; j++) {
      output.data[j + i * width] = j >= rowLength ? ' ' : rowStr[j];
    }
  }

  return output;
};

/*
  Draws an `Image` object
*/
void drawImage (Image *img, double x, double y, double w, double h, double sx, double sy, double sw, double sh) {
  if (!img->ready) {
    GLuint texture;

    if (img->data == nullptr) {
      throw std::runtime_error("Failed to draw image! Possibly unloaded?");
    }

    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, img->width, img->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, img->data);
    img->texture = texture;
    img->ready = true;

    // The image has been shipped to the GPU, so it can now be freed safely
    stbi_image_free(img->data);

    img->data = nullptr;
  }

  // Set transform
  glUniform2f(imageTransform.sourcePos, sx / img->width, sy / img->height);
  glUniform2f(imageTransform.sourceSize, sw / img->width, sh / img->height);
  glUniform2f(imageTransform.destPos, 2.0 * x / width - 1.0, 2.0 * y / height - 1.0);
  glUniform2f(imageTransform.destSize, 2.0 * w / width, 2.0 * h / height);

  // Draw image
  glBindTexture(GL_TEXTURE_2D, img->texture);
  glDrawArrays(GL_TRIANGLES, 0, 6);
}
void drawImage (Image *img, double x, double y, double w, double h) {
  drawImage (img, x, y, w, h, 0.0, 0.0, img->width, img->height);
}
void drawImage (Image *img, double x, double y) {
  drawImage (img, x, y, img->width, img->height);
}

/*
  Queues an audio buffer with openal
*/
void queueAudioBuffer (const AudioData& audio, ALuint buffer) {
  ALenum format = (audio.channels == 2) ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16;
  alBufferData(buffer, format, audio.samples.data(), audio.samples.size() * sizeof(short), audio.sampleRate);
}

/*
  Create an OpenGL Shader program from two shader files (vertex & fragment)
*/
GLuint createProgram (const std::string vsDir, const std::string fsDir) {
  std::vector<char> vsSource = readFile(vsDir);
  const char* vsBuffer = vsSource.data();

  const GLuint vsh = glCreateShader(GL_VERTEX_SHADER);

  glShaderSource (vsh, 1, &vsBuffer, NULL);
  glCompileShader (vsh);
  
  int params = -1;
  glGetShaderiv(vsh, GL_COMPILE_STATUS, &params);

  if (params != GL_TRUE) {
    int len = 0;
    char error[2048];

    glGetShaderInfoLog(vsh, 2048, &len, error);
    onerror("Failed to compile vertex shader!\n\n" + std::string(error));
  }


  std::vector<char> fsSource = readFile(fsDir);
  const char* fsBuffer = fsSource.data();

  const GLuint fsh = glCreateShader(GL_FRAGMENT_SHADER);

  glShaderSource (fsh, 1, &fsBuffer, NULL);
  glCompileShader (fsh);

  params = -1;
  glGetShaderiv(vsh, GL_COMPILE_STATUS, &params);

  if (params != GL_TRUE) {
    int len = 0;
    char error[2048];

    glGetShaderInfoLog(fsh, 2048, &len, error);
    onerror("Failed to compile fragment shader!\n\n" + std::string(error));
  }

  GLuint program = glCreateProgram();

  glAttachShader (program, vsh);
  glAttachShader (program, fsh);
  glLinkProgram (program);

  // Remove shaders once the program is built
  glDeleteShader(vsh);
  glDeleteShader(fsh);

  return program;
}

// GLFW input callbacks
void onKeyInput (GLFWwindow* w, int key, int scancode, int action, int mods) {
  if (action == GLFW_PRESS) {
    keys[key] = true;
  } else if (action == GLFW_RELEASE) {
    // Deleting is useless here, since it would get re-created when checking for the same key
    keys[key] = false;
  }
}
void onMouseInput (GLFWwindow *w, int button, int action, int mods) {
  if (action == GLFW_PRESS) {
    mouse.pressed = true;
    mouse.button = button;
  } else if (action == GLFW_RELEASE) {
    mouse.pressed = false;
    mouse.clicked = true;
    mouse.button = button;
  }
}

/*
  This is where I load some images & stuff (probably threaded)
*/
void load () {
  playerSprite = loadImage("./assets/player.png");
  blockSprite = loadImage("./assets/block.png");

  currentLevel = readLevel("./assets/level.json");

  overlay = loadImage("./assets/overlay.png");

  player.x = currentLevel.playerX;
  player.y = currentLevel.playerY;

  loading = false;

  std::cout << "Player got placed at: " << player.x << ", " << player.y << "\n";
  std::cout << "Finished loading!\nLevel size: " << currentLevel.width << " x " << currentLevel.height << std::endl;
}

/*
  This is where things get loaded before the render loop begins
*/
void preload () {
  background = loadImage("./assets/background.png");
}

/*
  Separated for neatness & sanity
*/
void draw () {
  drawImage(&background, 0, 0, width, height);
  if (loading) {
    return;
  }

  // Paint tile map
  for (int j = 0; j < currentLevel.height; j++) {
    for (int i = 0; i < currentLevel.width; i++) {
      int idx = j * currentLevel.width + i;
      char tile = currentLevel.data[idx];

      switch (tile) {
        case ' ':
          break;
        case '#':
          drawImage(&blockSprite, i * 64, j * 64, 64, 64);
          break;
      }
    }
  }

  // X movement
  if (keys[GLFW_KEY_A]) {
    player.vx -= 1;
  }
  if (keys[GLFW_KEY_D]) {
    player.vx += 1;
  }
  player.vx *= 0.9;
  player.x += player.vx;

  // X collision

  int startX = MAX(0, floor(player.x / 64.0)),
      startY = MAX(0, floor(player.y / 64.0)),
      stopX  = MIN(currentLevel.width, ceil((player.x + player.width) / 64.0)),
      stopY  = MIN(currentLevel.height, ceil((player.y + player.height) / 64.0));
  
  int i, j, idx;
  float tileX, tileY;

  char tile;

  for (j = startY; j < stopY; j++) {
    tileY = j * 64.0;
    for (i = startX; i < stopX; i++) {
      idx = j * currentLevel.width + i;
      tile = currentLevel.data[idx];

      tileX = i * 64.0;

      // Only care about blocks
      if (tile != '#') continue;

      // Ignore blocks that aren't being hit
      if (!(
        player.x + player.width > tileX &&
        player.y + player.height > tileY &&
        player.x < tileX + 64 &&
        player.y < tileY + 64
      )) continue;

      if (player.vx > 0) {
        player.x = tileX - player.width;
      } else {
        player.x = tileX + 64;
      }
      player.vx = 0;
    }
  }

  // Y movement
  if (keys[GLFW_KEY_W] && player.onGround) {
    player.onGround = false;
    player.vy -= 16.0;
  }

  player.vy += 0.3;
  player.vy *= 0.99;

  player.y += player.vy;

  // Y Collision
  player.onGround = false;

  startX = MAX(0, floor(player.x / 64.0));
  startY = MAX(0, floor(player.y / 64.0));
  stopX  = MIN(currentLevel.width, ceil((player.x + player.width) / 64.0));
  stopY  = MIN(currentLevel.height, ceil((player.y + player.height) / 64.0));

  for (j = startY; j < stopY; j++) {
    tileY = j * 64.0;
    for (i = startX; i < stopX; i++) {
      idx = j * currentLevel.width + i;
      tile = currentLevel.data[idx];

      tileX = i * 64.0;

      // Only care about blocks
      if (tile != '#') continue;

      // Ignore blocks that aren't being hit
      if (!(
        player.x + player.width > tileX &&
        player.y + player.height > tileY &&
        player.x < tileX + 64 &&
        player.y < tileY + 64
      )) continue;

      drawImage(&overlay, tileX, tileY, 64, 64);

      if (player.vy > 0) {
        player.y = tileY - player.height;
        player.onGround = true;
      } else {
        player.y = tileY + 64;
      }
      player.vy = 0;
    }
  }

  

  // paint player sprite
  drawImage(&playerSprite, player.x - 14.0, player.y - 2.0, 64, 64);
}

/*
  The final part of the program
*/
void unloadAssets () {
  unloadImage(&background);
  unloadImage(&blockSprite);
  unloadImage(&playerSprite);

  // Stop audio context
  audioDevice = alcGetContextsDevice(audioContext);

  alcMakeContextCurrent(NULL);
  alcDestroyContext(audioContext);
  alcCloseDevice(audioDevice);
}

/*
  This is the main draw loop (also probably threaded)
*/
void render () {
  // Background audio
  ALuint loopBuffer;
  ALuint audioSource;

  // Create audio source node
  alGenSources(1, &audioSource);
  alSourcef(audioSource, AL_PITCH, 1);
  alSourcef(audioSource, AL_GAIN, 0.5);
  alSource3f(audioSource, AL_POSITION, 0, 0, 0);
  alSource3f(audioSource, AL_VELOCITY, 0, 0, 0);
  alSourcei(audioSource, AL_LOOPING, AL_TRUE);

  alGenBuffers(1, &loopBuffer);
  
  AudioData loopSource = loadMP3("./assets/loop.mp3");
  queueAudioBuffer(loopSource, loopBuffer);

  // Actually play it
  alSourcei(audioSource, AL_BUFFER, loopBuffer);
  alSourcePlay(audioSource);

  // Main loop
  while (!glfwWindowShouldClose(window)) {
    

    mouse.px = mouse.x;
    mouse.py = mouse.y;

    glfwGetWindowSize(window, &width, &height);
    glfwGetCursorPos(window, &mouse.x, &mouse.y);

    mouse.x = CLAMP(mouse.x, 0, width);
    mouse.y = CLAMP(mouse.y, 0, height);    

    // Draw stuff
    glViewport(0, 0, width, height);
    glClearColor(1., 0., 0., 1.);
    glClear(GL_COLOR_BUFFER_BIT);

    draw ();

    mouse.clicked = false;

    glfwSwapBuffers (window);

    // Poll events put here to prevent segfault on Wayland
    glfwPollEvents ();
  }

  // Unload loop buffer & source
  alSourceStop(audioSource);
  alDeleteSources(1, &audioSource);
  alDeleteBuffers(1, &loopBuffer);
  
}

int main () {

  glfwSetErrorCallback (errorCallback);
  glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_WAYLAND);

  if (!glfwInit ()) {
    onerror("Failed to load GLFW!");
    return 1;
  }

  int vMaj = 0;
  int vMin = 0;
  int vRev = 0;
  glfwGetVersion(&vMaj, &vMin, &vRev);
  std::cout << "Using GLFW version " << vMaj << "." << vMin << "." << vRev << "\n";
  std::cout << "Using platform " << (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND ? "Wayland" : "X11") << "\n";


  // Don't really care about the version; latest should be fine
  window = glfwCreateWindow(800, 800, "Quick Game", NULL, NULL);

  if (!window) {
    onerror ("Failed to create window!");
    glfwTerminate ();
    return 1;
  }

  // Very important line right here
  glfwMakeContextCurrent(window);

  // Add the callbacks
  glfwSetKeyCallback(window, onKeyInput);
  glfwSetMouseButtonCallback(window, onMouseInput);

  // Load GL functions
  int gladVersion = gladLoadGL(glfwGetProcAddress);

  if (gladVersion == 0) {
    onerror ("Could not load GLAD!");
    glfwTerminate ();
    return 1;
  }  

  // Do some shader initialization & image loading here
  GLuint mainProgram = createProgram("./assets/main.vsh", "./assets/main.fsh");

  glUseProgram(mainProgram);

  // Initialize the audio device
  audioDevice = alcOpenDevice(NULL);
  if (!audioDevice) {
    onerror("Could not find an audio output device!");
  }

  audioContext = alcCreateContext(audioDevice, NULL);
  if (!alcMakeContextCurrent(audioContext)) {
    onerror("Audio context did not work :(");
  }
  checkALerror("Something went wrong!");

  // Set default audio listener
  ALfloat audioOrientation[] = {0, 0, 1, 0, 1, 0};

  alListener3f(AL_POSITION, 0, 0, 1);
  alListener3f(AL_VELOCITY, 0, 0, 0);
  alListenerfv(AL_ORIENTATION, audioOrientation);

  checkALerror("Something went wrong while configuring audio listener!");
 
  

  // Create the sprite vertex buffer object
  float rect[] = {
    0.0, 0.0, 0.0,
    1.0, 0.0, 0.0,
    0.0, 1.0, 0.0,
    
    1.0, 0.0, 0.0,
    1.0, 1.0, 0.0,
    0.0, 1.0, 0.0
  };
  GLuint vbo = 0;
  glGenBuffers(1, &vbo);
  glBindBuffer (GL_ARRAY_BUFFER, vbo);
  glBufferData (GL_ARRAY_BUFFER, 18 * sizeof(float), rect, GL_STATIC_DRAW);

  // Create vertex array object to send vertex buffer object to gpu
  GLuint vao = 0;
  glGenVertexArrays(1, &vao);
  glBindVertexArray(vao);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, NULL);

  // Find & cache locations for image transform
  imageTransform.sourcePos = glGetUniformLocation(mainProgram, "sourcePos");
  imageTransform.sourceSize = glGetUniformLocation(mainProgram, "sourceSize");
  imageTransform.destPos = glGetUniformLocation(mainProgram, "destPos");
  imageTransform.destSize = glGetUniformLocation(mainProgram, "destSize");

  // Set up the load thread & render thread  
  std::thread loadThread(load);

  // Load a default image
  preload();
  
  // Render loop in main thread, for "stability"
  render();

  // Force loader to quit before program ends
  loadThread.join();

  // Free up memory once program finishes
  unloadAssets();
  glDeleteBuffers(1, &vbo);
  glDeleteProgram(mainProgram);

  // Program closes successfully
  glfwTerminate ();
  return 0;
};
