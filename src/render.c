#include <SDL3/SDL.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "gpu.h"
#include "host_log.h"
#include "precise.h"
#include "psyq/host_psyq.h"
#include "render.h"
#include "settings.h"

/* The shaders, compiled to SPIR-V by the build (shaders/) */
#include "draw_vert.h"
#include "draw_frag.h"
#include "present_vert.h"
#include "present_frag.h"

/* The GPU renderer. Each primitive gpu.c hands the software rasterizer is recorded again here: its vertices in VRAM's
 * pixels, its texture page, CLUT, window and the limits of its texture coordinates, its blending. At a vertical blank
 * the frame's records go to the GPU in order: the vertices, then each step, a draw into the target (VRAM at the
 * resolution's multiple), an upload of the software VRAM (the textures the draws after it read, taken when a load, copy
 * or fill has changed it), or a copy within the target. The target's displayed rectangle is the window's picture.
 * A load into VRAM is drawn into the target from the software VRAM, so what the CPU writes shows as it does there.
 * TRADEOFF: the textures are the software VRAM as of the last load, copy or fill: a draw that reads what the frame
 * drew earlier (the framebuffer as a texture) reads it as it was then; tracking the draws would cost an upload each */

#define VRAM_BYTES (VRAM_WIDTH * VRAM_HEIGHT * 2)
#define VERTICES_MAX 262144
#define STEPS_MAX 16384
#define SNAPSHOTS_MAX 16
/* Pictures submitted and not yet drawn, at most: each holds its buffers, which SDL_GPU otherwise copies anew, 26 MB a
 * time, into a 32-bit address space (headless nothing else waits on the GPU) */
#define FRAMES_IN_FLIGHT 2
/* The largest displayed rectangle, the software picture's texture */
#define PICTURE_WIDTH 640
#define PICTURE_HEIGHT 512
#define PICTURE_PIXEL_BYTES 4
#define TARGET_PIXEL_BYTES 4
#define TRIANGLE_VERTICES 3
#define QUAD_TRIANGLE_VERTICES 6
#define PRESENT_VERTICES 6
#define QUAD_CORNERS 4
/* A shader's uniform, a vec4 */
#define UNIFORM_FLOATS 4
#define RGB_BYTES 3
/* The 4:3 picture */
#define ASPECT_WIDTH 4
#define ASPECT_HEIGHT 3
#define WIDE_ASPECT_WIDTH 16
#define WIDE_ASPECT_HEIGHT 9
#define HALF 0.5f

/* The command's bits (gpu.c's) and E1's: the page's fields, the blending mode, the rectangles' flips */
#define COMMAND_SHIFT 24
#define COMMAND_RAW 0x01
#define COMMAND_SEMI_TRANSPARENT 0x02
#define COMMAND_TEXTURED 0x04
#define DRAW_MODE_PAGE_MASK 0x19F
#define BLEND_MODE_SHIFT 5
#define BLEND_MODE_MASK 3
#define FLIP_X 0x1000
#define FLIP_Y 0x2000
#define PALETTE_SHIFT 16
#define COLOUR_MASK 0xFFFFFF
#define OPAQUE_ALPHA 0xFF000000u
#define BYTE_MASK 0xFF
#define BYTE_SHIFT 8
#define SECOND_BYTE_SHIFT 16
#define THIRD_BYTE_SHIFT 24
/* E2's fields (gpu_raster.cpp's texture_window) */
#define WINDOW_FIELD_MASK 0x1F
#define WINDOW_MASK_Y_SHIFT 5
#define WINDOW_OFFSET_X_SHIFT 10
#define WINDOW_OFFSET_Y_SHIFT 15
#define WINDOW_UNIT 8
/* A polygon the GPU skips: an edge as wide as VRAM or as high as half of it */
#define POLYGON_MAX_WIDTH 1024
#define POLYGON_MAX_HEIGHT 512
/* A flipped rectangle's first texture coordinate just short of the next texel, so each pixel's floor is its own */
#define FLIP_START (1.0f - 1.0f / 1024.0f)

/* draw.frag's flags */
enum {
    FLAG_TEXTURED = 1,
    FLAG_RAW = 2,
    FLAG_ABSOLUTE = 4,
    FLAG_OPAQUE_TEXELS = 8,
    FLAG_BLENDED_TEXELS = 16,
    FLAG_HALF = 32,
    FLAG_QUARTER = 64
};

/* The pipelines: opaque, and one for each of the GPU's four blendings (B/2 + F/2, B + F, B - F, B + F/4) */
enum {
    PIPELINE_OPAQUE,
    PIPELINE_AVERAGE,
    PIPELINE_ADD,
    PIPELINE_SUBTRACT,
    PIPELINE_ADD_QUARTER,
    PIPELINES
};

/* A quad's corners (top left, top right, bottom left, bottom right) as two triangles, as the GPU splits it */
static const int s_QuadOrder[QUAD_TRIANGLE_VERTICES] = {0, 1, 2, 1, 2, 3};

typedef struct {
    float x, y;
    uint32_t colour;
    float u, v;
    uint32_t texture;
    uint32_t limits;
    uint32_t window;
    uint32_t flags;
    float depth;
} DrawVertex;

typedef enum {
    STEP_DRAW,
    STEP_UPLOAD,
    STEP_COPY
} StepKind;

typedef struct {
    StepKind kind;
    int pipeline;
    int first, count;
    SDL_Rect scissor;
    int snapshot;
    int from_x, from_y, to_x, to_y, width, height;
} Step;

static SDL_GPUDevice *s_Device;
static SDL_Window *s_Window;
static SDL_GPUGraphicsPipeline *s_Draw[PIPELINES];
static SDL_GPUGraphicsPipeline *s_Present;
static SDL_GPUSampler *s_Nearest;
static SDL_GPUSampler *s_Linear;
static SDL_GPUTexture *s_Vram;
static SDL_GPUTexture *s_Picture;
static SDL_GPUTexture *s_Target;
static SDL_GPUTexture *s_Copy;
static SDL_GPUBuffer *s_VertexBuffer;
static SDL_GPUTransferBuffer *s_VertexTransfer;
static SDL_GPUTransferBuffer *s_SnapshotTransfer;
static SDL_GPUTransferBuffer *s_PictureTransfer;
static int s_Scale = 1;
static DrawVertex *s_Vertices;
static unsigned char *s_Snapshots;
static int s_VertexCount;
static int s_SnapshotCount;
static Step s_Steps[STEPS_MAX];
static int s_StepCount;
static int s_VramChanged = 1;
static SDL_Rect s_Area;
static int s_Mapped;
static SDL_GPUFence *s_Fences[FRAMES_IN_FLIGHT];
static int s_Frame;

static SDL_GPUShader *shader(const uint32_t *code, size_t bytes, SDL_GPUShaderStage stage, Uint32 samplers,
                             Uint32 uniforms) {
    SDL_GPUShaderCreateInfo info;

    SDL_zero(info);
    info.code = (const Uint8 *)code;
    info.code_size = bytes;
    info.entrypoint = "main";
    info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    info.stage = stage;
    info.num_samplers = samplers;
    info.num_uniform_buffers = uniforms;
    return SDL_CreateGPUShader(s_Device, &info);
}

/* A blending's factors: the colour the shader scales (draw.frag's HALF and QUARTER) added to or taken from what is
 * there, B/2 + F/2 by the shader's alpha of a half */
static SDL_GPUColorTargetBlendState blending(int pipeline) {
    SDL_GPUColorTargetBlendState blend;

    SDL_zero(blend);
    blend.enable_blend = pipeline != PIPELINE_OPAQUE;
    blend.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    blend.dst_color_blendfactor =
        pipeline == PIPELINE_AVERAGE ? SDL_GPU_BLENDFACTOR_SRC_ALPHA : SDL_GPU_BLENDFACTOR_ONE;
    blend.color_blend_op = pipeline == PIPELINE_SUBTRACT ? SDL_GPU_BLENDOP_REVERSE_SUBTRACT : SDL_GPU_BLENDOP_ADD;
    blend.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    blend.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
    blend.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    return blend;
}

static int create_draw_pipelines(void) {
    static const SDL_GPUVertexAttribute attributes[] = {
        {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(DrawVertex, x)},
        {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, offsetof(DrawVertex, colour)},
        {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(DrawVertex, u)},
        {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_UINT, offsetof(DrawVertex, texture)},
        {4, 0, SDL_GPU_VERTEXELEMENTFORMAT_UINT, offsetof(DrawVertex, limits)},
        {5, 0, SDL_GPU_VERTEXELEMENTFORMAT_UINT, offsetof(DrawVertex, window)},
        {6, 0, SDL_GPU_VERTEXELEMENTFORMAT_UINT, offsetof(DrawVertex, flags)},
        {7, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, offsetof(DrawVertex, depth)},
    };
    SDL_GPUVertexBufferDescription buffer;
    SDL_GPUColorTargetDescription target;
    SDL_GPUGraphicsPipelineCreateInfo info;
    SDL_GPUShader *vertex = shader(draw_vert, sizeof(draw_vert), SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
    SDL_GPUShader *fragment = shader(draw_frag, sizeof(draw_frag), SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0);
    int pipeline;
    int created = vertex != NULL && fragment != NULL;

    SDL_zero(buffer);
    buffer.slot = 0;
    buffer.pitch = sizeof(DrawVertex);
    buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    SDL_zero(info);
    info.vertex_shader = vertex;
    info.fragment_shader = fragment;
    info.vertex_input_state.vertex_buffer_descriptions = &buffer;
    info.vertex_input_state.num_vertex_buffers = 1;
    info.vertex_input_state.vertex_attributes = attributes;
    info.vertex_input_state.num_vertex_attributes = SDL_arraysize(attributes);
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.target_info.color_target_descriptions = &target;
    info.target_info.num_color_targets = 1;
    for (pipeline = 0; created && pipeline < PIPELINES; pipeline++) {
        SDL_zero(target);
        target.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        target.blend_state = blending(pipeline);
        s_Draw[pipeline] = SDL_CreateGPUGraphicsPipeline(s_Device, &info);
        created = s_Draw[pipeline] != NULL;
    }
    if (vertex != NULL) {
        SDL_ReleaseGPUShader(s_Device, vertex);
    } else {
        /* None made */
    }
    if (fragment != NULL) {
        SDL_ReleaseGPUShader(s_Device, fragment);
    } else {
        /* None made */
    }
    return created;
}

static int create_present_pipeline(void) {
    SDL_GPUColorTargetDescription target;
    SDL_GPUGraphicsPipelineCreateInfo info;
    SDL_GPUShader *vertex = shader(present_vert, sizeof(present_vert), SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
    SDL_GPUShader *fragment = shader(present_frag, sizeof(present_frag), SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0);

    if (vertex != NULL && fragment != NULL) {
        SDL_zero(target);
        target.format = SDL_GetGPUSwapchainTextureFormat(s_Device, s_Window);
        SDL_zero(info);
        info.vertex_shader = vertex;
        info.fragment_shader = fragment;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.target_info.color_target_descriptions = &target;
        info.target_info.num_color_targets = 1;
        s_Present = SDL_CreateGPUGraphicsPipeline(s_Device, &info);
    } else {
        /* A shader not made: no pipeline */
    }
    if (vertex != NULL) {
        SDL_ReleaseGPUShader(s_Device, vertex);
    } else {
        /* None made */
    }
    if (fragment != NULL) {
        SDL_ReleaseGPUShader(s_Device, fragment);
    } else {
        /* None made */
    }
    return s_Present != NULL;
}

static SDL_GPUTexture *texture(SDL_GPUTextureFormat format, SDL_GPUTextureUsageFlags usage, int width, int height) {
    SDL_GPUTextureCreateInfo info;

    SDL_zero(info);
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = format;
    info.usage = usage;
    info.width = (Uint32)width;
    info.height = (Uint32)height;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    return SDL_CreateGPUTexture(s_Device, &info);
}

static SDL_GPUTransferBuffer *transfer(SDL_GPUTransferBufferUsage usage, Uint32 bytes) {
    SDL_GPUTransferBufferCreateInfo info;

    SDL_zero(info);
    info.usage = usage;
    info.size = bytes;
    return SDL_CreateGPUTransferBuffer(s_Device, &info);
}

static SDL_GPUSampler *sampler(SDL_GPUFilter filter) {
    SDL_GPUSamplerCreateInfo info;

    SDL_zero(info);
    info.min_filter = filter;
    info.mag_filter = filter;
    info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    return SDL_CreateGPUSampler(s_Device, &info);
}

static int create_resources(void) {
    SDL_GPUBufferCreateInfo vertices;

    SDL_zero(vertices);
    vertices.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    vertices.size = VERTICES_MAX * sizeof(DrawVertex);
    s_VertexBuffer = SDL_CreateGPUBuffer(s_Device, &vertices);
    s_VertexTransfer = transfer(SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, VERTICES_MAX * sizeof(DrawVertex));
    s_SnapshotTransfer = transfer(SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, SNAPSHOTS_MAX * VRAM_BYTES);
    s_PictureTransfer =
        transfer(SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, PICTURE_WIDTH * PICTURE_HEIGHT * PICTURE_PIXEL_BYTES);
    s_Vram = texture(SDL_GPU_TEXTUREFORMAT_R16_UINT, SDL_GPU_TEXTUREUSAGE_SAMPLER, VRAM_WIDTH, VRAM_HEIGHT);
    s_Picture = texture(SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER, PICTURE_WIDTH,
                        PICTURE_HEIGHT);
    s_Nearest = sampler(SDL_GPU_FILTER_NEAREST);
    s_Linear = sampler(SDL_GPU_FILTER_LINEAR);
    return s_VertexBuffer != NULL && s_VertexTransfer != NULL && s_SnapshotTransfer != NULL &&
           s_PictureTransfer != NULL && s_Vram != NULL && s_Picture != NULL && s_Nearest != NULL && s_Linear != NULL;
}

int Render_Start(SDL_Window *window) {
    s_Device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, NULL);
    if (s_Device == NULL) {
        Host_Log("no GPU renderer (%s): the picture at the PlayStation's resolution", SDL_GetError());
        return 0;
    } else if (window != NULL && !SDL_ClaimWindowForGPUDevice(s_Device, window)) {
        Host_Log("no GPU renderer for the window (%s)", SDL_GetError());
        SDL_DestroyGPUDevice(s_Device);
        s_Device = NULL;
        return 0;
    } else {
        s_Window = window;
    }
    if (s_Window != NULL && SDL_WindowSupportsGPUPresentMode(s_Device, s_Window, SDL_GPU_PRESENTMODE_MAILBOX)) {
        /* video.c paces the pictures: the swapchain takes each without waiting */
        SDL_SetGPUSwapchainParameters(s_Device, s_Window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
                                      SDL_GPU_PRESENTMODE_MAILBOX);
    } else {
        /* The swapchain's own pacing, a vertical blank's (VSYNC, always there) */
    }
    if (!create_draw_pipelines() || !create_resources() || (s_Window != NULL && !create_present_pipeline())) {
        Host_Fatal("the GPU renderer: %s", SDL_GetError());
    } else {
        Host_Log("GPU renderer: %s", SDL_GetGPUDeviceDriver(s_Device));
    }
    Render_ApplySettings();
    return 1;
}

int Render_Active(void) {
    return s_Device != NULL;
}

int Render_Drawing(void) {
    return s_Device != NULL && s_Scale > 1;
}

static void map(void) {
    if (!s_Mapped) {
        s_Vertices = SDL_MapGPUTransferBuffer(s_Device, s_VertexTransfer, true);
        s_Snapshots = SDL_MapGPUTransferBuffer(s_Device, s_SnapshotTransfer, true);
        if (s_Vertices == NULL || s_Snapshots == NULL) {
            Host_Fatal("the GPU renderer's buffers: %s", SDL_GetError());
        } else {
            s_Mapped = 1;
        }
    } else {
        /* Mapped since the last submission */
    }
}

static void end_pass(SDL_GPURenderPass **pass) {
    if (*pass != NULL) {
        SDL_EndGPURenderPass(*pass);
        *pass = NULL;
    } else {
        /* None open */
    }
}

static SDL_GPURenderPass *begin_target_pass(SDL_GPUCommandBuffer *command) {
    SDL_GPUColorTargetInfo target;
    SDL_GPUBufferBinding binding;
    SDL_GPURenderPass *pass;

    SDL_zero(target);
    target.texture = s_Target;
    target.load_op = SDL_GPU_LOADOP_LOAD;
    target.store_op = SDL_GPU_STOREOP_STORE;
    pass = SDL_BeginGPURenderPass(command, &target, 1, NULL);
    SDL_zero(binding);
    binding.buffer = s_VertexBuffer;
    SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
    return pass;
}

static void upload_snapshot(SDL_GPUCommandBuffer *command, int snapshot) {
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(command);
    SDL_GPUTextureTransferInfo from;
    SDL_GPUTextureRegion to;

    SDL_zero(from);
    from.transfer_buffer = s_SnapshotTransfer;
    from.offset = (Uint32)(snapshot * VRAM_BYTES);
    SDL_zero(to);
    to.texture = s_Vram;
    to.w = VRAM_WIDTH;
    to.h = VRAM_HEIGHT;
    to.d = 1;
    SDL_UploadToGPUTexture(copy, &from, &to, true);
    SDL_EndGPUCopyPass(copy);
}

/* A copy within the target, through the copy texture: the rectangles may overlap */
static void copy_target(SDL_GPUCommandBuffer *command, const Step *step) {
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(command);
    SDL_GPUTextureLocation from, to;

    SDL_zero(from);
    SDL_zero(to);
    from.texture = s_Target;
    from.x = (Uint32)(step->from_x * s_Scale);
    from.y = (Uint32)(step->from_y * s_Scale);
    to.texture = s_Copy;
    to.x = from.x;
    to.y = from.y;
    SDL_CopyGPUTextureToTexture(copy, &from, &to, (Uint32)(step->width * s_Scale), (Uint32)(step->height * s_Scale), 1,
                                false);
    from.texture = s_Copy;
    to.texture = s_Target;
    to.x = (Uint32)(step->to_x * s_Scale);
    to.y = (Uint32)(step->to_y * s_Scale);
    SDL_CopyGPUTextureToTexture(copy, &from, &to, (Uint32)(step->width * s_Scale), (Uint32)(step->height * s_Scale), 1,
                                false);
    SDL_EndGPUCopyPass(copy);
}

/* The records so far into the command buffer, in order; then none */
static void encode(SDL_GPUCommandBuffer *command) {
    SDL_GPURenderPass *pass = NULL;
    SDL_GPUTextureSamplerBinding vram;
    float params[UNIFORM_FLOATS] = {0};
    int bound = -1;
    int index;

    if (s_Mapped) {
        SDL_UnmapGPUTransferBuffer(s_Device, s_VertexTransfer);
        SDL_UnmapGPUTransferBuffer(s_Device, s_SnapshotTransfer);
        s_Mapped = 0;
    } else {
        /* Nothing recorded */
    }
    if (s_VertexCount > 0) {
        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(command);
        SDL_GPUTransferBufferLocation from;
        SDL_GPUBufferRegion to;

        SDL_zero(from);
        from.transfer_buffer = s_VertexTransfer;
        SDL_zero(to);
        to.buffer = s_VertexBuffer;
        to.size = (Uint32)(s_VertexCount * sizeof(DrawVertex));
        SDL_UploadToGPUBuffer(copy, &from, &to, true);
        SDL_EndGPUCopyPass(copy);
    } else {
        /* No vertices */
    }
    params[0] = HALF / (float)s_Scale;
    SDL_PushGPUVertexUniformData(command, 0, params, sizeof(params));
    vram.texture = s_Vram;
    vram.sampler = s_Nearest;
    for (index = 0; index < s_StepCount; index++) {
        const Step *step = &s_Steps[index];

        switch (step->kind) {
        case STEP_UPLOAD:
            end_pass(&pass);
            upload_snapshot(command, step->snapshot);
            break;
        case STEP_COPY:
            end_pass(&pass);
            copy_target(command, step);
            break;
        case STEP_DRAW:
            if (pass == NULL) {
                pass = begin_target_pass(command);
                bound = -1;
            } else {
                /* Drawing on */
            }
            if (step->pipeline != bound) {
                SDL_BindGPUGraphicsPipeline(pass, s_Draw[step->pipeline]);
                SDL_BindGPUFragmentSamplers(pass, 0, &vram, 1);
                bound = step->pipeline;
            } else {
                /* Bound */
            }
            SDL_SetGPUScissor(pass, &step->scissor);
            SDL_DrawGPUPrimitives(pass, (Uint32)step->count, 1, (Uint32)step->first, 0);
            break;
        default:
            Host_Fatal("the GPU renderer: a step of kind %d", (int)step->kind);
            break;
        }
    }
    end_pass(&pass);
    s_StepCount = 0;
    s_VertexCount = 0;
    s_SnapshotCount = 0;
}

/* The records submitted, with no picture: the buffers full, the scale changing, a shot */
static void flush(void) {
    SDL_GPUCommandBuffer *command = SDL_AcquireGPUCommandBuffer(s_Device);

    if (command == NULL) {
        Host_Fatal("the GPU renderer's command buffer: %s", SDL_GetError());
    } else {
        encode(command);
        SDL_SubmitGPUCommandBuffer(command);
    }
}

/* Room in the records for vertices, steps and snapshots, the records submitted first if they haven't it; mapped */
static void make_room(int vertices, int steps, int snapshots) {
    if (s_VertexCount + vertices > VERTICES_MAX || s_StepCount + steps > STEPS_MAX ||
        s_SnapshotCount + snapshots > SNAPSHOTS_MAX) {
        flush();
    } else {
        /* Room */
    }
    map();
}

/* A step, in the room make_room made */
static Step *add_step(StepKind kind) {
    if (s_StepCount == STEPS_MAX) {
        Host_Fatal("the GPU renderer: a step past its room");
    } else {
        s_Steps[s_StepCount].kind = kind;
    }
    return &s_Steps[s_StepCount++];
}

/* The software VRAM as the textures of the draws after, if a load, copy or fill has changed it */
static void take_snapshot(void) {
    Step *step;

    if (!s_VramChanged) {
        return;
    } else {
        make_room(0, 1, 1);
    }
    memcpy(s_Snapshots + s_SnapshotCount * VRAM_BYTES, Host_Vram(), VRAM_BYTES);
    step = add_step(STEP_UPLOAD);
    step->snapshot = s_SnapshotCount++;
    s_VramChanged = 0;
}

static SDL_Rect scaled(int x, int y, int width, int height) {
    SDL_Rect rect;

    rect.x = x * s_Scale;
    rect.y = y * s_Scale;
    rect.w = width * s_Scale;
    rect.h = height * s_Scale;
    return rect;
}

/* Room for count vertices drawn with the pipeline within the scissor, the last draw's step taking them if it has the
 * same */
static DrawVertex *reserve(int count, int pipeline, const SDL_Rect *scissor) {
    Step *last;
    DrawVertex *vertices;

    make_room(count, 1, 0);
    last = s_StepCount > 0 ? &s_Steps[s_StepCount - 1] : NULL;
    if (last != NULL && last->kind == STEP_DRAW && last->pipeline == pipeline &&
        SDL_memcmp(&last->scissor, scissor, sizeof(*scissor)) == 0) {
        last->count += count;
    } else {
        last = add_step(STEP_DRAW);
        last->pipeline = pipeline;
        last->scissor = *scissor;
        last->first = s_VertexCount;
        last->count = count;
    }
    vertices = &s_Vertices[s_VertexCount];
    s_VertexCount += count;
    return vertices;
}

/* A primitive's own: its texture (page and CLUT), the limits of its texture coordinates, its window */
typedef struct {
    uint32_t texture;
    uint32_t limits;
    uint32_t window;
    uint32_t flags;
    int pipeline;
} Shading;

static uint32_t window(uint32_t value) {
    uint32_t mask_x = value & WINDOW_FIELD_MASK;
    uint32_t mask_y = (value >> WINDOW_MASK_Y_SHIFT) & WINDOW_FIELD_MASK;
    uint32_t offset_x = (value >> WINDOW_OFFSET_X_SHIFT) & WINDOW_FIELD_MASK;
    uint32_t offset_y = (value >> WINDOW_OFFSET_Y_SHIFT) & WINDOW_FIELD_MASK;
    uint32_t and_x = ~(mask_x * WINDOW_UNIT) & BYTE_MASK;
    uint32_t and_y = ~(mask_y * WINDOW_UNIT) & BYTE_MASK;
    uint32_t or_x = (offset_x & mask_x) * WINDOW_UNIT;
    uint32_t or_y = (offset_y & mask_y) * WINDOW_UNIT;

    return and_x | (and_y << BYTE_SHIFT) | (or_x << SECOND_BYTE_SHIFT) | (or_y << THIRD_BYTE_SHIFT);
}

static uint32_t limits(int low_u, int low_v, int high_u, int high_v) {
    return (uint32_t)low_u | ((uint32_t)low_v << BYTE_SHIFT) | ((uint32_t)high_u << SECOND_BYTE_SHIFT) |
           ((uint32_t)high_v << THIRD_BYTE_SHIFT);
}

static Shading shading(const RasterState *state) {
    uint32_t command = state->command >> COMMAND_SHIFT;
    Shading result;

    result.texture = (state->draw_mode & DRAW_MODE_PAGE_MASK) | ((uint32_t)state->palette << PALETTE_SHIFT);
    result.limits = limits(0, 0, BYTE_MASK, BYTE_MASK);
    result.window = window(state->texture_window);
    result.flags = 0;
    result.pipeline = PIPELINE_OPAQUE;
    if (command & COMMAND_TEXTURED) {
        result.flags |= FLAG_TEXTURED;
        take_snapshot();
    } else {
        /* Its colour alone */
    }
    if ((command & COMMAND_TEXTURED) && (command & COMMAND_RAW)) {
        result.flags |= FLAG_RAW;
    } else {
        /* Modulated, or untextured */
    }
    if (command & COMMAND_SEMI_TRANSPARENT) {
        result.pipeline = PIPELINE_AVERAGE + ((state->draw_mode >> BLEND_MODE_SHIFT) & BLEND_MODE_MASK);
    } else {
        /* Opaque */
    }
    return result;
}

/* The draws of a primitive's vertices: a semi-transparent texture's texels blend where their bit is set, the others
 * are drawn opaque */
static void emit(const Shading *shading, const DrawVertex *vertices, int count) {
    SDL_Rect scissor = scaled(s_Area.x, s_Area.y, s_Area.w, s_Area.h);
    uint32_t scale = 0;
    DrawVertex *out;
    int index;

    if (shading->pipeline == PIPELINE_AVERAGE) {
        scale = FLAG_HALF;
    } else if (shading->pipeline == PIPELINE_ADD_QUARTER) {
        scale = FLAG_QUARTER;
    } else {
        /* The colour as it is */
    }
    if (shading->pipeline != PIPELINE_OPAQUE && (shading->flags & FLAG_TEXTURED)) {
        out = reserve(count, PIPELINE_OPAQUE, &scissor);
        for (index = 0; index < count; index++) {
            out[index] = vertices[index];
            out[index].flags |= FLAG_OPAQUE_TEXELS;
        }
        out = reserve(count, shading->pipeline, &scissor);
        for (index = 0; index < count; index++) {
            out[index] = vertices[index];
            out[index].flags |= FLAG_BLENDED_TEXELS | scale;
        }
    } else {
        out = reserve(count, shading->pipeline, &scissor);
        for (index = 0; index < count; index++) {
            out[index] = vertices[index];
            out[index].flags |= scale;
        }
    }
}

static DrawVertex draw_vertex(const Shading *shading, float x, float y, uint32_t colour, float u, float v) {
    DrawVertex vertex;

    vertex.x = x;
    vertex.y = y;
    vertex.colour = (colour & COLOUR_MASK) | OPAQUE_ALPHA;
    vertex.u = u;
    vertex.v = v;
    vertex.texture = shading->texture;
    vertex.limits = shading->limits;
    vertex.window = shading->window;
    vertex.flags = shading->flags;
    vertex.depth = 1.0f;
    return vertex;
}

void Render_SetDrawingArea(uint32_t left, uint32_t top, uint32_t right, uint32_t bottom) {
    s_Area.x = (int)left;
    s_Area.y = (int)top;
    s_Area.w = (int)(right - left + 1);
    s_Area.h = (int)(bottom - top + 1);
}

/* A polygon's vertices' projections (precise.h), as the settings take them: their fractions (steady) and their
 * depths (unwarped). Only if each vertex has one: a polygon the GTE didn't project, the HUD's or the font's, whose
 * values a projection may share by chance, stays as written. A vertex with several takes the one whose depth is
 * nearest the polygon's others' (those with one alone), or with none alone the nearest the first vertex's first */
static void projections_none(float *dx, float *dy, float *depth, int count) {
    int index;

    for (index = 0; index < count; index++) {
        dx[index] = 0.0f;
        dy[index] = 0.0f;
        depth[index] = 1.0f;
    }
}

static float depth_apart(float a, float b) {
    return a > b ? a - b : b - a;
}

static void projections(const RasterVertex *vertices, int count, float *dx, float *dy, float *depth) {
    Projection found[QUAD_CORNERS][CANDIDATES];
    int counts[QUAD_CORNERS];
    int steady = Host_Setting(SETTING_STEADY);
    int unwarped = Host_Setting(SETTING_PERSPECTIVE);
    float reference = 0.0f;
    int alone = 0;
    int index;

    projections_none(dx, dy, depth, count);
    if (!steady && !unwarped) {
        return;
    } else {
        /* Looked up */
    }
    for (index = 0; index < count; index++) {
        counts[index] = Precise_Find(vertices[index].position, found[index]);
        if (counts[index] == 0) {
            return;
        } else if (counts[index] == 1) {
            reference += found[index][0].depth;
            alone++;
        } else {
            /* Chosen below */
        }
    }
    if (alone > 0) {
        reference /= (float)alone;
    } else {
        reference = found[0][0].depth;
    }
    for (index = 0; index < count; index++) {
        const Projection *chosen = &found[index][0];
        int candidate;

        for (candidate = 1; candidate < counts[index]; candidate++) {
            if (depth_apart(found[index][candidate].depth, reference) < depth_apart(chosen->depth, reference)) {
                chosen = &found[index][candidate];
            } else {
                continue;
            }
        }
        dx[index] = steady ? chosen->dx : 0.0f;
        dy[index] = steady ? chosen->dy : 0.0f;
        depth[index] = unwarped ? chosen->depth : 1.0f;
    }
}

void Render_Polygon(const RasterState *state, int count, const RasterVertex *vertices) {
    DrawVertex corners[QUAD_TRIANGLE_VERTICES];
    float dx[QUAD_CORNERS], dy[QUAD_CORNERS], depth[QUAD_CORNERS];
    Shading look;
    int low_u = BYTE_MASK, low_v = BYTE_MASK, high_u = 0, high_v = 0;
    int low_x = vertices[0].x, high_x = vertices[0].x, low_y = vertices[0].y, high_y = vertices[0].y;
    int index;

    if (!Render_Drawing()) {
        return;
    } else {
        look = shading(state);
    }
    for (index = 0; index < count; index++) {
        int u = vertices[index].texcoord & BYTE_MASK;
        int v = vertices[index].texcoord >> BYTE_SHIFT;

        low_u = SDL_min(low_u, u);
        high_u = SDL_max(high_u, u);
        low_v = SDL_min(low_v, v);
        high_v = SDL_max(high_v, v);
        low_x = SDL_min(low_x, vertices[index].x);
        high_x = SDL_max(high_x, vertices[index].x);
        low_y = SDL_min(low_y, vertices[index].y);
        high_y = SDL_max(high_y, vertices[index].y);
    }
    if (high_x - low_x >= POLYGON_MAX_WIDTH || high_y - low_y >= POLYGON_MAX_HEIGHT) {
        return;
    } else if (look.flags & FLAG_TEXTURED) {
        look.limits = limits(low_u, low_v, high_u, high_v);
    } else {
        /* No texture to limit */
    }
    projections(vertices, count, dx, dy, depth);
    for (index = 0; index < (count == TRIANGLE_VERTICES ? TRIANGLE_VERTICES : QUAD_TRIANGLE_VERTICES); index++) {
        int corner = s_QuadOrder[index];
        const RasterVertex *from = &vertices[corner];

        corners[index] = draw_vertex(&look, (float)from->x + dx[corner], (float)from->y + dy[corner], from->color,
                                     (float)(from->texcoord & BYTE_MASK), (float)(from->texcoord >> BYTE_SHIFT));
        corners[index].depth = depth[corner];
    }
    emit(&look, corners, count == TRIANGLE_VERTICES ? TRIANGLE_VERTICES : QUAD_TRIANGLE_VERTICES);
}

/* A quad's six vertices from its corners: top left, top right, bottom left, bottom right */
static void quad(const Shading *look, const float *x, const float *y, const uint32_t *colour, const float *u,
                 const float *v) {
    DrawVertex corners[QUAD_TRIANGLE_VERTICES];
    int index;

    for (index = 0; index < QUAD_TRIANGLE_VERTICES; index++) {
        int corner = s_QuadOrder[index];

        corners[index] = draw_vertex(look, x[corner], y[corner], colour[corner], u[corner], v[corner]);
    }
    emit(look, corners, QUAD_TRIANGLE_VERTICES);
}

/* Each segment as a quad a pixel across its minor axis, its last pixel included as the GPU draws it */
void Render_Line(const RasterState *state, int count, const RasterVertex *vertices) {
    static const float untextured[QUAD_CORNERS] = {0};
    Shading look;
    int index;

    if (!Render_Drawing()) {
        return;
    } else {
        look = shading(state);
    }
    for (index = 0; index + 1 < count; index++) {
        const RasterVertex *a = &vertices[index];
        const RasterVertex *b = &vertices[index + 1];
        float x[QUAD_CORNERS], y[QUAD_CORNERS];
        uint32_t colour[QUAD_CORNERS];
        int dx = b->x - a->x;
        int dy = b->y - a->y;

        colour[0] = a->color;
        colour[1] = b->color;
        colour[2] = a->color;
        colour[3] = b->color;
        if (SDL_abs(dx) >= SDL_abs(dy)) {
            x[0] = (float)a->x + (dx >= 0 ? 0.0f : 1.0f);
            x[1] = (float)b->x + (dx >= 0 ? 1.0f : 0.0f);
            x[2] = x[0];
            x[3] = x[1];
            y[0] = (float)a->y;
            y[1] = (float)b->y;
            y[2] = y[0] + 1.0f;
            y[3] = y[1] + 1.0f;
        } else {
            y[0] = (float)a->y + (dy >= 0 ? 0.0f : 1.0f);
            y[1] = (float)b->y + (dy >= 0 ? 1.0f : 0.0f);
            y[2] = y[0];
            y[3] = y[1];
            x[0] = (float)a->x;
            x[1] = (float)b->x;
            x[2] = x[0] + 1.0f;
            x[3] = x[1] + 1.0f;
        }
        quad(&look, x, y, colour, untextured, untextured);
    }
}

/* A rectangle's texels one to a pixel from its corner's, backwards along a flipped axis */
void Render_Rectangle(const RasterState *state, int32_t x, int32_t y, uint16_t width, uint16_t height,
                      uint16_t texcoord) {
    float xs[QUAD_CORNERS], ys[QUAD_CORNERS], us[QUAD_CORNERS], vs[QUAD_CORNERS];
    uint32_t colour[QUAD_CORNERS];
    float u = (float)(texcoord & BYTE_MASK);
    float v = (float)(texcoord >> BYTE_SHIFT);
    float u_end = u + (float)width;
    float v_end = v + (float)height;
    Shading look;
    int index;

    if (!Render_Drawing()) {
        return;
    } else {
        look = shading(state);
    }
    if (state->draw_mode & FLIP_X) {
        u += FLIP_START;
        u_end = u - (float)width;
    } else {
        /* Forwards */
    }
    if (state->draw_mode & FLIP_Y) {
        v += FLIP_START;
        v_end = v - (float)height;
    } else {
        /* Forwards */
    }
    for (index = 0; index < QUAD_CORNERS; index++) {
        int right = index & 1;
        int bottom = index >> 1;

        xs[index] = (float)(x + (right ? width : 0));
        ys[index] = (float)(y + (bottom ? height : 0));
        us[index] = right ? u_end : u;
        vs[index] = bottom ? v_end : v;
        colour[index] = state->command;
    }
    quad(&look, xs, ys, colour, us, vs);
}

/* A rectangle of VRAM's, clipped to it, as the target's scissor */
static SDL_Rect vram_scissor(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    uint32_t right = SDL_min(x + width, (uint32_t)VRAM_WIDTH);
    uint32_t bottom = SDL_min(y + height, (uint32_t)VRAM_HEIGHT);

    return scaled((int)x, (int)y, (int)(right - x), (int)(bottom - y));
}

/* A rectangle of VRAM's drawn into the target, its colour or (absolute) the software VRAM's pixels */
static void vram_rectangle(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t colour, uint32_t flags) {
    SDL_Rect scissor = vram_scissor(x, y, width, height);
    Shading look;
    DrawVertex *out;
    int index;

    SDL_zero(look);
    look.flags = flags;
    look.pipeline = PIPELINE_OPAQUE;
    out = reserve(QUAD_TRIANGLE_VERTICES, PIPELINE_OPAQUE, &scissor);
    for (index = 0; index < QUAD_TRIANGLE_VERTICES; index++) {
        int corner = s_QuadOrder[index];
        float left = (float)x;
        float top = (float)y;
        float px = left + ((corner & 1) ? (float)width : 0.0f);
        float py = top + ((corner >> 1) ? (float)height : 0.0f);

        out[index] = draw_vertex(&look, px, py, colour, px, py);
    }
}

void Render_Fill(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t color) {
    if (!Render_Drawing()) {
        return;
    } else {
        s_VramChanged = 1;
        vram_rectangle(x, y, width, height, color, 0);
    }
}

void Render_Copy(uint32_t from_x, uint32_t from_y, uint32_t to_x, uint32_t to_y, uint32_t width, uint32_t height) {
    Step *step;

    if (!Render_Drawing()) {
        return;
    } else {
        s_VramChanged = 1;
        make_room(0, 1, 0);
        step = add_step(STEP_COPY);
    }
    /* Within VRAM: the GPU's wrapping at its edges left out */
    step->from_x = (int)from_x;
    step->from_y = (int)from_y;
    step->to_x = (int)to_x;
    step->to_y = (int)to_y;
    step->width = (int)SDL_min(width, SDL_min((uint32_t)VRAM_WIDTH - from_x, (uint32_t)VRAM_WIDTH - to_x));
    step->height = (int)SDL_min(height, SDL_min((uint32_t)VRAM_HEIGHT - from_y, (uint32_t)VRAM_HEIGHT - to_y));
}

void Render_Load(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    if (!Render_Drawing()) {
        return;
    } else {
        s_VramChanged = 1;
        take_snapshot();
        vram_rectangle(x, y, width, height, 0, FLAG_ABSOLUTE);
    }
}

static void release_target(void) {
    if (s_Target != NULL) {
        SDL_ReleaseGPUTexture(s_Device, s_Target);
        SDL_ReleaseGPUTexture(s_Device, s_Copy);
        s_Target = NULL;
        s_Copy = NULL;
    } else {
        /* None made */
    }
}

void Render_ApplySettings(void) {
    int scale = Host_ResolutionScale();

    if (s_Device == NULL || scale == s_Scale) {
        return;
    } else {
        flush();
        release_target();
        s_Scale = scale;
    }
    if (scale > 1) {
        SDL_GPUTextureUsageFlags usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;

        s_Target = texture(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, usage, VRAM_WIDTH * scale, VRAM_HEIGHT * scale);
        s_Copy = texture(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, usage, VRAM_WIDTH * scale, VRAM_HEIGHT * scale);
    } else {
        /* The software picture */
        return;
    }
    if (s_Target == NULL || s_Copy == NULL) {
        Host_Log("no target at %d times the resolution (%s): the PlayStation's", scale, SDL_GetError());
        release_target();
        s_Scale = 1;
    } else {
        /* The target starts as the software VRAM */
        s_VramChanged = 1;
        Render_Load(0, 0, VRAM_WIDTH, VRAM_HEIGHT);
    }
}

/* The software picture's pixels into its texture */
static void upload_picture(SDL_GPUCommandBuffer *command, int width, int height, const uint32_t *pixels) {
    unsigned char *mapped = SDL_MapGPUTransferBuffer(s_Device, s_PictureTransfer, true);
    SDL_GPUCopyPass *copy;
    SDL_GPUTextureTransferInfo from;
    SDL_GPUTextureRegion to;

    if (mapped == NULL) {
        Host_Fatal("the GPU renderer's picture: %s", SDL_GetError());
    } else {
        memcpy(mapped, pixels, (size_t)width * (size_t)height * PICTURE_PIXEL_BYTES);
        SDL_UnmapGPUTransferBuffer(s_Device, s_PictureTransfer);
    }
    copy = SDL_BeginGPUCopyPass(command);
    SDL_zero(from);
    from.transfer_buffer = s_PictureTransfer;
    from.pixels_per_row = (Uint32)width;
    from.rows_per_layer = (Uint32)height;
    SDL_zero(to);
    to.texture = s_Picture;
    to.w = (Uint32)width;
    to.h = (Uint32)height;
    to.d = 1;
    SDL_UploadToGPUTexture(copy, &from, &to, true);
    SDL_EndGPUCopyPass(copy);
}

/* The window's viewport for the picture: 4:3 (or 16:9 for the wide view and the zoomed one) in the middle, or all of
 * it stretched */
static SDL_GPUViewport viewport(Uint32 width, Uint32 height) {
    SDL_GPUViewport result;
    int wide = Host_Setting(SETTING_SHAPE) == SHAPE_WIDE || Host_Setting(SETTING_SHAPE) == SHAPE_ZOOMED;
    Uint32 aspect_width = wide ? WIDE_ASPECT_WIDTH : ASPECT_WIDTH;
    Uint32 aspect_height = wide ? WIDE_ASPECT_HEIGHT : ASPECT_HEIGHT;

    SDL_zero(result);
    result.max_depth = 1.0f;
    if (Host_Setting(SETTING_SHAPE) == SHAPE_STRETCHED) {
        result.w = (float)width;
        result.h = (float)height;
    } else if (width * aspect_height > height * aspect_width) {
        result.h = (float)height;
        result.w = (float)height * aspect_width / aspect_height;
        result.x = ((float)width - result.w) * HALF;
    } else {
        result.w = (float)width;
        result.h = (float)width * aspect_height / aspect_width;
        result.y = ((float)height - result.h) * HALF;
    }
    return result;
}

void Render_Present(int x, int y, int width, int height, int enabled, const uint32_t *pixels) {
    SDL_GPUCommandBuffer *command;
    SDL_GPUTexture *swapchain = NULL;
    SDL_GPUTextureSamplerBinding source;
    SDL_GPUColorTargetInfo target;
    SDL_GPUViewport view;
    Uint32 swapchain_width, swapchain_height;
    float rectangle[UNIFORM_FLOATS];

    if (s_Device == NULL) {
        return;
    } else if (s_Fences[s_Frame] != NULL) {
        SDL_WaitForGPUFences(s_Device, true, &s_Fences[s_Frame], 1);
        SDL_ReleaseGPUFence(s_Device, s_Fences[s_Frame]);
        s_Fences[s_Frame] = NULL;
    } else {
        /* None in flight in this slot */
    }
    command = SDL_AcquireGPUCommandBuffer(s_Device);
    if (command == NULL) {
        Host_Fatal("the GPU renderer's command buffer: %s", SDL_GetError());
    } else {
        encode(command);
    }
    if (Render_Drawing()) {
        source.texture = s_Target;
        rectangle[0] = (float)x / VRAM_WIDTH;
        rectangle[1] = (float)y / VRAM_HEIGHT;
        rectangle[2] = (float)width / VRAM_WIDTH;
        rectangle[3] = (float)height / VRAM_HEIGHT;
    } else {
        upload_picture(command, width, height, pixels);
        source.texture = s_Picture;
        rectangle[0] = 0.0f;
        rectangle[1] = 0.0f;
        rectangle[2] = (float)width / PICTURE_WIDTH;
        rectangle[3] = (float)height / PICTURE_HEIGHT;
    }
    if (Host_Setting(SETTING_SHAPE) == SHAPE_ZOOMED) {
        /* The middle of the picture's height */
        float shown = rectangle[3] * ZOOM_SHOWN_NUMERATOR / ZOOM_SHOWN_DENOMINATOR;

        rectangle[1] += (rectangle[3] - shown) * HALF;
        rectangle[3] = shown;
    } else {
        /* All of it */
    }
    source.sampler = Host_Setting(SETTING_SMOOTH) ? s_Linear : s_Nearest;
    if (s_Window != NULL &&
        !SDL_WaitAndAcquireGPUSwapchainTexture(command, s_Window, &swapchain, &swapchain_width, &swapchain_height)) {
        Host_Fatal("the window's picture: %s", SDL_GetError());
    } else if (swapchain != NULL) {
        SDL_GPURenderPass *pass;

        SDL_zero(target);
        target.texture = swapchain;
        target.load_op = SDL_GPU_LOADOP_CLEAR;
        target.store_op = SDL_GPU_STOREOP_STORE;
        pass = SDL_BeginGPURenderPass(command, &target, 1, NULL);
        if (enabled) {
            view = viewport(swapchain_width, swapchain_height);
            SDL_SetGPUViewport(pass, &view);
            SDL_BindGPUGraphicsPipeline(pass, s_Present);
            SDL_BindGPUFragmentSamplers(pass, 0, &source, 1);
            SDL_PushGPUVertexUniformData(command, 0, rectangle, sizeof(rectangle));
            SDL_DrawGPUPrimitives(pass, PRESENT_VERTICES, 1, 0, 0);
        } else {
            /* The display off: black */
        }
        SDL_EndGPURenderPass(pass);
    } else {
        /* No window, or none to draw into now (minimized) */
    }
    s_Fences[s_Frame] = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
    s_Frame = (s_Frame + 1) % FRAMES_IN_FLIGHT;
}

void Render_Shot(const char *path) {
    int x, y, width, height, enabled;
    SDL_GPUTransferBuffer *download;
    SDL_GPUCommandBuffer *command;
    SDL_GPUCopyPass *copy;
    SDL_GPUTextureRegion from;
    SDL_GPUTextureTransferInfo to;
    SDL_GPUFence *fence;
    const unsigned char *pixels;
    FILE *file;
    int row, column;

    if (!Render_Drawing()) {
        return;
    } else {
        Host_DisplayArea(&x, &y, &width, &height, &enabled);
        flush();
    }
    width *= s_Scale;
    height *= s_Scale;
    download = transfer(SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, (Uint32)(width * height * TARGET_PIXEL_BYTES));
    command = SDL_AcquireGPUCommandBuffer(s_Device);
    if (download == NULL || command == NULL) {
        Host_Fatal("the GPU renderer's shot: %s", SDL_GetError());
    } else {
        copy = SDL_BeginGPUCopyPass(command);
    }
    SDL_zero(from);
    from.texture = s_Target;
    from.x = (Uint32)(x * s_Scale);
    from.y = (Uint32)(y * s_Scale);
    from.w = (Uint32)width;
    from.h = (Uint32)height;
    from.d = 1;
    SDL_zero(to);
    to.transfer_buffer = download;
    SDL_DownloadFromGPUTexture(copy, &from, &to);
    SDL_EndGPUCopyPass(copy);
    fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
    SDL_WaitForGPUFences(s_Device, true, &fence, 1);
    SDL_ReleaseGPUFence(s_Device, fence);
    pixels = SDL_MapGPUTransferBuffer(s_Device, download, false);
    file = fopen(path, "wb");
    if (pixels == NULL || file == NULL) {
        Host_Fatal("can't write %s", path);
    } else {
        fprintf(file, "P6\n%d %d\n255\n", width, height);
    }
    for (row = 0; row < height; row++) {
        for (column = 0; column < width; column++) {
            fwrite(pixels + (row * width + column) * TARGET_PIXEL_BYTES, RGB_BYTES, 1, file);
        }
    }
    fclose(file);
    SDL_UnmapGPUTransferBuffer(s_Device, download);
    SDL_ReleaseGPUTransferBuffer(s_Device, download);
}
