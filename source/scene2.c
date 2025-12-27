#include <gccore.h>
#include <wiiuse/wpad.h>
#include <ogc/lwp_watchdog.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <math.h>

#include "stb_image.h"

extern void *frameBuffer[2];
extern u32 fbIndex;

// --- Extern from main.c ---
extern u8* ConvertToTiled(u8 *src, int w, int h, int *outW, int *outH);

// --- Embedded Data ---
extern const u8 ps_obj[];
extern const u8 ps_obj_end[];
extern const u8 sprites_png[];
extern const u8 sprites_png_end[];
extern const u8 scene2_txt[];
extern const u8 scene2_txt_end[];

// --- 3D Constants ---
#define MAX_VERTS 2048
#define DL_SIZE (256 * 1024)

typedef struct {
    f32 x, y, z;
} Vertex;

Vertex vertices[MAX_VERTS];
typedef struct { f32 x, y, z; } Normal;
Normal normals[MAX_VERTS];
int vertCount = 0;
int normCount = 0;
void *ps1DL = NULL;
u32 ps1DLSize = 0; // Store actual display list size

// --- Sprite Data (old, unused) ---
GXTexObj spriteTexObj_old;
typedef struct { int x, y, w, h; } SpriteRegion;
SpriteRegion spriteRegions[4];
int spriteCount = 0;

// --- Sprite Data (new, for single sprite) ---
GXTexObj spriteTexObj;
SpriteRegion spriteRegion; // First sprite
SpriteRegion spriteRegion2; // Second sprite
SpriteRegion spriteRegion3; // Third sprite
SpriteRegion spriteRegion4; // Fourth sprite
int spriteTexW = 0, spriteTexH = 0;
// --- Helper: Parse OBJ and Create Display List ---
void ParseAndLoadModel() {
    printf("Parsing OBJ...\n");
    
    // Allocate Display List memory
    ps1DL = memalign(32, DL_SIZE);
    if (!ps1DL) {
        return;
    }
    memset(ps1DL, 0, DL_SIZE);
    
    DCInvalidateRange(ps1DL, DL_SIZE);
    GX_BeginDispList(ps1DL, DL_SIZE);
    
    // Default Color (White)
    GXColor curColor = (GXColor){255, 255, 255, 255};
    
    // Parse the embedded OBJ file
    u32 size = ps_obj_end - ps_obj;
    char *txt = (char*)malloc(size + 1);
    if (!txt) {
        if (ps1DL) free(ps1DL);
        ps1DL = NULL;
        return;
    }
    
    // --- Pass 1: Parse Vertices and Count Face Vertices ---
    memcpy(txt, ps_obj, size);
    txt[size] = 0;
    
    char *lineStart = txt;
    int totalDrawVerts = 0;
    
    while(lineStart < txt + size) {
        char *eol = strchr(lineStart, '\n');
        if (!eol) eol = txt + size;
        *eol = 0;
        
        // Trim carriage return
        int len = strlen(lineStart);
        if (len > 0 && lineStart[len-1] == '\r') lineStart[len-1] = 0;
        
        if (lineStart[0] == 'v' && lineStart[1] == ' ') {
            if (vertCount < MAX_VERTS) {
                float vx, vy, vz;
                sscanf(lineStart + 2, "%f %f %f", &vx, &vy, &vz);
                vertices[vertCount].x = vx;
                vertices[vertCount].y = vy;
                vertices[vertCount].z = vz;
                vertCount++;
            }
        }
        else if (lineStart[0] == 'v' && lineStart[1] == 'n') {
            if (normCount < MAX_VERTS) {
                float nx, ny, nz;
                sscanf(lineStart + 3, "%f %f %f", &nx, &ny, &nz);
                normals[normCount].x = nx;
                normals[normCount].y = ny;
                normals[normCount].z = nz;
                normCount++;
            }
        }
        else if (lineStart[0] == 'f' && lineStart[1] == ' ') {
            // Count vertices in face
            int vCount = 0;
            char *ptr = lineStart + 2;
            while(*ptr) {
                while(*ptr == ' ') ptr++;
                if (!*ptr) break;
                vCount++;
                while(*ptr && *ptr != ' ') ptr++;
            }
            
            if (vCount == 3) totalDrawVerts += 3;
            else if (vCount == 4) totalDrawVerts += 6; // Quad = 2 Tris
        }
        
        lineStart = eol + 1;
    }
    
    
    // --- Pass 2: Draw Faces ---
    memcpy(txt, ps_obj, size);
    txt[size] = 0;
    lineStart = txt;
    
    GX_Begin(GX_TRIANGLES, GX_VTXFMT0, totalDrawVerts);
    
    while(lineStart < txt + size) {
        char *eol = strchr(lineStart, '\n');
        if (!eol) eol = txt + size;
        *eol = 0;
        
         // Trim
        int len = strlen(lineStart);
        if (len > 0 && lineStart[len-1] == '\r') lineStart[len-1] = 0;
        
        if (strncmp(lineStart, "usemtl", 6) == 0) {
            char mtlName[64];
            sscanf(lineStart + 7, "%s", mtlName);
            
            // Map Colors - Note: We can't change state inside GX_Begin on some hardware/APIs easily,
            // but for Vertex Colors via parameters, we just change the Attribute value.
            // GX_Color4u8 emits the color attribute for the NEXT vertex.
            // So we just update curColor.
            
            if (strstr(mtlName, "Material__25")) { // Blue
                curColor = (GXColor){0, 0, 255, 255};
            } else if (strstr(mtlName, "Material__26")) { // Green
                curColor = (GXColor){0, 255, 0, 255};
            } else if (strstr(mtlName, "Material__27")) { // Red
                curColor = (GXColor){255, 0, 0, 255};
            } else if (strstr(mtlName, "Material__28")) { // Yellow
                curColor = (GXColor){255, 255, 0, 255};
            } else {
                curColor = (GXColor){200, 200, 200, 255};
            }
        }
        else if (lineStart[0] == 'f' && lineStart[1] == ' ') {
            int v[4] = {0,0,0,0};
            // int n[4] = {0,0,0,0}; // We ignore file normals for faceted look
            int count = 0;
            
            char *ptr = lineStart + 2;
            while(*ptr && count < 4) {
                while(*ptr == ' ') ptr++; 
                if (!*ptr) break;
                
                v[count] = atoi(ptr);
                
                // Skip /vt/vn
                while(*ptr && *ptr != ' ') ptr++;
                
                count++;
            }
            
            if (count >= 3) {
                // Helper to calc face normal
                // We use guVector for math (needs <gccore.h> which includes gu.h)
                // CRITICAL: Validate indices are within bounds
                if (v[0] > 0 && v[0] <= vertCount && 
                    v[1] > 0 && v[1] <= vertCount && 
                    v[2] > 0 && v[2] <= vertCount) {
                     Vertex *p0 = &vertices[v[0]-1];
                     Vertex *p1 = &vertices[v[1]-1];
                     Vertex *p2 = &vertices[v[2]-1];
                     
                     guVector a = {p1->x - p0->x, p1->y - p0->y, p1->z - p0->z};
                     guVector b = {p2->x - p0->x, p2->y - p0->y, p2->z - p0->z};
                     guVector fn;
                     c_guVecCross(&a, &b, &fn);
                     c_guVecNormalize(&fn);
                     
                    // Triangle
                    for(int i=0; i<3; i++) {
                         int idxV = v[i]-1;
                         
                         if(idxV >= 0 && idxV < vertCount) {
                             Vertex *vert = &vertices[idxV];
                             GX_Position3f32(vert->x, vert->y, vert->z);
                             // Use Face Normal for Flat/Faceted Look
                             GX_Normal3f32(fn.x, fn.y, fn.z);
                             GX_Color4u8(curColor.r, curColor.g, curColor.b, 255);
                         }
                    }
                    
                    // Quad part 2
                    if (count == 4 && v[3] > 0 && v[3] <= vertCount) {
                         Vertex *p3 = &vertices[v[3]-1];
                         // New triangle 0-2-3? Or rather 0, 2, 3 as defined before
                         // For consistent flat shading across the quad, we technically should check planarity
                         // But usually quads in these OBJs are flat. Let's re-calc just in case or reuse.
                         // Reuse is fine if planar. Let's recalc for 0-2-3 triangle.
                         Vertex *q0 = p0;
                         Vertex *q1 = p2;
                         Vertex *q2 = p3;
                         guVector qa = {q1->x - q0->x, q1->y - q0->y, q1->z - q0->z};
                         guVector qb = {q2->x - q0->x, q2->y - q0->y, q2->z - q0->z};
                         guVector qfn;
                         c_guVecCross(&qa, &qb, &qfn);
                         c_guVecNormalize(&qfn);

                         int idxs[3] = {0, 2, 3};
                         for(int i=0; i<3; i++) {
                             int k = idxs[i];
                             int idxV = v[k]-1;

                             if(idxV >= 0 && idxV < vertCount) {
                                 Vertex *vert = &vertices[idxV];
                                 GX_Position3f32(vert->x, vert->y, vert->z);
                                 GX_Normal3f32(qfn.x, qfn.y, qfn.z);
                                 GX_Color4u8(curColor.r, curColor.g, curColor.b, 255);
                             }
                         }
                    }
                }
            }
        }
        
        lineStart = eol + 1;
    }
    
    GX_End();
    u32 listSize = GX_EndDispList();
    
    
    // CRITICAL: Validate display list size
    if (listSize == 0 || listSize > DL_SIZE) {
        printf("Display list error: size=%u (max=%d)\n", listSize, DL_SIZE);
        if (ps1DL) {
            free(ps1DL);
            ps1DL = NULL;
        }
        free(txt);
        return;
    }
    
    // CRITICAL: Store the actual size for later use
    ps1DLSize = listSize;
    printf("Display list created: %u bytes\n", ps1DLSize);
    
    // CRITICAL: Flush the cache so the GPU sees the command list (REAL WII REQUIREMENT)
    DCFlushRange(ps1DL, ps1DLSize);
    
    free(txt);
}

void LoadFirstSprite() {
    // Load sprites.png using stbi
    int imgW, imgH, imgCh;
    int pngSize = sprites_png_end - sprites_png;
    
    // Validate PNG size
    if (pngSize <= 0 || pngSize > 10*1024*1024) {
        printf("Invalid PNG size: %d\n", pngSize);
        return;
    }
    
    u8 *linearData = stbi_load_from_memory(sprites_png, pngSize, &imgW, &imgH, &imgCh, 4);
    
    if (!linearData) {
        printf("Failed to load sprite PNG\n");
        return;
    }
    
    // Convert to Tiled
    int alignedW = 0, alignedH = 0;
    u8 *tiledData = ConvertToTiled(linearData, imgW, imgH, &alignedW, &alignedH);
    
    if (!tiledData) {
        printf("Failed to convert to tiled\n");
        stbi_image_free(linearData);
        return;
    }
    
    spriteTexW = alignedW;
    spriteTexH = alignedH;
    
    
    // Validate dimensions
    if (alignedW <= 0 || alignedH <= 0 || alignedW > 1024 || alignedH > 1024) {
        printf("ERROR: Invalid texture dimensions!\n");
        stbi_image_free(linearData);
        return;
    }
    
    // CRITICAL: Ensure cache is flushed before initializing texture (REAL WII REQUIREMENT)
    u32 texSize = alignedW * alignedH * 4;
    DCFlushRange(tiledData, texSize);
    
    GX_InitTexObj(&spriteTexObj, tiledData, alignedW, alignedH, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjFilterMode(&spriteTexObj, GX_NEAR, GX_NEAR);
    
    stbi_image_free(linearData);
    
    // Parse scene2.txt - first two lines
    u32 txtSize = scene2_txt_end - scene2_txt;
    char *txt = (char*)malloc(txtSize + 1);
    memcpy(txt, scene2_txt, txtSize);
    txt[txtSize] = 0;
    
    // First line (Region1)
    char *line = txt;
    char *eol = strchr(line, '\n');
    if (eol) *eol = 0;
    
    if (strlen(line) > 0) {
        char name[32];
        int x, y, w, h;
        if (sscanf(line, "%s %d %d %d %d", name, &x, &y, &w, &h) == 5) {
            spriteRegion.x = x;
            spriteRegion.y = y;
            spriteRegion.w = w;
            spriteRegion.h = h;
        }
    }
    
    // Second line (Region2)
    if (eol) {
        line = eol + 1;
        eol = strchr(line, '\n');
        if (eol) *eol = 0;
        
        if (strlen(line) > 0) {
            char name[32];
            int x, y, w, h;
            if (sscanf(line, "%s %d %d %d %d", name, &x, &y, &w, &h) == 5) {
                spriteRegion2.x = x;
                spriteRegion2.y = y;
                spriteRegion2.w = w;
                spriteRegion2.h = h;
            }
        }
    }
    
    // Third and Fourth lines (Region3 and Region4)
    if (eol) {
        line = eol + 1;
        eol = strchr(line, '\n');
        if (eol) *eol = 0;
        
        // Parse Region3
        if (strlen(line) > 0) {
            char name[32];
            int x, y, w, h;
            if (sscanf(line, "%s %d %d %d %d", name, &x, &y, &w, &h) == 5) {
                spriteRegion3.x = x;
                spriteRegion3.y = y;
                spriteRegion3.w = w;
                spriteRegion3.h = h;
            }
        }
        
        // Parse Region4
        if (eol) {
            line = eol + 1;
            eol = strchr(line, '\n');
            if (eol) *eol = 0;
            
            if (strlen(line) > 0) {
                char name[32];
                int x, y, w, h;
                if (sscanf(line, "%s %d %d %d %d", name, &x, &y, &w, &h) == 5) {
                    spriteRegion4.x = x;
                    spriteRegion4.y = y;
                    spriteRegion4.w = w;
                    spriteRegion4.h = h;
                }
            }
        }
    }
    
    free(txt);
}


void RunScene2(u64 appStartTime) {
    printf("Starting Scene2...\n");
    
    // CRITICAL: Initialize Z-buffer settings FIRST
    GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    GX_SetColorUpdate(GX_TRUE);
    GX_SetAlphaUpdate(GX_TRUE);
    
    // Init 3D View
    Mtx44 projection;
    // guPerspective(projection, 50, 1.33f, 10.0f, 300.0f); 
    guOrtho(projection, 30, -30, -40, 40, 10, 300); // Orthographic (Octagonal/Isometric style)
    GX_LoadProjectionMtx(projection, GX_ORTHOGRAPHIC);
    
    // Re-setup Vertex Desc for 3D (Position + Color + Normal)
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_NRM, GX_DIRECT); // Added Normals
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_NRM, GX_NRM_XYZ, GX_F32, 0); // Added Normals
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    
    // --- key Light (Sun) ---
    GXLightObj lightSun;
    guVector sunPos = {0.0F, 150.0F, 80.0F}; 
    GX_InitLightPos(&lightSun, sunPos.x, sunPos.y, sunPos.z);
    GX_InitLightColor(&lightSun, (GXColor){255, 255, 255, 255});
    GX_LoadLightObj(&lightSun, GX_LIGHT0);
    
    // --- Fill Light (To fix pale/dark shadows) ---
    GXLightObj lightFill;
    guVector fillPos = {-100.0F, -50.0F, 50.0F}; // From below/side
    GX_InitLightPos(&lightFill, fillPos.x, fillPos.y, fillPos.z);
    GX_InitLightColor(&lightFill, (GXColor){60, 60, 80, 255}); // Blue-ish grey fill
    GX_LoadLightObj(&lightFill, GX_LIGHT1);
    
    // Set Ambient to Dark Grey (Boosts saturation vs Pure Black)
    GX_SetChanAmbColor(GX_COLOR0A0, (GXColor){40, 40, 40, 255});
    GX_SetChanMatColor(GX_COLOR0A0, (GXColor){255, 255, 255, 255});
    
    GX_SetNumChans(1);
    
    // Enable Both Lights (Sun + Fill)
    GX_SetChanCtrl(GX_COLOR0A0, GX_ENABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT0 | GX_LIGHT1, GX_DF_CLAMP, GX_AF_NONE);
    
    GX_SetTevOrder(GX_TEVSTAGE0, GX_COLOR0A0, GX_TEXMAP_NULL, GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    
    // Load Model and First Sprite
    printf("Loading model...\n");
    ParseAndLoadModel();
    
    // Check if model loaded successfully
    if (!ps1DL) {
        printf("ERROR: Model failed to load!\n");
        return;
    }
    
    printf("Loading sprites...\n");
    LoadFirstSprite();
    
    // CRITICAL FOR REAL WII: Ensure video system is synchronized after setup
    VIDEO_WaitVSync();
    
    // Track start time for fade-in
    u64 startTime = gettime();
    const f32 FADE_DURATION = 1.0f; // 1 second fade
    
    int frameCount = 0;
    
    while(1) {
        WPAD_ScanPads();
        if (WPAD_ButtonsHeld(0) & WPAD_BUTTON_HOME) {
            break;
        }
        
        frameCount++;
        if (frameCount % 60 == 0) {
        }
        
        // Calculate total elapsed time from application start
        f32 totalElapsed = (f32)ticks_to_millisecs(gettime() - appStartTime) / 1000.0f;
        
        // Exit to Wii Menu after music ends (~19.8s)
        if (totalElapsed >= 19.8f) {
            SYS_ResetSystem(SYS_RETURNTOMENU, 0, 0);
            break;
        }
        
        // Calculate fade progress (0.0 to 1.0)
        u64 now = gettime();
        f32 elapsed = (f32)ticks_to_millisecs(now - startTime) / 1000.0f;
        f32 fadeProgress = elapsed / FADE_DURATION;
        if (fadeProgress > 1.0f) fadeProgress = 1.0f;
        
        // Update lights with fade
        // --- Key Light (Sun) ---
        GXLightObj lightSun;
        guVector sunPos = {0.0F, 150.0F, 80.0F}; 
        GX_InitLightPos(&lightSun, sunPos.x, sunPos.y, sunPos.z);
        u8 sunIntensity = (u8)(255.0f * fadeProgress);
        GX_InitLightColor(&lightSun, (GXColor){sunIntensity, sunIntensity, sunIntensity, 255});
        GX_LoadLightObj(&lightSun, GX_LIGHT0);
        
        // --- Fill Light ---
        GXLightObj lightFill;
        guVector fillPos = {-100.0F, -50.0F, 50.0F};
        GX_InitLightPos(&lightFill, fillPos.x, fillPos.y, fillPos.z);
        u8 fillR = (u8)(60.0f * fadeProgress);
        u8 fillG = (u8)(60.0f * fadeProgress);
        u8 fillB = (u8)(80.0f * fadeProgress);
        GX_InitLightColor(&lightFill, (GXColor){fillR, fillG, fillB, 255});
        GX_LoadLightObj(&lightFill, GX_LIGHT1);
        
        // Ambient also fades
        u8 ambIntensity = (u8)(40.0f * fadeProgress);
        GX_SetChanAmbColor(GX_COLOR0A0, (GXColor){ambIntensity, ambIntensity, ambIntensity, 255});
        
        // Setup Camera
        Mtx model, view, mv;
        guVector cam = {0.0F, 0.0F, 65.0F};
        guVector up  = {0.0F, 1.0F, 0.0F};
        guVector look = {0.0F, -10.0F, 0.0F}; // Look down to move model up on screen
        
        guLookAt(view, &cam, &up, &look);
        
        // Model Matrix: Rotate
        guMtxIdentity(model);
        guMtxRotDeg(model, 'x', 12.0f); // Rotated upward
        
        guMtxConcat(view, model, mv);
        GX_LoadPosMtxImm(mv, GX_PNMTX0);
        
        // Draw
        GX_SetCopyClear((GXColor){0, 0, 0, 255}, GX_MAX_Z24);
        
        // === RENDER 2D SPRITE FIRST (Background) ===
        // Switch to 2D Orthographic
        Mtx44 ortho2D;
        guOrtho(ortho2D, 0, 224, 0, 256, 0, 100);
        GX_LoadProjectionMtx(ortho2D, GX_ORTHOGRAPHIC);
        
        Mtx identity;
        guMtxIdentity(identity);
        guMtxTransApply(identity, identity, 0, 0, -50.0f); // Far back
        GX_LoadPosMtxImm(identity, GX_PNMTX0);
        
        // Setup vertex descriptor for 2D (Position + TexCoord)
        GX_ClearVtxDesc();
        GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
        GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
        
        GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
        GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
        
        // Disable lighting for 2D
        GX_SetNumChans(0);
        
        // Enable texture for 2D
        GX_SetNumTexGens(1);
        GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
        
        GX_LoadTexObj(&spriteTexObj, GX_TEXMAP0);
        GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
        GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
        
        // Enable alpha blending and depth write for 2D background
        GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
        GX_SetZMode(GX_TRUE, GX_ALWAYS, GX_TRUE); // Write to depth buffer
        
        // Calculate UV with vertical flip for first sprite
        f32 u0 = (f32)spriteRegion.x / (f32)spriteTexW;
        f32 v0 = (f32)(spriteTexH - spriteRegion.y) / (f32)spriteTexH;
        f32 u1 = (f32)(spriteRegion.x + spriteRegion.w) / (f32)spriteTexW;
        f32 v1 = (f32)(spriteTexH - spriteRegion.y - spriteRegion.h) / (f32)spriteTexH;
        
        f32 x0 = 128.0f - (spriteRegion.w * 0.35f * 0.5f); // Centered, 35% scale (larger)
        f32 y0 = 110.0f; // Moved further up (user request)
        f32 x1 = x0 + (spriteRegion.w * 0.35f);
        f32 y1 = y0 + (spriteRegion.h * 0.35f);
        
        // Draw first sprite
        GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
            GX_Position3f32(x0, y0, 0);
            GX_TexCoord2f32(u0, v0);
            
            GX_Position3f32(x1, y0, 0);
            GX_TexCoord2f32(u1, v0);
            
            GX_Position3f32(x1, y1, 0);
            GX_TexCoord2f32(u1, v1);
            
            GX_Position3f32(x0, y1, 0);
            GX_TexCoord2f32(u0, v1);
        GX_End();
        
        // Calculate UV for second sprite
        f32 u0_2 = (f32)spriteRegion2.x / (f32)spriteTexW;
        f32 v0_2 = (f32)(spriteTexH - spriteRegion2.y) / (f32)spriteTexH;
        f32 u1_2 = (f32)(spriteRegion2.x + spriteRegion2.w) / (f32)spriteTexW;
        f32 v1_2 = (f32)(spriteTexH - spriteRegion2.y - spriteRegion2.h) / (f32)spriteTexH;
        
        // Position second sprite below the first
        f32 x0_2 = 128.0f - (spriteRegion2.w * 0.30f * 0.5f); // Centered, 30% scale
        f32 y0_2 = y1 + 20.0f; // 20 pixels below first sprite (moved down)
        f32 x1_2 = x0_2 + (spriteRegion2.w * 0.30f);
        f32 y1_2 = y0_2 + (spriteRegion2.h * 0.30f);
        
        // Draw second sprite
        GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
            GX_Position3f32(x0_2, y0_2, 0);
            GX_TexCoord2f32(u0_2, v0_2);
            
            GX_Position3f32(x1_2, y0_2, 0);
            GX_TexCoord2f32(u1_2, v0_2);
            
            GX_Position3f32(x1_2, y1_2, 0);
            GX_TexCoord2f32(u1_2, v1_2);
            
            GX_Position3f32(x0_2, y1_2, 0);
            GX_TexCoord2f32(u0_2, v1_2);
        GX_End();
        
        // Calculate UV for fourth sprite (Region4)
        f32 u0_4 = (f32)spriteRegion4.x / (f32)spriteTexW;
        f32 v0_4 = (f32)(spriteTexH - spriteRegion4.y) / (f32)spriteTexH;
        f32 u1_4 = (f32)(spriteRegion4.x + spriteRegion4.w) / (f32)spriteTexW;
        f32 v1_4 = (f32)(spriteTexH - spriteRegion4.y - spriteRegion4.h) / (f32)spriteTexH;
        
        // Position fourth sprite below the second
        f32 x0_4 = 128.0f - (spriteRegion4.w * 0.30f * 0.5f); // Centered, 30% scale
        f32 y0_4 = y1_2 + 0.0f; // No gap - touching Region2
        f32 x1_4 = x0_4 + (spriteRegion4.w * 0.30f);
        f32 y1_4 = y0_4 + (spriteRegion4.h * 0.30f);
        
        // Draw fourth sprite
        GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
            GX_Position3f32(x0_4, y0_4, 0);
            GX_TexCoord2f32(u0_4, v0_4);
            
            GX_Position3f32(x1_4, y0_4, 0);
            GX_TexCoord2f32(u1_4, v0_4);
            
            GX_Position3f32(x1_4, y1_4, 0);
            GX_TexCoord2f32(u1_4, v1_4);
            
            GX_Position3f32(x0_4, y1_4, 0);
            GX_TexCoord2f32(u0_4, v1_4);
        GX_End();
        
        // Calculate UV for third sprite (Region3)
        f32 u0_3 = (f32)spriteRegion3.x / (f32)spriteTexW;
        f32 v0_3 = (f32)(spriteTexH - spriteRegion3.y) / (f32)spriteTexH;
        f32 u1_3 = (f32)(spriteRegion3.x + spriteRegion3.w) / (f32)spriteTexW;
        f32 v1_3 = (f32)(spriteTexH - spriteRegion3.y - spriteRegion3.h) / (f32)spriteTexH;
        
        // Position third sprite below the fourth
        f32 x0_3 = 128.0f - (spriteRegion3.w * 0.30f * 0.5f); // Centered, 30% scale
        f32 y0_3 = y1_4 + 6.0f; // 6 pixels below fourth sprite (more separated)
        f32 x1_3 = x0_3 + (spriteRegion3.w * 0.30f);
        f32 y1_3 = y0_3 + (spriteRegion3.h * 0.30f);
        
        // Draw third sprite
        GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
            GX_Position3f32(x0_3, y0_3, 0);
            GX_TexCoord2f32(u0_3, v0_3);
            
            GX_Position3f32(x1_3, y0_3, 0);
            GX_TexCoord2f32(u1_3, v0_3);
            
            GX_Position3f32(x1_3, y1_3, 0);
            GX_TexCoord2f32(u1_3, v1_3);
            
            GX_Position3f32(x0_3, y1_3, 0);
            GX_TexCoord2f32(u0_3, v1_3);
        GX_End();
        
        // === NOW RENDER 3D MODEL ON TOP ===
        // Switch back to 3D projection
        GX_LoadProjectionMtx(projection, GX_ORTHOGRAPHIC);
        GX_LoadPosMtxImm(mv, GX_PNMTX0);
        
        // Setup vertex descriptor for 3D (Position + Normal + Color)
        GX_ClearVtxDesc();
        GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
        GX_SetVtxDesc(GX_VA_NRM, GX_DIRECT);
        GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
        
        GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
        GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_NRM, GX_NRM_XYZ, GX_F32, 0);
        GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
        
        // Enable lighting for 3D
        GX_SetNumChans(1);
        GX_SetChanCtrl(GX_COLOR0A0, GX_ENABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT0 | GX_LIGHT1, GX_DF_CLAMP, GX_AF_NONE);
        
        // No textures for 3D model
        GX_SetNumTexGens(0);
        GX_SetTevOrder(GX_TEVSTAGE0, GX_COLOR0A0, GX_TEXMAP_NULL, GX_COLOR0A0);
        GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
        
        // Enable depth testing for 3D (will render in front of 2D background)
        GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
        GX_SetBlendMode(GX_BM_NONE, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
        
        // Render 3D model
        if (ps1DL && ps1DLSize > 0) {
            if (frameCount == 1) {
            }
            // CRITICAL FOR REAL WII: Invalidate cache before calling display list
            DCInvalidateRange(ps1DL, ps1DLSize);
            GX_CallDispList(ps1DL, ps1DLSize); // Use actual size, not DL_SIZE!
            if (frameCount == 1) {
            }
        } else {
            if (frameCount == 1) {
            }
        }
        
        GX_DrawDone();
        GX_CopyDisp(frameBuffer[fbIndex], GX_TRUE);
        VIDEO_SetNextFramebuffer(frameBuffer[fbIndex]);
        VIDEO_Flush();
        VIDEO_WaitVSync();
        fbIndex ^= 1;
    }
    
    if (ps1DL) {
        free(ps1DL);
    }
}
