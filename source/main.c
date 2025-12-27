#include <gccore.h>
#include <wiiuse/wpad.h>
#include <ogc/lwp_watchdog.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <math.h>
#include <asndlib.h>
#include "scene2.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// --- Embedded Data ---
extern const u8 sprites_png[];
extern const u8 sprites_png_end[];
extern const u8 coords_txt[];
extern const u8 coords_txt_end[];
extern const u8 startup_raw[];
extern const u8 startup_raw_end[];

void *frameBuffer[2] = { NULL, NULL };
u32 fbIndex = 0;

// --- Helper Structs ---
typedef struct {
    float x, y, w, h;     // Position on screen (relative or absolute)
    float u0, v0, u1, v1; // Texture Coordinates
} Sprite;

// --- Tiling ---
// Convert Linear RGBA to 4x4 Tiled RGBA
// Returns the padded buffer. Puts aligned width/height into args if needed?
// GX works best if the input texture is already aligned.
u8* ConvertToTiled(u8* src, int w, int h, int *outW, int *outH) {
    // Wii textures (RGBA8) are made of 4x4 tiles.
    // Width and Height of the buffer must be aligned to 4.
    int alignedW = (w + 3) & ~3;
    int alignedH = (h + 3) & ~3;
    
    if (outW) *outW = alignedW;
    if (outH) *outH = alignedH;

    u32 texSize = alignedW * alignedH * 4;
    u8 *dst = (u8*)memalign(32, texSize);
    memset(dst, 0, texSize); // clear padding to 0
    u8 *d = dst;
    
    // Wii RGBA8 Format (Format 6):
    // Tiles are 4x4 pixels.
    // Each tile is 64 bytes.
    // Bytes 0-31: AR data (16 pixels * 2 bytes) -> A, R, A, R...
    // Bytes 32-63: GB data (16 pixels * 2 bytes) -> G, B, G, B...
    
    for (int by=0; by < alignedH; by+=4) {
        for (int bx=0; bx < alignedW; bx+=4) {
             
             // Pass 1: Write AR data for the 16 pixels
             for (int y=0; y<4; y++) {
                 for (int x=0; x<4; x++) {
                     if ((bx+x) < w && (by+y) < h) {
                         int srcIdx = ((by+y)*w + (bx+x)) * 4;
                         *d++ = src[srcIdx+3]; // Alpha
                         *d++ = src[srcIdx+0]; // Red
                     } else {
                         *d++ = 0; *d++ = 0; 
                     }
                 }
             }
             
             // Pass 2: Write GB data for the 16 pixels
             for (int y=0; y<4; y++) {
                 for (int x=0; x<4; x++) {
                     if ((bx+x) < w && (by+y) < h) {
                         int srcIdx = ((by+y)*w + (bx+x)) * 4;
                         *d++ = src[srcIdx+1]; // Green
                         *d++ = src[srcIdx+2]; // Blue
                     } else {
                         *d++ = 0; *d++ = 0; 
                     }
                 }
             }
        }
    }
    DCFlushRange(dst, texSize); // CRITICAL: Flush cache so GPU sees the data
    return dst;
}

int main(int argc, char **argv) {
    VIDEO_Init();
    WPAD_Init();
    ASND_Init();
    ASND_Pause(0); // Ensure sound is unpaused
    
    // Play Startup Sound (Raw PCM 16-bit Stereo 48kHz)
    u32 rawSize = startup_raw_end - startup_raw;
    ASND_SetVoice(ASND_GetFirstUnusedVoice(), VOICE_STEREO_16BIT, 48000, 0, 
                  (u8*)startup_raw, rawSize, 255, 255, NULL);
    
    GXRModeObj *rmode = VIDEO_GetPreferredMode(NULL);
    frameBuffer[0] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
    frameBuffer[1] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
    VIDEO_Configure(rmode);
    VIDEO_SetNextFramebuffer(frameBuffer[0]);
    VIDEO_SetBlack(FALSE);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if(rmode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();

    void *gp_fifo = memalign(32, 256 * 1024);
    memset(gp_fifo, 0, 256 * 1024);
    GX_Init(gp_fifo, 256 * 1024);

    GX_SetCopyClear((GXColor){255, 255, 255, 255}, GX_MAX_Z24);
    GX_SetViewport(0,0,rmode->fbWidth,rmode->efbHeight,0,1);
    f32 yscale = GX_GetYScaleFactor(rmode->efbHeight, rmode->xfbHeight);
    u32 xfbHeight = GX_SetDispCopyYScale(yscale);
    GX_SetScissor(0,0,rmode->fbWidth,rmode->efbHeight);
    GX_SetDispCopySrc(0,0,rmode->fbWidth,rmode->efbHeight);
    GX_SetDispCopyDst(rmode->fbWidth,xfbHeight);
    GX_SetCopyFilter(rmode->aa,rmode->sample_pattern,GX_TRUE,rmode->vfilter);
    GX_SetFieldMode(rmode->field_rendering,((rmode->viHeight==2*rmode->xfbHeight)?GX_ENABLE:GX_DISABLE));
    GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
    GX_SetCullMode(GX_CULL_NONE);
    GX_SetDispCopyGamma(GX_GM_1_0);

    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    
    GX_SetNumChans(1);
    GX_SetNumTexGens(1);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
    GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);

    Mtx44 ortho;
    guOrtho(ortho, 0, 224, 0, 256, 0, 10);
    GX_LoadProjectionMtx(ortho, GX_ORTHOGRAPHIC);

    Mtx modelview;
    guMtxIdentity(modelview);
    guMtxTransApply(modelview, modelview, 0, 0, -5.0f);
    GX_LoadPosMtxImm(modelview, GX_PNMTX0);

    // --- Load Sprite Sheet ---
    int imgW, imgH, imgCh;
    // stbi_load_from_memory expects int len
    int pngSize = sprites_png_end - sprites_png;
    u8 *linearData = stbi_load_from_memory(sprites_png, pngSize, &imgW, &imgH, &imgCh, 4); // Force 4 channels (RGBA)
    
    if (!linearData) {
        // Handle Error? Just default to NULL mapping
    }
    
    // Convert to Tiled
    u8 *tiledData = NULL;
    GXTexObj texObj;
    int alignedW = 0, alignedH = 0;
    
    if (linearData) {
        tiledData = ConvertToTiled(linearData, imgW, imgH, &alignedW, &alignedH);
        GX_InitTexObj(&texObj, tiledData, alignedW, alignedH, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
        GX_InitTexObjFilterMode(&texObj, GX_NEAR, GX_NEAR); 
        stbi_image_free(linearData);
    }
    
    // --- Parse Coords ---
    // Format: "Name X Y W H"
    Sprite sprites[3];  // Support 3 sprites now
    memset(sprites, 0, sizeof(sprites));
    int spriteCount = 0;
    
    // Create a mutable copy of the coord string
    int txtSize = coords_txt_end - coords_txt;
    char *txtBuffer = malloc(txtSize + 1);
    memcpy(txtBuffer, coords_txt, txtSize);
    txtBuffer[txtSize] = 0;
    
    // Use pointer arithmetic to traverse lines
    char *cursor = txtBuffer;
    while(cursor && *cursor && spriteCount < 3) {  // Changed to 3
        // Find end of line
        char *eol = strpbrk(cursor, "\r\n");
        if (eol) *eol = 0; // Terminate line
        
        if (strlen(cursor) > 0) {
            char name[64];
            int x, y, w, h;
            // Attempt parse
            int count = sscanf(cursor, "%63s %d %d %d %d", name, &x, &y, &w, &h);
            if (count == 5) {
                // Valid Line
                float safeW = (float)(alignedW > 0 ? alignedW : imgW);
                float safeH = (float)(alignedH > 0 ? alignedH : imgH);
                
                sprites[spriteCount].x = (float)x;
                sprites[spriteCount].y = (float)y;
                sprites[spriteCount].w = (float)w;
                sprites[spriteCount].h = (float)h;
                
                // CRITICAL: Texture V coordinates are flipped!
                // In the image: Y=0 is at TOP
                // In texture coords: V=0 is at BOTTOM
                // So we need to flip: V = (imgH - Y) / alignedH
                
                // Also flip vertically by swapping V coordinates
                sprites[spriteCount].u0 = (float)x / safeW;
                sprites[spriteCount].v0 = (float)(imgH - y) / safeH;        // Swapped for vertical flip
                sprites[spriteCount].u1 = (float)(x + w) / safeW;
                sprites[spriteCount].v1 = (float)(imgH - y - h) / safeH;    // Swapped for vertical flip
                
                spriteCount++;
            }
        }
        
        // Move to next line
        if (eol) cursor = eol + 1;
        else break;
        
        // Skip extra newlines
        while (*cursor == '\r' || *cursor == '\n') cursor++;
    }
    free(txtBuffer);
    
    // Positions:
    // Rhombus Center: 128, 112.
    // Sprite 1 (Top): Above Rhombus.
    // Sprite 2 (Bot): Below Rhombus.
    
    // Let's set absolute positions
    // Screen is 256x224.
    // Top Sprite at (128, 40) center?
    // Bot Sprite at (128, 190) center?
    
    // Animation timing
    u64 startTime = gettime();
    u64 ticksPerSecond = TB_TIMER_CLOCK;
    
    // Helper to get elapsed seconds
    #define GET_ELAPSED_SEC() (ticks_to_millisecs(diff_ticks(startTime, gettime())) / 1000.0f)
    
    
    while(1) {
        WPAD_ScanPads();
        if (WPAD_ButtonsHeld(0) & WPAD_BUTTON_HOME) break;

        
        float elapsed = GET_ELAPSED_SEC();
        
        // Determine background color based on act
        GXColor bgColor;
        float fadeAlpha = 1.0f;
        
        if (elapsed < 4.0f) {
            // Act 1: Black screen (0-4s)
            if (elapsed < 0.1f) {
            }
            bgColor = (GXColor){0, 0, 0, 255};
        } else if (elapsed < 5.0f) {
            // Fade to white (4-5s)
            if (elapsed < 4.1f) {
            }
            float fade = (elapsed - 4.0f) / 1.0f;  // 0 to 1
            u8 val = (u8)(fade * 255.0f);
            bgColor = (GXColor){val, val, val, 255};
        } else {
            // White background (5s+)
            if (elapsed < 5.1f) {
            }
            bgColor = (GXColor){255, 255, 255, 255};
        }
        
        // Apply fade out at end (Act 4: 11s+)
        // Apply fade out at end (Act 4: 11s+)
        if (elapsed >= 11.0f) {
            float fadeOut = (elapsed - 11.0f) / 1.0f;  // 1 second fade (faster)
            if (fadeOut > 1.0f) fadeOut = 1.0f;
            fadeAlpha = 1.0f - fadeOut;
            // Fade to black
            u8 val = (u8)((1.0f - fadeOut) * 255.0f);
            bgColor = (GXColor){val, val, val, 255};
            
            // Transition to next scene
            if (elapsed >= 12.0f) {
                RunScene2(startTime);
                break;  // Exit after scene 2
            }
        }
        
        // Clear with current background
        GX_SetCopyClear(bgColor, GX_MAX_Z24);
        
        GX_SetVtxDesc(GX_VA_TEX0, GX_NONE);
        GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
        
        // Act 2: Draw Rhombus (5-6s and beyond)
        if (elapsed >= 5.0f && elapsed < 11.0f) {
            float cx = 128.0f, cy = 112.0f, sz = 50.0f;
            
            // Fade in rhombus (5-6s)
            u8 alpha = 255;
            if (elapsed < 6.0f) {
                float fade = (elapsed - 5.0f) / 1.0f;
                alpha = (u8)(fade * 255.0f);
            }
            
            GX_Begin(GX_TRIANGLES, GX_VTXFMT0, 12);
                // Triangle 1: Center -> Right -> Top
                GX_Position3f32(cx, cy, 0.0f);     GX_Color4u8(255, 255, 0, alpha);  // Center: Yellow
                GX_Position3f32(cx+sz, cy, 0.0f);  GX_Color4u8(255, 0, 0, alpha);    // Right: Red
                GX_Position3f32(cx, cy-sz, 0.0f);  GX_Color4u8(255, 255, 0, alpha);  // Top: Yellow
                
                // Triangle 2: Center -> Top -> Left
                GX_Position3f32(cx, cy, 0.0f);     GX_Color4u8(255, 255, 0, alpha);  // Center: Yellow
                GX_Position3f32(cx, cy-sz, 0.0f);  GX_Color4u8(255, 255, 0, alpha);  // Top: Yellow
                GX_Position3f32(cx-sz, cy, 0.0f);  GX_Color4u8(255, 0, 0, alpha);    // Left: Red
                
                // Triangle 3: Center -> Left -> Bottom
                GX_Position3f32(cx, cy, 0.0f);     GX_Color4u8(255, 255, 0, alpha);  // Center: Yellow
                GX_Position3f32(cx-sz, cy, 0.0f);  GX_Color4u8(255, 0, 0, alpha);    // Left: Red
                GX_Position3f32(cx, cy+sz, 0.0f);  GX_Color4u8(255, 255, 0, alpha);  // Bottom: Yellow
                
                // Triangle 4: Center -> Bottom -> Right
                GX_Position3f32(cx, cy, 0.0f);     GX_Color4u8(255, 255, 0, alpha);  // Center: Yellow
                GX_Position3f32(cx, cy+sz, 0.0f);  GX_Color4u8(255, 255, 0, alpha);  // Bottom: Yellow
                GX_Position3f32(cx+sz, cy, 0.0f);  GX_Color4u8(255, 0, 0, alpha);    // Right: Red
            GX_End();
            
            // PS1 BIOS authentic animation (5-6s)
            // Rhombus splits in half vertically, halves move INWARD to center
            if (elapsed >= 5.0f && elapsed < 11.0f) {
                float animTime = (elapsed - 5.0f);
                if (animTime > 1.0f) animTime = 1.0f;
                
                // Horizontal separation: start even closer to center (15% of sz), move INWARD to center (0)
                float horizontalSep = (sz * 0.15f) * (1.0f - animTime);  // Start even closer
                
                // Shrink even more while moving inward
                float scale = 1.0f - (animTime * 0.6f);  // Shrink to 40% (even smaller)
                float triSize = sz * scale;
                
                // Vertical S-shape offset
                float verticalOffset = 15.0f * animTime;
                
                // Fade effect
                u8 triAlpha = (u8)(255 * (1.0f - animTime * 0.3f));
                
                // LEFT HALF - moves left and UP
                float leftX = cx - horizontalSep;
                float leftY = cy - verticalOffset;
                GX_Begin(GX_TRIANGLES, GX_VTXFMT0, 6);
                    // Top-left triangle
                    GX_Position3f32(leftX, leftY, 0.0f);           GX_Color4u8(255, 255, 0, triAlpha);
                    GX_Position3f32(leftX, leftY - triSize, 0.0f); GX_Color4u8(255, 255, 0, triAlpha);
                    GX_Position3f32(leftX - triSize, leftY, 0.0f); GX_Color4u8(255, 0, 0, triAlpha);
                    
                    // Bottom-left triangle
                    GX_Position3f32(leftX, leftY, 0.0f);           GX_Color4u8(255, 255, 0, triAlpha);
                    GX_Position3f32(leftX - triSize, leftY, 0.0f); GX_Color4u8(255, 0, 0, triAlpha);
                    GX_Position3f32(leftX, leftY + triSize, 0.0f); GX_Color4u8(255, 255, 0, triAlpha);
                GX_End();
                
                // RIGHT HALF - moves right and DOWN
                float rightX = cx + horizontalSep;
                float rightY = cy + verticalOffset;
                GX_Begin(GX_TRIANGLES, GX_VTXFMT0, 6);
                    // Top-right triangle
                    GX_Position3f32(rightX, rightY, 0.0f);           GX_Color4u8(255, 255, 0, triAlpha);
                    GX_Position3f32(rightX + triSize, rightY, 0.0f); GX_Color4u8(255, 0, 0, triAlpha);
                    GX_Position3f32(rightX, rightY - triSize, 0.0f); GX_Color4u8(255, 255, 0, triAlpha);
                    
                    // Bottom-right triangle
                    GX_Position3f32(rightX, rightY, 0.0f);           GX_Color4u8(255, 255, 0, triAlpha);
                    GX_Position3f32(rightX, rightY + triSize, 0.0f); GX_Color4u8(255, 255, 0, triAlpha);
                    GX_Position3f32(rightX + triSize, rightY, 0.0f); GX_Color4u8(255, 0, 0, triAlpha);
                GX_End();
            }
        }
        
        // Act 3: Draw Sprites (6-11s)
        if (elapsed >= 6.0f && elapsed < 11.0f && tiledData) {
            GX_LoadTexObj(&texObj, GX_TEXMAP0);
            GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
            GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
            
            // Fade in sprites (6-7s)
            u8 spriteAlpha = 255;
            if (elapsed < 7.0f) {
                float fade = (elapsed - 6.0f) / 1.0f;
                spriteAlpha = (u8)(fade * 255.0f);
            }
            
            // Draw all 3 sprites
            if (spriteCount > 0) {
                Sprite *s = &sprites[0];
                float scale = 0.5f;
                float displayW = s->w * scale;
                float displayH = s->h * scale;
                float sx = 128.0f - (displayW / 2.0f);
                float sy = 30.0f;
                
                GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
                    GX_Position3f32(sx, sy, 0);                 GX_Color4u8(255,255,255,spriteAlpha); GX_TexCoord2f32(s->u0, s->v0);
                    GX_Position3f32(sx+displayW, sy, 0);        GX_Color4u8(255,255,255,spriteAlpha); GX_TexCoord2f32(s->u1, s->v0);
                    GX_Position3f32(sx+displayW, sy+displayH, 0);GX_Color4u8(255,255,255,spriteAlpha); GX_TexCoord2f32(s->u1, s->v1);
                    GX_Position3f32(sx, sy+displayH, 0);        GX_Color4u8(255,255,255,spriteAlpha); GX_TexCoord2f32(s->u0, s->v1);
                GX_End();
            }
            
            if (spriteCount > 1) {
                Sprite *s = &sprites[1];
                float scale = 0.5f;
                float displayW = s->w * scale;
                float displayH = s->h * scale;
                float sx = 128.0f - (displayW / 2.0f);
                float sy = 180.0f;
                
                GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
                    GX_Position3f32(sx, sy, 0);                 GX_Color4u8(255,255,255,spriteAlpha); GX_TexCoord2f32(s->u0, s->v0);
                    GX_Position3f32(sx+displayW, sy, 0);        GX_Color4u8(255,255,255,spriteAlpha); GX_TexCoord2f32(s->u1, s->v0);
                    GX_Position3f32(sx+displayW, sy+displayH, 0);GX_Color4u8(255,255,255,spriteAlpha); GX_TexCoord2f32(s->u1, s->v1);
                    GX_Position3f32(sx, sy+displayH, 0);        GX_Color4u8(255,255,255,spriteAlpha); GX_TexCoord2f32(s->u0, s->v1);
                GX_End();
            }
            
            if (spriteCount > 2) {
                Sprite *s = &sprites[2];
                float scale = 0.5f;
                float displayW = s->w * scale;
                float displayH = s->h * scale;
                float sx = 128.0f - (displayW / 2.0f) + 20.0f;
                float sy = 165.0f;
                
                GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
                    GX_Position3f32(sx, sy, 0);                 GX_Color4u8(255,255,255,spriteAlpha); GX_TexCoord2f32(s->u0, s->v0);
                    GX_Position3f32(sx+displayW, sy, 0);        GX_Color4u8(255,255,255,spriteAlpha); GX_TexCoord2f32(s->u1, s->v0);
                    GX_Position3f32(sx+displayW, sy+displayH, 0);GX_Color4u8(255,255,255,spriteAlpha); GX_TexCoord2f32(s->u1, s->v1);
                    GX_Position3f32(sx, sy+displayH, 0);        GX_Color4u8(255,255,255,spriteAlpha); GX_TexCoord2f32(s->u0, s->v1);
                GX_End();
            }
        }

        GX_DrawDone();
        GX_CopyDisp(frameBuffer[fbIndex], GX_TRUE);
        VIDEO_SetNextFramebuffer(frameBuffer[fbIndex]);
        VIDEO_Flush();
        VIDEO_WaitVSync();
        fbIndex ^= 1;
    }
    
    if(tiledData) free(tiledData);
    
    return 0;
}