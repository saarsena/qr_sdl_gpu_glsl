#include <SDL3/SDL.h>
#include <string.h>

#include "qrcodegen.h"

#define WINDOW_TITLE "SDL3 GPU Template"
#define WINDOW_WIDTH 800
#define WINDOW_HEIGHT 600
#define QR_TEXT "Hello QR via SDL_GPU + GLSL"
#define QR_QUIET_ZONE 4

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

static SDL_GPUTexture *create_qr_texture(SDL_GPUDevice *device,
                                         const char *text) {
    uint8_t qrcode[qrcodegen_BUFFER_LEN_MAX];
    uint8_t scratch[qrcodegen_BUFFER_LEN_MAX];

    bool ok = qrcodegen_encodeText(text, scratch, qrcode,
                                   qrcodegen_Ecc_MEDIUM,
                                   qrcodegen_VERSION_MIN,
                                   qrcodegen_VERSION_MAX,
                                   qrcodegen_Mask_AUTO, true);
    if (!ok) {
        SDL_Log("qrcodegen_encodeText failed (text too long?)");
        return NULL;
    }

    int qr_size = qrcodegen_getSize(qrcode);
    int tex_size = qr_size + 2 * QR_QUIET_ZONE;
    size_t bytes = (size_t)tex_size * (size_t)tex_size;

    uint8_t *pixels = (uint8_t *)SDL_calloc(1, bytes);
    if (!pixels) {
        SDL_Log("Out of memory building QR pixel buffer");
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

    SDL_GPUTexture *qr_tex = create_qr_texture(device, QR_TEXT);
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

            float resolution[2] = {(float)swap_w, (float)swap_h};
            SDL_PushGPUFragmentUniformData(cmd, 0, resolution,
                                           sizeof(resolution));

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
