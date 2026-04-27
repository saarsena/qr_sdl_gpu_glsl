#include <SDL3/SDL.h>
#include <errno.h>
#include <spawn.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>

#include "stb_image.h"
#include "stb_image_write.h"
#include "qrcodegen.h"

extern char **environ;

#define WINDOW_TITLE "SDL3 GPU Template"
#define WINDOW_WIDTH 800
#define WINDOW_HEIGHT 800
#define DEFAULT_IMAGE_PATH "assets/SolidarityScarfmaybedone.png"
#define QR_QUIET_ZONE 4

static SDL_GPUShader *load_shader(SDL_GPUDevice *device,
                                  const char *spv_path,
                                  SDL_GPUShaderStage stage,
                                  Uint32 num_samplers,
                                  Uint32 num_uniform_buffers) {
    size_t code_size = 0;
    void *code = SDL_LoadFile(spv_path, &code_size);
    if (!code) {
        SDL_Log("Failed to load %s: %s", spv_path, SDL_GetError());
        return NULL;
    }

    SDL_GPUShaderCreateInfo info = {
        .code_size = code_size,
        .code = (const Uint8 *)code,
        .entrypoint = "main",
        .format = SDL_GPU_SHADERFORMAT_SPIRV,
        .stage = stage,
        .num_samplers = num_samplers,
        .num_uniform_buffers = num_uniform_buffers,
    };

    SDL_GPUShader *shader = SDL_CreateGPUShader(device, &info);
    SDL_free(code);
    if (!shader) {
        SDL_Log("SDL_CreateGPUShader failed for %s: %s", spv_path, SDL_GetError());
    }
    return shader;
}

static bool file_mtime(const char *path, time_t *out) {
    struct stat st;
    if (stat(path, &st) != 0) return false;
    *out = st.st_mtime;
    return true;
}

// Compile `src_path` to SPIR-V at `spv_out_path`, optionally with a single
// preprocessor define passed as e.g. "DOUBLE_BUFFER_0=1" (NULL = no define).
static bool compile_shader_glslc(const char *src_path, const char *spv_out_path,
                                 const char *define) {
    char define_buf[256];
    char *argv_glslc[8];
    int argc = 0;
    argv_glslc[argc++] = "glslc";
    if (define && *define) {
        SDL_snprintf(define_buf, sizeof(define_buf), "-D%s", define);
        argv_glslc[argc++] = define_buf;
    }
    argv_glslc[argc++] = (char *)src_path;
    argv_glslc[argc++] = "-o";
    argv_glslc[argc++] = (char *)spv_out_path;
    argv_glslc[argc] = NULL;

    pid_t pid;
    int rc = posix_spawnp(&pid, "glslc", NULL, NULL, argv_glslc, environ);
    if (rc != 0) {
        SDL_Log("posix_spawnp(glslc) failed: %s", strerror(rc));
        return false;
    }
    int status;
    if (waitpid(pid, &status, 0) < 0) {
        SDL_Log("waitpid failed: %s", strerror(errno));
        return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static SDL_GPUGraphicsPipeline *build_pipeline(SDL_GPUDevice *device,
                                               SDL_GPUTextureFormat color_format,
                                               const char *vert_spv_path,
                                               const char *frag_spv_path) {
    SDL_GPUShader *vert = load_shader(device, vert_spv_path,
                                      SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
    SDL_GPUShader *frag = load_shader(device, frag_spv_path,
                                      SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
    if (!vert || !frag) {
        if (vert) SDL_ReleaseGPUShader(device, vert);
        if (frag) SDL_ReleaseGPUShader(device, frag);
        return NULL;
    }

    SDL_GPUColorTargetDescription color_target_desc = {
        .format = color_format,
    };

    SDL_GPUGraphicsPipelineCreateInfo pipeline_info = {
        .vertex_shader = vert,
        .fragment_shader = frag,
        .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
        .target_info = {
            .color_target_descriptions = &color_target_desc,
            .num_color_targets = 1,
        },
    };

    SDL_GPUGraphicsPipeline *pipeline =
        SDL_CreateGPUGraphicsPipeline(device, &pipeline_info);
    SDL_ReleaseGPUShader(device, vert);
    SDL_ReleaseGPUShader(device, frag);

    if (!pipeline) {
        SDL_Log("SDL_CreateGPUGraphicsPipeline failed: %s", SDL_GetError());
    }
    return pipeline;
}

// Resolve the absolute path to a shader source file. Tries in-tree dev location
// (<basepath>../shaders/<name>) first, then deployed (<basepath>shaders/<name>).
// Returns true if a path was filled in (even if the file doesn't exist at the
// deployed location — the caller will discover that on first use).
static bool resolve_source_path(const char *base_path, const char *name,
                                char *out, size_t out_sz) {
    struct stat st;
    SDL_snprintf(out, out_sz, "%s../shaders/%s", base_path, name);
    if (stat(out, &st) == 0) return true;
    SDL_snprintf(out, out_sz, "%sshaders/%s", base_path, name);
    return stat(out, &st) == 0;
}

// Decode a PNG/JPEG via stb_image and upload it to a GPU sampler texture as
// RGBA8. Returns the texture (or NULL on failure) and writes the image
// dimensions to *out_w / *out_h.
// Encode `text` as a QR via qrcodegen and upload it as an R8 sampler texture
// (1 byte per module: 0 = light, 0xFF = dark) with QR_QUIET_ZONE pixels of
// border on each side. Returns NULL on failure.
static SDL_GPUTexture *create_qr_texture_from_text(SDL_GPUDevice *device,
                                                   const char *text) {
    uint8_t *scratch = (uint8_t *)SDL_malloc(qrcodegen_BUFFER_LEN_MAX);
    uint8_t *qrcode  = (uint8_t *)SDL_malloc(qrcodegen_BUFFER_LEN_MAX);
    if (!scratch || !qrcode) {
        SDL_free(scratch);
        SDL_free(qrcode);
        return NULL;
    }

    bool ok = qrcodegen_encodeText(text, scratch, qrcode,
                                   qrcodegen_Ecc_LOW,
                                   qrcodegen_VERSION_MIN,
                                   qrcodegen_VERSION_MAX,
                                   qrcodegen_Mask_AUTO, true);
    SDL_free(scratch);
    if (!ok) {
        SDL_Log("qrcodegen_encodeText failed (text too long for v40 ECC L?)");
        SDL_free(qrcode);
        return NULL;
    }

    int qr_size  = qrcodegen_getSize(qrcode);
    int tex_size = qr_size + 2 * QR_QUIET_ZONE;
    size_t bytes = (size_t)tex_size * (size_t)tex_size;

    SDL_Log("QR: \"%.40s%s\" (%zu chars) → version %d (%dx%d modules, %dx%d tex)",
            text, SDL_strlen(text) > 40 ? "…" : "",
            SDL_strlen(text), (qr_size - 17) / 4, qr_size, qr_size,
            tex_size, tex_size);

    uint8_t *pixels = (uint8_t *)SDL_calloc(1, bytes);
    if (!pixels) {
        SDL_free(qrcode);
        return NULL;
    }
    for (int y = 0; y < qr_size; y++) {
        for (int x = 0; x < qr_size; x++) {
            if (qrcodegen_getModule(qrcode, x, y)) {
                pixels[(QR_QUIET_ZONE + y) * tex_size + (QR_QUIET_ZONE + x)] =
                    0xFF;
            }
        }
    }
    SDL_free(qrcode);

    SDL_GPUTextureCreateInfo tex_info = {
        .type = SDL_GPU_TEXTURETYPE_2D,
        .format = SDL_GPU_TEXTUREFORMAT_R8_UNORM,
        .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER,
        .width = (Uint32)tex_size,
        .height = (Uint32)tex_size,
        .layer_count_or_depth = 1,
        .num_levels = 1,
        .sample_count = SDL_GPU_SAMPLECOUNT_1,
    };
    SDL_GPUTexture *tex = SDL_CreateGPUTexture(device, &tex_info);
    if (!tex) {
        SDL_Log("SDL_CreateGPUTexture failed: %s", SDL_GetError());
        SDL_free(pixels);
        return NULL;
    }

    SDL_GPUTransferBufferCreateInfo tbuf_info = {
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
        .size = (Uint32)bytes,
    };
    SDL_GPUTransferBuffer *tbuf =
        SDL_CreateGPUTransferBuffer(device, &tbuf_info);

    void *map = SDL_MapGPUTransferBuffer(device, tbuf, false);
    SDL_memcpy(map, pixels, bytes);
    SDL_UnmapGPUTransferBuffer(device, tbuf);
    SDL_free(pixels);

    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);

    SDL_GPUTextureTransferInfo src = {
        .transfer_buffer = tbuf,
        .pixels_per_row = (Uint32)tex_size,
        .rows_per_layer = (Uint32)tex_size,
    };
    SDL_GPUTextureRegion dst = {
        .texture = tex,
        .w = (Uint32)tex_size,
        .h = (Uint32)tex_size,
        .d = 1,
    };
    SDL_UploadToGPUTexture(copy, &src, &dst, false);

    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(device, tbuf);

    return tex;
}

static SDL_GPUTexture *load_image_texture(SDL_GPUDevice *device,
                                          const char *path,
                                          int *out_w, int *out_h) {
    int w = 0, h = 0, channels = 0;
    stbi_uc *pixels = stbi_load(path, &w, &h, &channels, 4);
    if (!pixels) {
        SDL_Log("stbi_load(%s) failed: %s", path, stbi_failure_reason());
        return NULL;
    }
    SDL_Log("Image: %s — %dx%d (%d channels)", path, w, h, channels);

    size_t bytes = (size_t)w * (size_t)h * 4;

    SDL_GPUTextureCreateInfo tex_info = {
        .type = SDL_GPU_TEXTURETYPE_2D,
        .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
        .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER,
        .width = (Uint32)w,
        .height = (Uint32)h,
        .layer_count_or_depth = 1,
        .num_levels = 1,
        .sample_count = SDL_GPU_SAMPLECOUNT_1,
    };
    SDL_GPUTexture *tex = SDL_CreateGPUTexture(device, &tex_info);
    if (!tex) {
        SDL_Log("SDL_CreateGPUTexture failed: %s", SDL_GetError());
        stbi_image_free(pixels);
        return NULL;
    }

    SDL_GPUTransferBufferCreateInfo tbuf_info = {
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
        .size = (Uint32)bytes,
    };
    SDL_GPUTransferBuffer *tbuf =
        SDL_CreateGPUTransferBuffer(device, &tbuf_info);

    void *map = SDL_MapGPUTransferBuffer(device, tbuf, false);
    SDL_memcpy(map, pixels, bytes);
    SDL_UnmapGPUTransferBuffer(device, tbuf);
    stbi_image_free(pixels);

    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);

    SDL_GPUTextureTransferInfo src = {
        .transfer_buffer = tbuf,
        .pixels_per_row = (Uint32)w,
        .rows_per_layer = (Uint32)h,
    };
    SDL_GPUTextureRegion dst = {
        .texture = tex,
        .w = (Uint32)w,
        .h = (Uint32)h,
        .d = 1,
    };
    SDL_UploadToGPUTexture(copy, &src, &dst, false);

    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(device, tbuf);

    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return tex;
}

int main(int argc, char *argv[]) {
    const char *frag_arg = NULL;
    const char *record_dir = NULL;
    const char *qr_text = NULL;
    float record_duration = 3.0f;
    int record_fps = 24;
    float loop_period = 0.0f;  // 0 = unset; resolved below.
    float start_time = 0.0f;
    bool feedback_mode = false;
    int rec_w = WINDOW_WIDTH, rec_h = WINDOW_HEIGHT;
    for (int i = 1; i < argc; i++) {
        if (SDL_strcmp(argv[i], "--record") == 0 && i + 1 < argc) {
            record_dir = argv[++i];
        } else if (SDL_strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            record_duration = (float)SDL_atof(argv[++i]);
        } else if (SDL_strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
            record_fps = SDL_atoi(argv[++i]);
        } else if (SDL_strcmp(argv[i], "--loop") == 0 && i + 1 < argc) {
            loop_period = (float)SDL_atof(argv[++i]);
        } else if (SDL_strcmp(argv[i], "--qr") == 0 && i + 1 < argc) {
            qr_text = argv[++i];
        } else if (SDL_strcmp(argv[i], "--start-time") == 0 && i + 1 < argc) {
            start_time = (float)SDL_atof(argv[++i]);
        } else if (SDL_strcmp(argv[i], "--feedback") == 0) {
            feedback_mode = true;
        } else if (SDL_strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            const char *s = argv[++i];
            int w = 0, h = 0;
            if (SDL_sscanf(s, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
                rec_w = w; rec_h = h;
            } else {
                SDL_Log("--size expects WxH (e.g. 600x300)");
                return 1;
            }
        } else if (argv[i][0] != '-') {
            frag_arg = argv[i];
        } else {
            SDL_Log("Unknown arg: %s", argv[i]);
            return 1;
        }
    }
    bool record_mode = (record_dir != NULL);
    // Default loop period: in record mode, match the gif duration exactly so the
    // gif loops cleanly. In interactive mode, pick something contemplative.
    if (loop_period <= 0.0f) {
        loop_period = record_mode ? record_duration : 6.0f;
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    SDL_Window *window = SDL_CreateWindow(WINDOW_TITLE, WINDOW_WIDTH,
                                          WINDOW_HEIGHT, SDL_WINDOW_RESIZABLE);
    if (!window) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_GPUDevice *device =
        SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, true, NULL);
    if (!device) {
        SDL_Log("GPU debug mode unavailable (%s) — retrying without it.",
                SDL_GetError());
        device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, NULL);
    }
    if (!device) {
        SDL_Log("SDL_CreateGPUDevice failed: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    if (!SDL_ClaimWindowForGPUDevice(device, window)) {
        SDL_Log("SDL_ClaimWindowForGPUDevice failed: %s", SDL_GetError());
        SDL_DestroyGPUDevice(device);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    const char *base_path = SDL_GetBasePath();
    if (!base_path) {
        SDL_Log("SDL_GetBasePath failed: %s", SDL_GetError());
        return 1;
    }

    // Source paths drive hot reload. The vertex shader is always the built-in
    // triangle.vert; the fragment shader is either the CLI arg or triangle.frag.
    // Loaded fragment shaders must declare the same bindings as triangle.frag:
    // 1 sampler (set 2 binding 0 = u_qr) and 1 UBO (set 3 binding 0 = vec2 res,
    // float time). They may be declared but unused.
    char vert_src_path[1024] = {0};
    char frag_src_path[1024] = {0};
    resolve_source_path(base_path, "triangle.vert", vert_src_path,
                        sizeof(vert_src_path));
    if (frag_arg) {
        SDL_strlcpy(frag_src_path, frag_arg, sizeof(frag_src_path));
    } else {
        const char *default_frag = feedback_mode ? "wolframs.frag"
                                 : qr_text       ? "triangle.frag"
                                                 : "scarf.frag";
        resolve_source_path(base_path, default_frag, frag_src_path,
                            sizeof(frag_src_path));
    }

    // Hot-reload writes here on every successful recompile.
    char vert_spv_tmp[1024], frag_spv_tmp[1024];
    SDL_snprintf(vert_spv_tmp, sizeof(vert_spv_tmp),
                 "%sshaders/_hot.vert.spv", base_path);
    SDL_snprintf(frag_spv_tmp, sizeof(frag_spv_tmp),
                 "%sshaders/_hot.frag.spv", base_path);

    // Initial .spv: prebuilt by CMake, except a CLI shader needs first compile.
    char vert_spv_initial[1024], frag_spv_initial[1024];
    SDL_snprintf(vert_spv_initial, sizeof(vert_spv_initial),
                 "%sshaders/triangle.vert.spv", base_path);
    if (frag_arg) {
        if (!compile_shader_glslc(frag_arg, frag_spv_tmp, NULL)) {
            SDL_Log("Initial compile of %s failed.", frag_arg);
            return 1;
        }
        SDL_strlcpy(frag_spv_initial, frag_spv_tmp, sizeof(frag_spv_initial));
    } else {
        const char *default_spv = feedback_mode ? "wolframs.frag.spv"
                                : qr_text       ? "triangle.frag.spv"
                                                : "scarf.frag.spv";
        SDL_snprintf(frag_spv_initial, sizeof(frag_spv_initial),
                     "%sshaders/%s", base_path, default_spv);
    }

    SDL_GPUTextureFormat live_format = SDL_GetGPUSwapchainTextureFormat(device, window);
    SDL_GPUGraphicsPipeline *pipeline =
        build_pipeline(device, live_format, vert_spv_initial, frag_spv_initial);
    if (!pipeline) {
        return 1;
    }

    // ---- Feedback (ping-pong) infrastructure ----
    // Compute pipeline writes to PP_FMT (R8G8B8A8) so the display pass can
    // sample the back buffer regardless of the swapchain's actual format.
    const SDL_GPUTextureFormat PP_FMT = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    SDL_GPUGraphicsPipeline *compute_pipeline = NULL;
    SDL_GPUTexture *ping_pong[2] = {NULL, NULL};
    int pp_w = feedback_mode && record_mode ? rec_w : WINDOW_WIDTH;
    int pp_h = feedback_mode && record_mode ? rec_h : WINDOW_HEIGHT;

    if (feedback_mode) {
        // Resolve the compute-variant .spv path.
        char compute_spv_initial[1024];
        if (frag_arg) {
            char compute_spv_tmp[1024];
            SDL_snprintf(compute_spv_tmp, sizeof(compute_spv_tmp),
                         "%sshaders/_hot.compute.spv", base_path);
            if (!compile_shader_glslc(frag_arg, compute_spv_tmp,
                                      "DOUBLE_BUFFER_0=1")) {
                SDL_Log("Initial compute compile of %s failed.", frag_arg);
                return 1;
            }
            SDL_strlcpy(compute_spv_initial, compute_spv_tmp,
                        sizeof(compute_spv_initial));
        } else {
            // Default: replace ".frag.spv" with ".frag.compute.spv" in the
            // already-resolved frag_spv_initial.
            SDL_strlcpy(compute_spv_initial, frag_spv_initial,
                        sizeof(compute_spv_initial));
            size_t len = SDL_strlen(compute_spv_initial);
            const char ext[] = ".spv";
            if (len > 4 && SDL_strcmp(compute_spv_initial + len - 4, ext) == 0) {
                SDL_snprintf(compute_spv_initial + len - 4,
                             sizeof(compute_spv_initial) - (len - 4),
                             ".compute.spv");
            }
        }

        compute_pipeline = build_pipeline(device, PP_FMT, vert_spv_initial,
                                          compute_spv_initial);
        if (!compute_pipeline) {
            SDL_Log("Compute pipeline build failed.");
            return 1;
        }

        SDL_GPUTextureCreateInfo pp_info = {
            .type = SDL_GPU_TEXTURETYPE_2D,
            .format = PP_FMT,
            .usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET
                   | SDL_GPU_TEXTUREUSAGE_SAMPLER,
            .width = (Uint32)pp_w,
            .height = (Uint32)pp_h,
            .layer_count_or_depth = 1,
            .num_levels = 1,
            .sample_count = SDL_GPU_SAMPLECOUNT_1,
        };
        for (int i = 0; i < 2; i++) {
            ping_pong[i] = SDL_CreateGPUTexture(device, &pp_info);
            if (!ping_pong[i]) {
                SDL_Log("ping-pong tex %d failed: %s", i, SDL_GetError());
                return 1;
            }
        }
        // Clear both ping-pong textures explicitly. Newly-created GPU textures
        // hold uninitialized memory; the shader's "preserve if g > 0.5" branch
        // would lock garbage in place if the first sample wasn't all-zero.
        for (int i = 0; i < 2; i++) {
            SDL_GPUCommandBuffer *clr_cmd = SDL_AcquireGPUCommandBuffer(device);
            SDL_GPUColorTargetInfo clr_info = {
                .texture = ping_pong[i],
                .clear_color = {0.0f, 0.0f, 0.0f, 0.0f},
                .load_op = SDL_GPU_LOADOP_CLEAR,
                .store_op = SDL_GPU_STOREOP_STORE,
            };
            SDL_GPURenderPass *clr =
                SDL_BeginGPURenderPass(clr_cmd, &clr_info, 1, NULL);
            SDL_EndGPURenderPass(clr);
            SDL_SubmitGPUCommandBuffer(clr_cmd);
        }
        SDL_Log("Feedback mode ON — compute %s + display %s, ping-pong %dx%d.",
                "wolframs.frag (compute variant)", "(display variant)",
                pp_w, pp_h);
    }

    SDL_GPUTexture *image_tex = NULL;
    if (feedback_mode) {
        // sampler binds to ping_pong textures per-pass; no static image needed
    } else if (qr_text) {
        image_tex = create_qr_texture_from_text(device, qr_text);
    } else {
        char image_path[1024];
        SDL_snprintf(image_path, sizeof(image_path), "%s%s", base_path,
                     DEFAULT_IMAGE_PATH);
        image_tex = load_image_texture(device, image_path, NULL, NULL);
    }
    if (!feedback_mode && !image_tex) {
        return 1;
    }

    // LINEAR sampling for the QR. Combined with the smoothstep dilation in
    // sample_qr(), edges stay crisp while the sub-pixel transition zone
    // anti-aliases away the 3-vs-4-px-per-module aliasing.
    SDL_GPUSamplerCreateInfo sampler_info = {
        .min_filter = SDL_GPU_FILTER_LINEAR,
        .mag_filter = SDL_GPU_FILTER_LINEAR,
        .mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
        .address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
        .address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
        .address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
    };
    SDL_GPUSampler *sampler = SDL_CreateGPUSampler(device, &sampler_info);
    if (!sampler) {
        SDL_Log("SDL_CreateGPUSampler failed: %s", SDL_GetError());
        return 1;
    }

    // ---- Record mode: render N frames offscreen, write each as PNG, exit. ----
    if (record_mode) {
        const int W = rec_w, H = rec_h;
        const SDL_GPUTextureFormat REC_FMT = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;

        // Pipeline matching the offscreen format (swapchain format may differ).
        SDL_GPUGraphicsPipeline *rec_pipeline =
            build_pipeline(device, REC_FMT, vert_spv_initial, frag_spv_initial);
        if (!rec_pipeline) return 1;

        SDL_GPUTextureCreateInfo target_info = {
            .type = SDL_GPU_TEXTURETYPE_2D,
            .format = REC_FMT,
            .usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
            .width = (Uint32)W,
            .height = (Uint32)H,
            .layer_count_or_depth = 1,
            .num_levels = 1,
            .sample_count = SDL_GPU_SAMPLECOUNT_1,
        };
        SDL_GPUTexture *target = SDL_CreateGPUTexture(device, &target_info);
        if (!target) {
            SDL_Log("offscreen SDL_CreateGPUTexture failed: %s", SDL_GetError());
            return 1;
        }

        const size_t bytes = (size_t)W * (size_t)H * 4;
        SDL_GPUTransferBufferCreateInfo dlbuf_info = {
            .usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,
            .size = (Uint32)bytes,
        };
        SDL_GPUTransferBuffer *dlbuf =
            SDL_CreateGPUTransferBuffer(device, &dlbuf_info);
        if (!dlbuf) {
            SDL_Log("download buffer failed: %s", SDL_GetError());
            return 1;
        }

        if (!SDL_CreateDirectory(record_dir)) {
            SDL_Log("Could not create %s: %s", record_dir, SDL_GetError());
            return 1;
        }

        const int N = (int)(record_duration * (float)record_fps + 0.5f);
        SDL_Log("Recording %d frames (%.2fs @ %d fps) at %dx%d to %s/",
                N, record_duration, record_fps, W, H, record_dir);

        for (int frame = 0; frame < N; frame++) {
            SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(device);
            if (!cmd) {
                SDL_Log("AcquireGPUCommandBuffer failed: %s", SDL_GetError());
                return 1;
            }

            float t = (float)frame / (float)record_fps + start_time;
            float ubo[4] = { (float)W, (float)H, t, loop_period };

            // ---- Feedback compute pass (writes ping_pong[cur] from prev) ----
            int cur = frame & 1;
            int prev = 1 - cur;
            if (feedback_mode) {
                SDL_GPUColorTargetInfo cp_target = {
                    .texture = ping_pong[cur],
                    .clear_color = {0.0f, 0.0f, 0.0f, 0.0f},
                    .load_op = SDL_GPU_LOADOP_DONT_CARE,
                    .store_op = SDL_GPU_STOREOP_STORE,
                };
                SDL_GPURenderPass *cp =
                    SDL_BeginGPURenderPass(cmd, &cp_target, 1, NULL);
                SDL_BindGPUGraphicsPipeline(cp, compute_pipeline);
                SDL_GPUTextureSamplerBinding cb = {
                    .texture = ping_pong[prev], .sampler = sampler,
                };
                SDL_BindGPUFragmentSamplers(cp, 0, &cb, 1);
                SDL_PushGPUFragmentUniformData(cmd, 0, ubo, sizeof(ubo));
                SDL_DrawGPUPrimitives(cp, 3, 1, 0, 0);
                SDL_EndGPURenderPass(cp);
            }

            // ---- Display pass into the offscreen record target ----
            SDL_GPUColorTargetInfo color_target = {
                .texture = target,
                .clear_color = {0.0f, 0.0f, 0.0f, 1.0f},
                .load_op = SDL_GPU_LOADOP_CLEAR,
                .store_op = SDL_GPU_STOREOP_STORE,
            };
            SDL_GPURenderPass *pass =
                SDL_BeginGPURenderPass(cmd, &color_target, 1, NULL);
            SDL_BindGPUGraphicsPipeline(pass, rec_pipeline);

            SDL_GPUTextureSamplerBinding binding = {
                .texture = feedback_mode ? ping_pong[cur] : image_tex,
                .sampler = sampler,
            };
            SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);

            SDL_PushGPUFragmentUniformData(cmd, 0, ubo, sizeof(ubo));

            SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
            SDL_EndGPURenderPass(pass);

            SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
            SDL_GPUTextureRegion src_region = {
                .texture = target,
                .w = (Uint32)W, .h = (Uint32)H, .d = 1,
            };
            SDL_GPUTextureTransferInfo dst_xfer = {
                .transfer_buffer = dlbuf,
                .pixels_per_row = (Uint32)W,
                .rows_per_layer = (Uint32)H,
            };
            SDL_DownloadFromGPUTexture(copy, &src_region, &dst_xfer);
            SDL_EndGPUCopyPass(copy);

            SDL_GPUFence *fence =
                SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
            SDL_WaitForGPUFences(device, true, &fence, 1);
            SDL_ReleaseGPUFence(device, fence);

            void *map = SDL_MapGPUTransferBuffer(device, dlbuf, false);
            char path[1024];
            SDL_snprintf(path, sizeof(path), "%s/frame_%05d.png",
                         record_dir, frame);
            if (!stbi_write_png(path, W, H, 4, map, W * 4)) {
                SDL_Log("stbi_write_png failed for %s", path);
            }
            SDL_UnmapGPUTransferBuffer(device, dlbuf);

            if ((frame % 10) == 9 || frame == N - 1) {
                SDL_Log("  frame %d/%d", frame + 1, N);
            }
        }

        SDL_Log("Done. Assemble with:");
        SDL_Log("  gifski -o output.gif --fps %d %s/frame_*.png", record_fps,
                record_dir);
        SDL_Log("Or with ffmpeg (lower quality):");
        SDL_Log("  ffmpeg -framerate %d -i %s/frame_%%05d.png -loop 0 output.gif",
                record_fps, record_dir);

        SDL_ReleaseGPUTransferBuffer(device, dlbuf);
        SDL_ReleaseGPUTexture(device, target);
        SDL_ReleaseGPUGraphicsPipeline(device, rec_pipeline);
        SDL_ReleaseGPUSampler(device, sampler);
        if (image_tex) SDL_ReleaseGPUTexture(device, image_tex);
        if (compute_pipeline) SDL_ReleaseGPUGraphicsPipeline(device, compute_pipeline);
        for (int i = 0; i < 2; i++) {
            if (ping_pong[i]) SDL_ReleaseGPUTexture(device, ping_pong[i]);
        }
        SDL_ReleaseGPUGraphicsPipeline(device, pipeline);
        SDL_ReleaseWindowFromGPUDevice(device, window);
        SDL_DestroyGPUDevice(device);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 0;
    }

    // Seed mtimes so the first frame doesn't trigger an immediate reload.
    time_t vert_mtime = 0, frag_mtime = 0;
    bool have_vert_src = file_mtime(vert_src_path, &vert_mtime);
    bool have_frag_src = file_mtime(frag_src_path, &frag_mtime);
    bool hot_reload_ok = have_vert_src && have_frag_src;
    if (!hot_reload_ok) {
        SDL_Log("Hot reload disabled (vert=%s, frag=%s).",
                have_vert_src ? "ok" : "missing",
                have_frag_src ? "ok" : "missing");
    }

    bool running = true;
    Uint64 frame_counter = 0;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_EVENT_QUIT:
                running = false;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (event.key.key == SDLK_ESCAPE) {
                    running = false;
                }
                break;
            default:
                break;
            }
        }

        if (hot_reload_ok) {
            time_t mt;
            bool changed = false;
            if (file_mtime(vert_src_path, &mt) && mt != vert_mtime) {
                vert_mtime = mt;
                changed = true;
            }
            if (file_mtime(frag_src_path, &mt) && mt != frag_mtime) {
                frag_mtime = mt;
                changed = true;
            }
            if (changed) {
                if (!compile_shader_glslc(vert_src_path, vert_spv_tmp, NULL) ||
                    !compile_shader_glslc(frag_src_path, frag_spv_tmp, NULL)) {
                    SDL_Log("Shader compile failed; keeping previous pipeline.");
                } else {
                    SDL_GPUGraphicsPipeline *new_pipe = build_pipeline(
                        device, live_format, vert_spv_tmp, frag_spv_tmp);
                    if (new_pipe) {
                        SDL_WaitForGPUIdle(device);
                        SDL_ReleaseGPUGraphicsPipeline(device, pipeline);
                        pipeline = new_pipe;
                        SDL_Log("Shader reloaded.");
                    }
                }
            }
        }

        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(device);
        if (!cmd) {
            SDL_Log("SDL_AcquireGPUCommandBuffer failed: %s", SDL_GetError());
            continue;
        }

        SDL_GPUTexture *swap_tex = NULL;
        Uint32 swap_w = 0, swap_h = 0;
        if (!SDL_AcquireGPUSwapchainTexture(cmd, window, &swap_tex, &swap_w,
                                            &swap_h)) {
            SDL_Log("SDL_AcquireGPUSwapchainTexture failed: %s", SDL_GetError());
            SDL_SubmitGPUCommandBuffer(cmd);
            continue;
        }

        if (swap_tex) {
            float now = (float)SDL_GetTicks() / 1000.0f + start_time;

            // ---- Feedback compute pass (ping_pong[cur] from prev) ----
            int cur = (int)(frame_counter & 1);
            int prev = 1 - cur;
            if (feedback_mode) {
                SDL_GPUColorTargetInfo cp_target = {
                    .texture = ping_pong[cur],
                    .clear_color = {0.0f, 0.0f, 0.0f, 0.0f},
                    .load_op = SDL_GPU_LOADOP_DONT_CARE,
                    .store_op = SDL_GPU_STOREOP_STORE,
                };
                SDL_GPURenderPass *cp =
                    SDL_BeginGPURenderPass(cmd, &cp_target, 1, NULL);
                SDL_BindGPUGraphicsPipeline(cp, compute_pipeline);
                SDL_GPUTextureSamplerBinding cb = {
                    .texture = ping_pong[prev], .sampler = sampler,
                };
                SDL_BindGPUFragmentSamplers(cp, 0, &cb, 1);
                float cubo[4] = {
                    (float)pp_w, (float)pp_h, now, loop_period,
                };
                SDL_PushGPUFragmentUniformData(cmd, 0, cubo, sizeof(cubo));
                SDL_DrawGPUPrimitives(cp, 3, 1, 0, 0);
                SDL_EndGPURenderPass(cp);
            }

            // ---- Display pass (to swapchain) ----
            SDL_GPUColorTargetInfo color_target = {
                .texture = swap_tex,
                .clear_color = {0.06f, 0.08f, 0.11f, 1.0f},
                .load_op = SDL_GPU_LOADOP_CLEAR,
                .store_op = SDL_GPU_STOREOP_STORE,
            };

            SDL_GPURenderPass *pass =
                SDL_BeginGPURenderPass(cmd, &color_target, 1, NULL);
            SDL_BindGPUGraphicsPipeline(pass, pipeline);

            SDL_GPUTextureSamplerBinding binding = {
                .texture = feedback_mode ? ping_pong[cur] : image_tex,
                .sampler = sampler,
            };
            SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);

            // std140 block: vec2 resolution, float time, float loop_period.
            float ubo[4] = {
                (float)swap_w, (float)swap_h, now, loop_period,
            };
            SDL_PushGPUFragmentUniformData(cmd, 0, ubo, sizeof(ubo));

            SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
            SDL_EndGPURenderPass(pass);
        }

        SDL_SubmitGPUCommandBuffer(cmd);
        frame_counter++;
    }

    SDL_ReleaseGPUSampler(device, sampler);
    if (image_tex) SDL_ReleaseGPUTexture(device, image_tex);
    if (compute_pipeline) SDL_ReleaseGPUGraphicsPipeline(device, compute_pipeline);
    for (int i = 0; i < 2; i++) {
        if (ping_pong[i]) SDL_ReleaseGPUTexture(device, ping_pong[i]);
    }
    SDL_ReleaseGPUGraphicsPipeline(device, pipeline);
    SDL_ReleaseWindowFromGPUDevice(device, window);
    SDL_DestroyGPUDevice(device);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
