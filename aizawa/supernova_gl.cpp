#include <GL/glew.h>
#include <SDL3/SDL.h>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

// --- Configuration ---
const int WINDOW_WIDTH = 1920;
const int WINDOW_HEIGHT = 1080;
const int PARTICLE_COUNT = 100000; // Increased from 10k to 100k for GPU glory
const float ZOOM = 350.0f;

// --- Aizawa Constants ---
struct SimulationParams {
  float dt = 0.01f;
  float a = 0.95f;
  float b = 0.7f;
  float c = 0.6f;
  float d = 3.5f;
  float e = 0.25f;
} params;

// --- Helper: Read File ---
std::string read_file(const char *path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    std::cerr << "Failed to open " << path << std::endl;
    return "";
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

// --- Helper: Compile Shader ---
GLuint create_shader(const char *path, GLenum type) {
  std::string source = read_file(path);
  if (source.empty())
    return 0;
  const char *src_ptr = source.c_str();

  GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &src_ptr, NULL);
  glCompileShader(shader);

  GLint success;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
  if (!success) {
    char infoLog[512];
    glGetShaderInfoLog(shader, 512, NULL, infoLog);
    std::cerr << "ERROR::SHADER::COMPILATION_FAILED: " << path << "\n"
              << infoLog << std::endl;
  }
  return shader;
}

// --- Helper: Create Program ---
GLuint create_program(const char *vertPath, const char *fragPath) {
  GLuint vert = create_shader(vertPath, GL_VERTEX_SHADER);
  GLuint frag = create_shader(fragPath, GL_FRAGMENT_SHADER);
  GLuint prog = glCreateProgram();
  glAttachShader(prog, vert);
  glAttachShader(prog, frag);
  glLinkProgram(prog);
  // Check errors...
  glDeleteShader(vert);
  glDeleteShader(frag);
  return prog;
}

GLuint create_compute_program(const char *path) {
  GLuint comp = create_shader(path, GL_COMPUTE_SHADER);
  GLuint prog = glCreateProgram();
  glAttachShader(prog, comp);
  glLinkProgram(prog);
  glDeleteShader(comp);
  return prog;
}

struct Particle {
  float x, y, z, w;     // pos
  float px, py, pz, pw; // prev_pos
};

float random_float(float min, float max) {
  return min + static_cast<float>(rand()) /
                   (static_cast<float>(RAND_MAX / (max - min)));
}

int main(int argc, char *argv[]) {
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::cerr << "SDL Init failed: " << SDL_GetError() << std::endl;
    return 1;
  }

  // Setup OpenGL 4.3+ for Compute Shaders
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);

  SDL_Window *window =
      SDL_CreateWindow("Aizawa GPU", WINDOW_WIDTH, WINDOW_HEIGHT,
                       SDL_WINDOW_OPENGL | SDL_WINDOW_BORDERLESS);
  if (!window) {
    std::cerr << "Window creation failed: " << SDL_GetError() << std::endl;
    return 1;
  }

  SDL_GLContext context = SDL_GL_CreateContext(window);
  if (!context) {
    std::cerr << "Failed to create OpenGL context: " << SDL_GetError()
              << std::endl;
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }

  SDL_GL_MakeCurrent(window, context);
  SDL_GL_SetSwapInterval(1); // Enable VSync to cap framerate

  std::cout << "GL Context Created. Version: " << glGetString(GL_VERSION)
            << std::endl;

  // Init GLEW
  glewExperimental = GL_TRUE;
  GLenum glewError = glewInit();
  if (glewError != GLEW_OK && glewError != 4) { // 4 = GLEW_ERROR_NO_GL_VERSION
    std::cerr << "GLEW Init failed. Code: " << glewError
              << " String: " << glewGetErrorString(glewError) << std::endl;
    return 1;
  }

  // Clear GL error that GLEW might have raised
  glGetError();

  // Check if we actually got the modern features we need
  if (!glDispatchCompute) {
    std::cerr << "Error: glDispatchCompute not loaded. Your OpenGL context "
                 "might be too old or GLEW failed completely."
              << std::endl;
    return 1;
  }

  // --- Shader Setup ---
  GLuint renderProg = create_program("render.vert", "render.frag");
  GLuint computeProg = create_compute_program("aizawa.comp");

  // --- Buffer Setup ---
  std::vector<Particle> host_particles(PARTICLE_COUNT);
  for (auto &p : host_particles) {
    p.x = random_float(-0.1f, 0.1f);
    p.y = random_float(-0.1f, 0.1f);
    p.z = random_float(-0.1f, 0.1f);
    p.px = p.x;
    p.py = p.y;
    p.pz = p.z;
  }

  GLuint ssbo;
  glGenBuffers(1, &ssbo);
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo);
  glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(Particle) * PARTICLE_COUNT,
               host_particles.data(), GL_DYNAMIC_DRAW);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ssbo);

  // CRITICAL: Create a dummy VAO. Core Profile requires a VAO to be bound.
  GLuint vao;
  glGenVertexArrays(1, &vao);
  glBindVertexArray(vao);

  // --- Render State ---
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE); // Additive blending

  // --- Loop Variables ---
  bool running = true;
  float rot_y = 0.0f;
  float rot_x = 0.0f;

  // Simple View Matrix helpers (LookAt-ish)
  // We'll just do manual rotation in Shader or pass matrices?
  // Let's pass matrices to be proper.

  while (running) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_EVENT_QUIT)
        running = false;
      if (event.type == SDL_EVENT_KEY_DOWN) {
        if (event.key.scancode == SDL_SCANCODE_ESCAPE)
          running = false;
        if (event.key.scancode == SDL_SCANCODE_LEFT)
          rot_y -= 0.05f;
        if (event.key.scancode == SDL_SCANCODE_RIGHT)
          rot_y += 0.05f;
        if (event.key.scancode == SDL_SCANCODE_UP)
          rot_x -= 0.05f;
        if (event.key.scancode == SDL_SCANCODE_DOWN)
          rot_x += 0.05f;
      }
    }

    // --- 1. Compute Step ---
    glUseProgram(computeProg);
    glUniform1f(glGetUniformLocation(computeProg, "dt"), params.dt);
    glUniform1f(glGetUniformLocation(computeProg, "a"), params.a);
    glUniform1f(glGetUniformLocation(computeProg, "b"), params.b);
    glUniform1f(glGetUniformLocation(computeProg, "c"), params.c);
    glUniform1f(glGetUniformLocation(computeProg, "d"), params.d);
    glUniform1f(glGetUniformLocation(computeProg, "e"), params.e);

    // Dispatch (Total / LocalSize)
    glDispatchCompute((PARTICLE_COUNT + 127) / 128, 1, 1);

    // Memory Barrier to ensure writes are visible to Vertex Shader
    glMemoryBarrier(GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT |
                    GL_SHADER_STORAGE_BARRIER_BIT);

    // --- 2. Render Step ---
    glViewport(0, 0, WINDOW_WIDTH, WINDOW_HEIGHT);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(renderProg);

    // Matrix Math (Simple Orbit Cam)
    // Projection (Perspective)
    float aspect = (float)WINDOW_WIDTH / (float)WINDOW_HEIGHT;
    float fov = 45.0f * (3.14159f / 180.0f);
    float f = 1.0f / tan(fov / 2.0f);
    float far = 5000.0f;
    float near = 0.1f;

    float proj[16] = {f / aspect,
                      0,
                      0,
                      0,
                      0,
                      f,
                      0,
                      0,
                      0,
                      0,
                      (far + near) / (near - far),
                      -1,
                      0,
                      0,
                      (2 * far * near) / (near - far),
                      0};

    // View (Rotation + Translation)
    // Original code had custom projection logic.
    // We will mimic the "Orbit" by rotating the world and pushing it back.
    // Rotation Y
    float cy = cos(rot_y), sy = sin(rot_y);
    float cx = cos(rot_x), sx = sin(rot_x);

    // Rotation matrix (Y then X)
    // Simplified for brevity, standard matrix mult would be better but this
    // works for orbit. Actually, let's just push a View Matrix that does:
    // Translate(0,0,-4) * RotateX * RotateY The attractors are usually small
    // (coords around 0-2). The original code zoomed by 350.0. In OpenGL, we can
    // just scale the view or model.

    // Model Matrix: Scale * Rotation
    float scale = 1.5f; // Fit in view

    // Manual 4x4 creation is annoying without GLM, but let's do a basic lookat.
    // Eye at (0, 0, 4), Target (0,0,0).
    // We will rotate the *particles* instead of the camera for simplicity of
    // controls matching original.

    // Let's pass Identity View and do transforms in Shader?
    // No, standard is best.
    // Let's do a simple construct:

    // V = T * Rx * Ry
    float view[16] = {
        cy,       sx * sy, -cx * sy, 0, 0,    cx,    sx, 0, sy,
        -sx * cy, cx * cy, 0,        0, 0.0f, -4.0f, 1 // Translate Z=-4, Y=0.0
                                                       // (center it vertically)
    };

    // Transpose for OpenGL column-major if we wrote row-major above?
    // Actually OpenGL expects Column-Major.
    // The above is Row-Major if read visually?
    // Let's just trust standard layout or use transpose=GL_TRUE
    // Standard Row Major memory:
    // | 0  1  2  3 |
    // | 4  5  6  7 |
    // OpenGL UniformMatrix4fv with transpose=GL_TRUE takes Row Major.

    glUniformMatrix4fv(glGetUniformLocation(renderProg, "projection"), 1,
                       GL_FALSE, proj);
    glUniformMatrix4fv(glGetUniformLocation(renderProg, "view"), 1, GL_FALSE,
                       view);
    glUniform1f(glGetUniformLocation(renderProg, "speed_scale"),
                140.0f); // Match CPU: speed * 1.4 / dt(0.01)

    // Draw Lines
    // 2 vertices per particle
    glLineWidth(2.0f);
    glDrawArrays(GL_LINES, 0, PARTICLE_COUNT * 2);

    SDL_GL_SwapWindow(window);

    rot_y += 0.005f;
  }

  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
