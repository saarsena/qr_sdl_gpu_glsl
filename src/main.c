#include <SDL3/SDL.h>
#include <string.h>
#include <zlib.h>

#include "qrcodegen.h"

#define WINDOW_TITLE "SDL3 GPU Template"
#define WINDOW_WIDTH 800
#define WINDOW_HEIGHT 600
#define QR_QUIET_ZONE 4
#define CANTO_FILE "divina_commedia_it.txt"
#define CANTO_START_MARKER "Inferno\nCanto I\n"
#define CANTO_END_MARKER "\n\n\n\nInferno\nCanto "

static SDL_GPUShader *load_shader(SDL_GPUDevice *device,
                                  const char *base_path,
                                  const char *spv_name,
                                  SDL_GPUShaderStage stage,
                                  Uint32 num_samplers,
                                  Uint32 num_uniform_buffers) {
    char path[1024];
    SDL_snprintf(path, sizeof(path), "%sshaders/%s", base_path, spv_name);

    size_t code_size = 0;
    void *code = SDL_LoadFile(path, &code_size);
    if (!code) {
        SDL_Log("Failed to load %s: %s", path, SDL_GetError());
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
        SDL_Log("SDL_CreateGPUShader failed for %s: %s", spv_name, SDL_GetError());
    }
    return shader;
}

/* Strip CR bytes in place (CRLF → LF). Returns the new length. */
static size_t normalize_lf(char *s, size_t len) {
    size_t w = 0;
    for (size_t r = 0; r < len; r++) {
        if (s[r] != '\r') s[w++] = s[r];
    }
    s[w] = '\0';
    return w;
}

/* Slice the canto out of the full Divina Commedia text. The file marks each
 * canto with "Inferno\nCanto N\n" headers separated by four blank lines. */
static char *extract_canto_i(const char *src, size_t src_len, size_t *out_len) {
    const char *start = strstr(src, CANTO_START_MARKER);
    if (!start) {
        SDL_Log("Could not find '%s' in source text", CANTO_START_MARKER);
        return NULL;
    }
    start += strlen(CANTO_START_MARKER);
    while (*start == '\n') start++;

    const char *end = strstr(start, CANTO_END_MARKER);
    if (!end) end = src + src_len;

    size_t len = (size_t)(end - start);
    char *out = (char *)SDL_malloc(len + 1);
    if (!out) return NULL;
    SDL_memcpy(out, start, len);
    out[len] = '\0';
    *out_len = len;
    return out;
}

/* Raw deflate (no zlib/gzip wrapper). Returns malloc'd buffer; caller frees. */
static uint8_t *deflate_raw(const void *src, size_t src_len, size_t *out_len) {
    z_stream zs = {0};
    int rc = deflateInit2(&zs, Z_BEST_COMPRESSION, Z_DEFLATED, -15, 9,
                          Z_DEFAULT_STRATEGY);
    if (rc != Z_OK) {
        SDL_Log("deflateInit2 failed: %d", rc);
        return NULL;
    }

    uLong bound = deflateBound(&zs, (uLong)src_len);
    uint8_t *buf = (uint8_t *)SDL_malloc(bound);
    if (!buf) {
        deflateEnd(&zs);
        return NULL;
    }

    zs.next_in = (Bytef *)src;
    zs.avail_in = (uInt)src_len;
    zs.next_out = buf;
    zs.avail_out = (uInt)bound;

    rc = deflate(&zs, Z_FINISH);
    if (rc != Z_STREAM_END) {
        SDL_Log("deflate failed: %d", rc);
        SDL_free(buf);
        deflateEnd(&zs);
        return NULL;
    }
    *out_len = (size_t)zs.total_out;
    deflateEnd(&zs);
    return buf;
}

static SDL_GPUTexture *create_qr_texture_from_bytes(SDL_GPUDevice *device,
                                                    const uint8_t *data,
                                                    size_t data_len) {
    if (data_len > qrcodegen_BUFFER_LEN_MAX) {
        SDL_Log("Compressed payload (%zu bytes) exceeds max QR capacity",
                data_len);
        return NULL;
    }

    uint8_t *scratch = (uint8_t *)SDL_malloc(qrcodegen_BUFFER_LEN_MAX);
    uint8_t *qrcode = (uint8_t *)SDL_malloc(qrcodegen_BUFFER_LEN_MAX);
    if (!scratch || !qrcode) {
        SDL_free(scratch);
        SDL_free(qrcode);
        return NULL;
    }
    SDL_memcpy(scratch, data, data_len);

    bool ok = qrcodegen_encodeBinary(scratch, data_len, qrcode,
                                     qrcodegen_Ecc_LOW,
                                     qrcodegen_VERSION_MIN,
                                     qrcodegen_VERSION_MAX,
                                     qrcodegen_Mask_AUTO, true);
    SDL_free(scratch);
    if (!ok) {
        SDL_Log("qrcodegen_encodeBinary failed (payload too large for v40 ECC L?)");
        SDL_free(qrcode);
        return NULL;
    }

    int qr_size = qrcodegen_getSize(qrcode);
    int tex_size = qr_size + 2 * QR_QUIET_ZONE;
    size_t bytes = (size_t)tex_size * (size_t)tex_size;

    SDL_Log("QR: %zu bytes payload → version %d (%dx%d modules)", data_len,
            (qr_size - 17) / 4, qr_size, qr_size);

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

static SDL_GPUTexture *load_canto_qr(SDL_GPUDevice *device,
                                     const char *base_path) {
    char path[1024];
    SDL_snprintf(path, sizeof(path), "%s%s", base_path, CANTO_FILE);

    size_t file_len = 0;
    void *file_data = SDL_LoadFile(path, &file_len);
    if (!file_data) {
        SDL_Log("Failed to load %s: %s", path, SDL_GetError());
        return NULL;
    }
    file_len = normalize_lf((char *)file_data, file_len);

    size_t canto_len = 0;
    char *canto = extract_canto_i((const char *)file_data, file_len, &canto_len);
    SDL_free(file_data);
    if (!canto) return NULL;

    size_t deflated_len = 0;
    uint8_t *deflated = deflate_raw(canto, canto_len, &deflated_len);
    SDL_Log("Canto I: %zu bytes raw → %zu bytes deflated (%.1f%%)", canto_len,
            deflated_len, 100.0 * deflated_len / (double)canto_len);
    SDL_free(canto);
    if (!deflated) return NULL;

    SDL_GPUTexture *tex =
        create_qr_texture_from_bytes(device, deflated, deflated_len);
    SDL_free(deflated);
    return tex;
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

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

    SDL_GPUShader *vert = load_shader(device, base_path, "triangle.vert.spv",
                                      SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
    SDL_GPUShader *frag = load_shader(device, base_path, "triangle.frag.spv",
                                      SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
    if (!vert || !frag) {
        return 1;
    }

    SDL_GPUColorTargetDescription color_target_desc = {
        .format = SDL_GetGPUSwapchainTextureFormat(device, window),
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
        return 1;
    }

    SDL_GPUTexture *qr_tex = load_canto_qr(device, base_path);
    if (!qr_tex) {
        return 1;
    }

    SDL_GPUSamplerCreateInfo sampler_info = {
        .min_filter = SDL_GPU_FILTER_NEAREST,
        .mag_filter = SDL_GPU_FILTER_NEAREST,
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

    bool running = true;
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
                .texture = qr_tex,
                .sampler = sampler,
            };
            SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);

            // std140 block: vec2 resolution, float time, plus 4-byte pad
            // to round the block size up to 16 bytes.
            // NOTE: 165-module QR @ ~3.12 px/module produces some 3-px and
            // some 4-px modules under NEAREST sampling — known aliasing.
            float ubo[4] = {
                (float)swap_w,
                (float)swap_h,
                (float)SDL_GetTicks() / 1000.0f,
                0.0f,
            };
            SDL_PushGPUFragmentUniformData(cmd, 0, ubo, sizeof(ubo));

            SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
            SDL_EndGPURenderPass(pass);
        }

        SDL_SubmitGPUCommandBuffer(cmd);
    }

    SDL_ReleaseGPUSampler(device, sampler);
    SDL_ReleaseGPUTexture(device, qr_tex);
    SDL_ReleaseGPUGraphicsPipeline(device, pipeline);
    SDL_ReleaseWindowFromGPUDevice(device, window);
    SDL_DestroyGPUDevice(device);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
