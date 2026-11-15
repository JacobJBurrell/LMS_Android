// Native post-processing pass for the Windows ARM64 build: a built-in replacement for
// the ReShade chain the PC game was played with (SMAA -> AMD CAS -> Vibrance). ReShade
// can't load into this native ARM64 process, so the same look is rebuilt here on the
// host IDirect3DDevice9 (standard D3D9 API only: system d3d9.dll or dgVoodoo's).
//
//   back buffer --StretchRect--> color0
//   SMAA 1x (preset HIGH): edge detection (luma; gamma) -> edges
//                          blending weights (edges, AreaTex, SearchTex) -> blend
//                          neighborhood blending (color0 linear/sRGB, blend) -> color1
//   AMD CAS (linear/sRGB, CAS.fx defaults)                                -> color0
//   vibrance (gamma)                                                      -> back buffer
// Disabled stages drop out of the chain; the last stage always writes the back buffer.
//
// Shaders are HLSL compiled at first use with D3DCompile from the system's
// d3dcompiler_47.dll (loaded dynamically). Any failure logs once and disables the pass.
// Settings: section [postfx] of lb_paths.ini next to the exe (see load_config below),
// LB_POSTFX=0/1 forces it off/on, and Scroll Lock toggles it in game.
//
// Third-party code embedded below, under their MIT licenses (notices kept verbatim):
//  - SMAA.fxh (Jimenez et al. 2013), the copy shipped with ReShade's SweetFX shaders,
//    unmodified; compiled with SMAA_HLSL_3. Its lookup textures are in smaa_textures.h.
//  - CAS (AMD FidelityFX Contrast Adaptive Sharpening), ported from SweetFX CAS.fx.
// The vibrance shader and everything else in this file is original to this port.
#ifdef _WIN32
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <mutex>
#include <string>
#include <vector>
#ifdef GPTR
#undef GPTR  // <winbase.h> GlobalAlloc flag; rt.h's GPTR() is the guest-pointer helper
#endif
extern "C" {
#include "rt.h"
}
#include "postfx.h"
#include "smaa_textures.h"

// ============================================================================ HLSL
namespace {

// Shared declarations, ahead of SMAA.fxh. One source holds every entry point.
const char k_hlsl_prelude[] = R"HLSL(
#define SMAA_HLSL_3 1
#define SMAA_PRESET_HIGH 1
#define SMAA_RT_METRICS pfx_rt_metrics
float4 pfx_rt_metrics : register(c0);   // (1/w, 1/h, w, h), VS and PS
float4 pfx_cas        : register(c1);   // PS: x = contrast, y = sharpening
float4 pfx_vibrance   : register(c2);   // PS: x = strength
sampler2D pfx_s0 : register(s0);
sampler2D pfx_s1 : register(s1);
sampler2D pfx_s2 : register(s2);
)HLSL";

// SMAA.fxh, verbatim.
const char k_smaa_fxh[] = R"SMAA_FXH(
/**
 * Copyright (C) 2013 Jorge Jimenez (jorge@iryoku.com)
 * Copyright (C) 2013 Jose I. Echevarria (joseignacioechevarria@gmail.com)
 * Copyright (C) 2013 Belen Masia (bmasia@unizar.es)
 * Copyright (C) 2013 Fernando Navarro (fernandn@microsoft.com)
 * Copyright (C) 2013 Diego Gutierrez (diegog@unizar.es)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is furnished to
 * do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software. As clarification, there
 * is no requirement that the copyright notice and permission be included in
 * binary distributions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */


/**
 *                  _______  ___  ___       ___           ___
 *                 /       ||   \/   |     /   \         /   \
 *                |   (---- |  \  /  |    /  ^  \       /  ^  \
 *                 \   \    |  |\/|  |   /  /_\  \     /  /_\  \
 *              ----)   |   |  |  |  |  /  _____  \   /  _____  \
 *             |_______/    |__|  |__| /__/     \__\ /__/     \__\
 * 
 *                               E N H A N C E D
 *       S U B P I X E L   M O R P H O L O G I C A L   A N T I A L I A S I N G
 *
 *                         http://www.iryoku.com/smaa/
 *
 * Hi, welcome aboard!
 * 
 * Here you'll find instructions to get the shader up and running as fast as
 * possible.
 *
 * IMPORTANTE NOTICE: when updating, remember to update both this file and the
 * precomputed textures! They may change from version to version.
 *
 * The shader has three passes, chained together as follows:
 *
 *                           |input|------------------+
 *                              v                     |
 *                    [ SMAA*EdgeDetection ]          |
 *                              v                     |
 *                          |edgesTex|                |
 *                              v                     |
 *              [ SMAABlendingWeightCalculation ]     |
 *                              v                     |
 *                          |blendTex|                |
 *                              v                     |
 *                [ SMAANeighborhoodBlending ] <------+
 *                              v
 *                           |output|
 *
 * Note that each [pass] has its own vertex and pixel shader. Remember to use
 * oversized triangles instead of quads to avoid overshading along the
 * diagonal.
 *
 * You've three edge detection methods to choose from: luma, color or depth.
 * They represent different quality/performance and anti-aliasing/sharpness
 * tradeoffs, so our recommendation is for you to choose the one that best
 * suits your particular scenario:
 *
 * - Depth edge detection is usually the fastest but it may miss some edges.
 *
 * - Luma edge detection is usually more expensive than depth edge detection,
 *   but catches visible edges that depth edge detection can miss.
 *
 * - Color edge detection is usually the most expensive one but catches
 *   chroma-only edges.
 *
 * For quickstarters: just use luma edge detection.
 *
 * The general advice is to not rush the integration process and ensure each
 * step is done correctly (don't try to integrate SMAA T2x with predicated edge
 * detection from the start!). Ok then, let's go!
 *
 *  1. The first step is to create two RGBA temporal render targets for holding
 *     |edgesTex| and |blendTex|.
 *
 *     In DX10 or DX11, you can use a RG render target for the edges texture.
 *     In the case of NVIDIA GPUs, using RG render targets seems to actually be
 *     slower.
 *
 *     On the Xbox 360, you can use the same render target for resolving both
 *     |edgesTex| and |blendTex|, as they aren't needed simultaneously.
 *
 *  2. Both temporal render targets |edgesTex| and |blendTex| must be cleared
 *     each frame. Do not forget to clear the alpha channel!
 *
 *  3. The next step is loading the two supporting precalculated textures,
 *     'areaTex' and 'searchTex'. You'll find them in the 'Textures' folder as
 *     C++ headers, and also as regular DDS files. They'll be needed for the
 *     'SMAABlendingWeightCalculation' pass.
 *
 *     If you use the C++ headers, be sure to load them in the format specified
 *     inside of them.
 *
 *     You can also compress 'areaTex' and 'searchTex' using BC5 and BC4
 *     respectively, if you have that option in your content processor pipeline.
 *     When compressing then, you get a non-perceptible quality decrease, and a
 *     marginal performance increase.
 *
 *  4. All samplers must be set to linear filtering and clamp.
 *
 *     After you get the technique working, remember that 64-bit inputs have
 *     half-rate linear filtering on GCN.
 *
 *     If SMAA is applied to 64-bit color buffers, switching to point filtering
 *     when accesing them will increase the performance. Search for
 *     'SMAASamplePoint' to see which textures may benefit from point
 *     filtering, and where (which is basically the color input in the edge
 *     detection and resolve passes).
 *
 *  5. All texture reads and buffer writes must be non-sRGB, with the exception
 *     of the input read and the output write in
 *     'SMAANeighborhoodBlending' (and only in this pass!). If sRGB reads in
 *     this last pass are not possible, the technique will work anyway, but
 *     will perform antialiasing in gamma space.
 *
 *     IMPORTANT: for best results the input read for the color/luma edge 
 *     detection should *NOT* be sRGB.
 *
 *  6. Before including SMAA.h you'll have to setup the render target metrics,
 *     the target and any optional configuration defines. Optionally you can
 *     use a preset.
 *
 *     You have the following targets available: 
 *         SMAA_HLSL_3
 *         SMAA_HLSL_4
 *         SMAA_HLSL_4_1
 *         SMAA_GLSL_3 *
 *         SMAA_GLSL_4 *
 *
 *         * (See SMAA_INCLUDE_VS and SMAA_INCLUDE_PS below).
 *
 *     And four presets:
 *         SMAA_PRESET_LOW          (%60 of the quality)
 *         SMAA_PRESET_MEDIUM       (%80 of the quality)
 *         SMAA_PRESET_HIGH         (%95 of the quality)
 *         SMAA_PRESET_ULTRA        (%99 of the quality)
 *
 *     For example:
 *         #define SMAA_RT_METRICS float4(1.0 / 1280.0, 1.0 / 720.0, 1280.0, 720.0)
 *         #define SMAA_HLSL_4
 *         #define SMAA_PRESET_HIGH
 *         #include "SMAA.h"
 *
 *     Note that SMAA_RT_METRICS doesn't need to be a macro, it can be a
 *     uniform variable. The code is designed to minimize the impact of not
 *     using a constant value, but it is still better to hardcode it.
 *
 *     Depending on how you encoded 'areaTex' and 'searchTex', you may have to
 *     add (and customize) the following defines before including SMAA.h:
 *          #define SMAA_AREATEX_SELECT(sample) sample.rg
 *          #define SMAA_SEARCHTEX_SELECT(sample) sample.r
 *
 *     If your engine is already using porting macros, you can define
 *     SMAA_CUSTOM_SL, and define the porting functions by yourself.
 *
 *  7. Then, you'll have to setup the passes as indicated in the scheme above.
 *     You can take a look into SMAA.fx, to see how we did it for our demo.
 *     Checkout the function wrappers, you may want to copy-paste them!
 *
 *  8. It's recommended to validate the produced |edgesTex| and |blendTex|.
 *     You can use a screenshot from your engine to compare the |edgesTex|
 *     and |blendTex| produced inside of the engine with the results obtained
 *     with the reference demo.
 *
 *  9. After you get the last pass to work, it's time to optimize. You'll have
 *     to initialize a stencil buffer in the first pass (discard is already in
 *     the code), then mask execution by using it the second pass. The last
 *     pass should be executed in all pixels.
 *
 *
 * After this point you can choose to enable predicated thresholding,
 * temporal supersampling and motion blur integration:
 *
 * a) If you want to use predicated thresholding, take a look into
 *    SMAA_PREDICATION; you'll need to pass an extra texture in the edge
 *    detection pass.
 *
 * b) If you want to enable temporal supersampling (SMAA T2x):
 *
 * 1. The first step is to render using subpixel jitters. I won't go into
 *    detail, but it's as simple as moving each vertex position in the
 *    vertex shader, you can check how we do it in our DX10 demo.
 *
 * 2. Then, you must setup the temporal resolve. You may want to take a look
 *    into SMAAResolve for resolving 2x modes. After you get it working, you'll
 *    probably see ghosting everywhere. But fear not, you can enable the
 *    CryENGINE temporal reprojection by setting the SMAA_REPROJECTION macro.
 *    Check out SMAA_DECODE_VELOCITY if your velocity buffer is encoded.
 *
 * 3. The next step is to apply SMAA to each subpixel jittered frame, just as
 *    done for 1x.
 *
 * 4. At this point you should already have something usable, but for best
 *    results the proper area textures must be set depending on current jitter.
 *    For this, the parameter 'subsampleIndices' of
 *    'SMAABlendingWeightCalculationPS' must be set as follows, for our T2x
 *    mode:
 *
 *    @SUBSAMPLE_INDICES
 *
 *    | S# |  Camera Jitter   |  subsampleIndices    |
 *    +----+------------------+---------------------+
 *    |  0 |  ( 0.25, -0.25)  |  float4(1, 1, 1, 0)  |
 *    |  1 |  (-0.25,  0.25)  |  float4(2, 2, 2, 0)  |
 *
 *    These jitter positions assume a bottom-to-top y axis. S# stands for the
 *    sample number.
 *
 * More information about temporal supersampling here:
 *    http://iryoku.com/aacourse/downloads/13-Anti-Aliasing-Methods-in-CryENGINE-3.pdf
 *
 * c) If you want to enable spatial multisampling (SMAA S2x):
 *
 * 1. The scene must be rendered using MSAA 2x. The MSAA 2x buffer must be
 *    created with:
 *      - DX10:     see below (*)
 *      - DX10.1:   D3D10_STANDARD_MULTISAMPLE_PATTERN or
 *      - DX11:     D3D11_STANDARD_MULTISAMPLE_PATTERN
 *
 *    This allows to ensure that the subsample order matches the table in
 *    @SUBSAMPLE_INDICES.
 *
 *    (*) In the case of DX10, we refer the reader to:
 *      - SMAA::detectMSAAOrder and
 *      - SMAA::msaaReorder
 *
 *    These functions allow to match the standard multisample patterns by
 *    detecting the subsample order for a specific GPU, and reordering
 *    them appropriately.
 *
 * 2. A shader must be run to output each subsample into a separate buffer
 *    (DX10 is required). You can use SMAASeparate for this purpose, or just do
 *    it in an existing pass (for example, in the tone mapping pass, which has
 *    the advantage of feeding tone mapped subsamples to SMAA, which will yield
 *    better results).
 *
 * 3. The full SMAA 1x pipeline must be run for each separated buffer, storing
 *    the results in the final buffer. The second run should alpha blend with
 *    the existing final buffer using a blending factor of 0.5.
 *    'subsampleIndices' must be adjusted as in the SMAA T2x case (see point
 *    b).
 *
 * d) If you want to enable temporal supersampling on top of SMAA S2x
 *    (which actually is SMAA 4x):
 *
 * 1. SMAA 4x consists on temporally jittering SMAA S2x, so the first step is
 *    to calculate SMAA S2x for current frame. In this case, 'subsampleIndices'
 *    must be set as follows:
 *
 *    | F# | S# |   Camera Jitter    |    Net Jitter     |   subsampleIndices   |
 *    +----+----+--------------------+-------------------+----------------------+
 *    |  0 |  0 |  ( 0.125,  0.125)  |  ( 0.375, -0.125) |  float4(5, 3, 1, 3)  |
 *    |  0 |  1 |  ( 0.125,  0.125)  |  (-0.125,  0.375) |  float4(4, 6, 2, 3)  |
 *    +----+----+--------------------+-------------------+----------------------+
 *    |  1 |  2 |  (-0.125, -0.125)  |  ( 0.125, -0.375) |  float4(3, 5, 1, 4)  |
 *    |  1 |  3 |  (-0.125, -0.125)  |  (-0.375,  0.125) |  float4(6, 4, 2, 4)  |
 *
 *    These jitter positions assume a bottom-to-top y axis. F# stands for the
 *    frame number. S# stands for the sample number.
 *
 * 2. After calculating SMAA S2x for current frame (with the new subsample
 *    indices), previous frame must be reprojected as in SMAA T2x mode (see
 *    point b).
 *
 * e) If motion blur is used, you may want to do the edge detection pass
 *    together with motion blur. This has two advantages:
 *
 * 1. Pixels under heavy motion can be omitted from the edge detection process.
 *    For these pixels we can just store "no edge", as motion blur will take
 *    care of them.
 * 2. The center pixel tap is reused.
 *
 * Note that in this case depth testing should be used instead of stenciling,
 * as we have to write all the pixels in the motion blur pass.
 *
 * That's it!
 */

//-----------------------------------------------------------------------------
// SMAA Presets

/**
 * Note that if you use one of these presets, the following configuration
 * macros will be ignored if set in the "Configurable Defines" section.
 */

#if defined(SMAA_PRESET_LOW)
#define SMAA_THRESHOLD 0.15
#define SMAA_MAX_SEARCH_STEPS 4
#define SMAA_DISABLE_DIAG_DETECTION
#define SMAA_DISABLE_CORNER_DETECTION
#elif defined(SMAA_PRESET_MEDIUM)
#define SMAA_THRESHOLD 0.1
#define SMAA_MAX_SEARCH_STEPS 8
#define SMAA_DISABLE_DIAG_DETECTION
#define SMAA_DISABLE_CORNER_DETECTION
#elif defined(SMAA_PRESET_HIGH)
#define SMAA_THRESHOLD 0.1
#define SMAA_MAX_SEARCH_STEPS 16
#define SMAA_MAX_SEARCH_STEPS_DIAG 8
#define SMAA_CORNER_ROUNDING 25
#elif defined(SMAA_PRESET_ULTRA)
#define SMAA_THRESHOLD 0.05
#define SMAA_MAX_SEARCH_STEPS 32
#define SMAA_MAX_SEARCH_STEPS_DIAG 16
#define SMAA_CORNER_ROUNDING 25
#endif

//-----------------------------------------------------------------------------
// Configurable Defines

/**
 * SMAA_THRESHOLD specifies the threshold or sensitivity to edges.
 * Lowering this value you will be able to detect more edges at the expense of
 * performance. 
 *
 * Range: [0, 0.5]
 *   0.1 is a reasonable value, and allows to catch most visible edges.
 *   0.05 is a rather overkill value, that allows to catch 'em all.
 *
 *   If temporal supersampling is used, 0.2 could be a reasonable value, as low
 *   contrast edges are properly filtered by just 2x.
 */
#ifndef SMAA_THRESHOLD
#define SMAA_THRESHOLD 0.1
#endif

/**
 * SMAA_DEPTH_THRESHOLD specifies the threshold for depth edge detection.
 * 
 * Range: depends on the depth range of the scene.
 */
#ifndef SMAA_DEPTH_THRESHOLD
#define SMAA_DEPTH_THRESHOLD (0.1 * SMAA_THRESHOLD)
#endif

/**
 * SMAA_MAX_SEARCH_STEPS specifies the maximum steps performed in the
 * horizontal/vertical pattern searches, at each side of the pixel.
 *
 * In number of pixels, it's actually the double. So the maximum line length
 * perfectly handled by, for example 16, is 64 (by perfectly, we meant that
 * longer lines won't look as good, but still antialiased).
 *
 * Range: [0, 112]
 */
#ifndef SMAA_MAX_SEARCH_STEPS
#define SMAA_MAX_SEARCH_STEPS 16
#endif

/**
 * SMAA_MAX_SEARCH_STEPS_DIAG specifies the maximum steps performed in the
 * diagonal pattern searches, at each side of the pixel. In this case we jump
 * one pixel at time, instead of two.
 *
 * Range: [0, 20]
 *
 * On high-end machines it is cheap (between a 0.8x and 0.9x slower for 16 
 * steps), but it can have a significant impact on older machines.
 *
 * Define SMAA_DISABLE_DIAG_DETECTION to disable diagonal processing.
 */
#ifndef SMAA_MAX_SEARCH_STEPS_DIAG
#define SMAA_MAX_SEARCH_STEPS_DIAG 8
#endif

/**
 * SMAA_CORNER_ROUNDING specifies how much sharp corners will be rounded.
 *
 * Range: [0, 100]
 *
 * Define SMAA_DISABLE_CORNER_DETECTION to disable corner processing.
 */
#ifndef SMAA_CORNER_ROUNDING
#define SMAA_CORNER_ROUNDING 25
#endif

/**
 * If there is an neighbor edge that has SMAA_LOCAL_CONTRAST_FACTOR times
 * bigger contrast than current edge, current edge will be discarded.
 *
 * This allows to eliminate spurious crossing edges, and is based on the fact
 * that, if there is too much contrast in a direction, that will hide
 * perceptually contrast in the other neighbors.
 */
#ifndef SMAA_LOCAL_CONTRAST_ADAPTATION_FACTOR
#define SMAA_LOCAL_CONTRAST_ADAPTATION_FACTOR 2.0
#endif

/**
 * Predicated thresholding allows to better preserve texture details and to
 * improve performance, by decreasing the number of detected edges using an
 * additional buffer like the light accumulation buffer, object ids or even the
 * depth buffer (the depth buffer usage may be limited to indoor or short range
 * scenes).
 *
 * It locally decreases the luma or color threshold if an edge is found in an
 * additional buffer (so the global threshold can be higher).
 *
 * This method was developed by Playstation EDGE MLAA team, and used in 
 * Killzone 3, by using the light accumulation buffer. More information here:
 *     http://iryoku.com/aacourse/downloads/06-MLAA-on-PS3.pptx 
 */
#ifndef SMAA_PREDICATION
#define SMAA_PREDICATION 0
#endif

/**
 * Threshold to be used in the additional predication buffer. 
 *
 * Range: depends on the input, so you'll have to find the magic number that
 * works for you.
 */
#ifndef SMAA_PREDICATION_THRESHOLD
#define SMAA_PREDICATION_THRESHOLD 0.01
#endif

/**
 * How much to scale the global threshold used for luma or color edge
 * detection when using predication.
 *
 * Range: [1, 16]
 */
#ifndef SMAA_PREDICATION_SCALE
#define SMAA_PREDICATION_SCALE 2.0
#endif

/**
 * How much to locally decrease the threshold.
 *
 * Range: [0, 4]
 */
#ifndef SMAA_PREDICATION_STRENGTH
#define SMAA_PREDICATION_STRENGTH 0.4
#endif

/**
 * Temporal reprojection allows to remove ghosting artifacts when using
 * temporal supersampling. We use the CryEngine 3 method which also introduces
 * velocity weighting. This feature is of extreme importance for totally
 * removing ghosting. More information here:
 *    http://iryoku.com/aacourse/downloads/13-Anti-Aliasing-Methods-in-CryENGINE-3.pdf
 *
 * Note that you'll need to setup a velocity buffer for enabling reprojection.
 * For static geometry, saving the previous depth buffer is a viable
 * alternative.
 */
#ifndef SMAA_REPROJECTION
#define SMAA_REPROJECTION 0
#endif

/**
 * SMAA_REPROJECTION_WEIGHT_SCALE controls the velocity weighting. It allows to
 * remove ghosting trails behind the moving object, which are not removed by
 * just using reprojection. Using low values will exhibit ghosting, while using
 * high values will disable temporal supersampling under motion.
 *
 * Behind the scenes, velocity weighting removes temporal supersampling when
 * the velocity of the subsamples differs (meaning they are different objects).
 *
 * Range: [0, 80]
 */
#ifndef SMAA_REPROJECTION_WEIGHT_SCALE
#define SMAA_REPROJECTION_WEIGHT_SCALE 30.0
#endif

/**
 * On some compilers, discard cannot be used in vertex shaders. Thus, they need
 * to be compiled separately.
 */
#ifndef SMAA_INCLUDE_VS
#define SMAA_INCLUDE_VS 1
#endif
#ifndef SMAA_INCLUDE_PS
#define SMAA_INCLUDE_PS 1
#endif

//-----------------------------------------------------------------------------
// Texture Access Defines

#ifndef SMAA_AREATEX_SELECT
#if defined(SMAA_HLSL_3)
#define SMAA_AREATEX_SELECT(sample) sample.ra
#else
#define SMAA_AREATEX_SELECT(sample) sample.rg
#endif
#endif

#ifndef SMAA_SEARCHTEX_SELECT
#define SMAA_SEARCHTEX_SELECT(sample) sample.r
#endif

#ifndef SMAA_DECODE_VELOCITY
#define SMAA_DECODE_VELOCITY(sample) sample.rg
#endif

//-----------------------------------------------------------------------------
// Non-Configurable Defines

#define SMAA_AREATEX_MAX_DISTANCE 16
#define SMAA_AREATEX_MAX_DISTANCE_DIAG 20
#define SMAA_AREATEX_PIXEL_SIZE (1.0 / float2(160.0, 560.0))
#define SMAA_AREATEX_SUBTEX_SIZE (1.0 / 7.0)
#define SMAA_SEARCHTEX_SIZE float2(66.0, 33.0)
#define SMAA_SEARCHTEX_PACKED_SIZE float2(64.0, 16.0)
#define SMAA_CORNER_ROUNDING_NORM (float(SMAA_CORNER_ROUNDING) / 100.0)

//-----------------------------------------------------------------------------
// Porting Functions

#if defined(SMAA_HLSL_3)
#define SMAATexture2D(tex) sampler2D tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) tex2Dlod(tex, float4(coord, 0.0, 0.0))
#define SMAASampleLevelZeroPoint(tex, coord) tex2Dlod(tex, float4(coord, 0.0, 0.0))
#define SMAASampleLevelZeroOffset(tex, coord, offset) tex2Dlod(tex, float4(coord + offset * SMAA_RT_METRICS.xy, 0.0, 0.0))
#define SMAASample(tex, coord) tex2D(tex, coord)
#define SMAASamplePoint(tex, coord) tex2D(tex, coord)
#define SMAASampleOffset(tex, coord, offset) tex2D(tex, coord + offset * SMAA_RT_METRICS.xy)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]
#endif
#if defined(SMAA_HLSL_4) || defined(SMAA_HLSL_4_1)
SamplerState LinearSampler { Filter = MIN_MAG_LINEAR_MIP_POINT; AddressU = Clamp; AddressV = Clamp; };
SamplerState PointSampler { Filter = MIN_MAG_MIP_POINT; AddressU = Clamp; AddressV = Clamp; };
#define SMAATexture2D(tex) Texture2D tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(LinearSampler, coord, 0)
#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(PointSampler, coord, 0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(LinearSampler, coord, 0, offset)
#define SMAASample(tex, coord) tex.Sample(LinearSampler, coord)
#define SMAASamplePoint(tex, coord) tex.Sample(PointSampler, coord)
#define SMAASampleOffset(tex, coord, offset) tex.Sample(LinearSampler, coord, offset)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]
#define SMAATexture2DMS2(tex) Texture2DMS<float4, 2> tex
#define SMAALoad(tex, pos, sample) tex.Load(pos, sample)
#if defined(SMAA_HLSL_4_1)
#define SMAAGather(tex, coord) tex.Gather(LinearSampler, coord, 0)
#endif
#endif
#if defined(SMAA_GLSL_3) || defined(SMAA_GLSL_4)
#define SMAATexture2D(tex) sampler2D tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) textureLod(tex, coord, 0.0)
#define SMAASampleLevelZeroPoint(tex, coord) textureLod(tex, coord, 0.0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) textureLodOffset(tex, coord, 0.0, offset)
#define SMAASample(tex, coord) texture(tex, coord)
#define SMAASamplePoint(tex, coord) texture(tex, coord)
#define SMAASampleOffset(tex, coord, offset) texture(tex, coord, offset)
#define SMAA_FLATTEN
#define SMAA_BRANCH
#define lerp(a, b, t) mix(a, b, t)
#define saturate(a) clamp(a, 0.0, 1.0)
#if defined(SMAA_GLSL_4)
#define mad(a, b, c) fma(a, b, c)
#define SMAAGather(tex, coord) textureGather(tex, coord)
#else
#define mad(a, b, c) (a * b + c)
#endif
#define float2 vec2
#define float3 vec3
#define float4 vec4
#define int2 ivec2
#define int3 ivec3
#define int4 ivec4
#define bool2 bvec2
#define bool3 bvec3
#define bool4 bvec4
#endif

#if !defined(SMAA_HLSL_3) && !defined(SMAA_HLSL_4) && !defined(SMAA_HLSL_4_1) && !defined(SMAA_GLSL_3) && !defined(SMAA_GLSL_4) && !defined(SMAA_CUSTOM_SL)
#error you must define the shading language: SMAA_HLSL_*, SMAA_GLSL_* or SMAA_CUSTOM_SL
#endif

//-----------------------------------------------------------------------------
// Misc functions

/**
 * Gathers current pixel, and the top-left neighbors.
 */
float3 SMAAGatherNeighbours(float2 texcoord,
                            float4 offset[3],
                            SMAATexture2D(tex)) {
    #ifdef SMAAGather
    return SMAAGather(tex, texcoord + SMAA_RT_METRICS.xy * float2(-0.5, -0.5)).grb;
    #else
    float P = SMAASamplePoint(tex, texcoord).r;
    float Pleft = SMAASamplePoint(tex, offset[0].xy).r;
    float Ptop  = SMAASamplePoint(tex, offset[0].zw).r;
    return float3(P, Pleft, Ptop);
    #endif
}

/**
 * Adjusts the threshold by means of predication.
 */
float2 SMAACalculatePredicatedThreshold(float2 texcoord,
                                        float4 offset[3],
                                        SMAATexture2D(predicationTex)) {
    float3 neighbours = SMAAGatherNeighbours(texcoord, offset, SMAATexturePass2D(predicationTex));
    float2 delta = abs(neighbours.xx - neighbours.yz);
    float2 edges = step(SMAA_PREDICATION_THRESHOLD, delta);
    return SMAA_PREDICATION_SCALE * SMAA_THRESHOLD * (1.0 - SMAA_PREDICATION_STRENGTH * edges);
}

/**
 * Conditional move:
 */
void SMAAMovc(bool2 cond, inout float2 variable, float2 value) {
    SMAA_FLATTEN if (cond.x) variable.x = value.x;
    SMAA_FLATTEN if (cond.y) variable.y = value.y;
}

void SMAAMovc(bool4 cond, inout float4 variable, float4 value) {
    SMAAMovc(cond.xy, variable.xy, value.xy);
    SMAAMovc(cond.zw, variable.zw, value.zw);
}


#if SMAA_INCLUDE_VS
//-----------------------------------------------------------------------------
// Vertex Shaders

/**
 * Edge Detection Vertex Shader
 */
void SMAAEdgeDetectionVS(float2 texcoord,
                         out float4 offset[3]) {
    offset[0] = mad(SMAA_RT_METRICS.xyxy, float4(-1.0, 0.0, 0.0, -1.0), texcoord.xyxy);
    offset[1] = mad(SMAA_RT_METRICS.xyxy, float4( 1.0, 0.0, 0.0,  1.0), texcoord.xyxy);
    offset[2] = mad(SMAA_RT_METRICS.xyxy, float4(-2.0, 0.0, 0.0, -2.0), texcoord.xyxy);
}

/**
 * Blend Weight Calculation Vertex Shader
 */
void SMAABlendingWeightCalculationVS(float2 texcoord,
                                     out float2 pixcoord,
                                     out float4 offset[3]) {
    pixcoord = texcoord * SMAA_RT_METRICS.zw;

    // We will use these offsets for the searches later on (see @PSEUDO_GATHER4):
    offset[0] = mad(SMAA_RT_METRICS.xyxy, float4(-0.25, -0.125,  1.25, -0.125), texcoord.xyxy);
    offset[1] = mad(SMAA_RT_METRICS.xyxy, float4(-0.125, -0.25, -0.125,  1.25), texcoord.xyxy);

    // And these for the searches, they indicate the ends of the loops:
    offset[2] = mad(SMAA_RT_METRICS.xxyy,
                    float4(-2.0, 2.0, -2.0, 2.0) * float(SMAA_MAX_SEARCH_STEPS),
                    float4(offset[0].xz, offset[1].yw));
}

/**
 * Neighborhood Blending Vertex Shader
 */
void SMAANeighborhoodBlendingVS(float2 texcoord,
                                out float4 offset) {
    offset = mad(SMAA_RT_METRICS.xyxy, float4( 1.0, 0.0, 0.0,  1.0), texcoord.xyxy);
}
#endif // SMAA_INCLUDE_VS

#if SMAA_INCLUDE_PS
//-----------------------------------------------------------------------------
// Edge Detection Pixel Shaders (First Pass)

/**
 * Luma Edge Detection
 *
 * IMPORTANT NOTICE: luma edge detection requires gamma-corrected colors, and
 * thus 'colorTex' should be a non-sRGB texture.
 */
float2 SMAALumaEdgeDetectionPS(float2 texcoord,
                               float4 offset[3],
                               SMAATexture2D(colorTex)
                               ) {
    // Calculate the threshold:
    float2 threshold = float2(SMAA_THRESHOLD, SMAA_THRESHOLD);

    // Calculate lumas:
    float3 weights = float3(0.2126, 0.7152, 0.0722);
    float L = dot(SMAASamplePoint(colorTex, texcoord).rgb, weights);

    float Lleft = dot(SMAASamplePoint(colorTex, offset[0].xy).rgb, weights);
    float Ltop  = dot(SMAASamplePoint(colorTex, offset[0].zw).rgb, weights);

    // We do the usual threshold:
    float4 delta;
    delta.xy = abs(L - float2(Lleft, Ltop));
    float2 edges = step(threshold, delta.xy);

    // Then discard if there is no edge:
    if (dot(edges, float2(1.0, 1.0)) == 0.0)
        discard;

    // Calculate right and bottom deltas:
    float Lright = dot(SMAASamplePoint(colorTex, offset[1].xy).rgb, weights);
    float Lbottom  = dot(SMAASamplePoint(colorTex, offset[1].zw).rgb, weights);
    delta.zw = abs(L - float2(Lright, Lbottom));

    // Calculate the maximum delta in the direct neighborhood:
    float2 maxDelta = max(delta.xy, delta.zw);

    // Calculate left-left and top-top deltas:
    float Lleftleft = dot(SMAASamplePoint(colorTex, offset[2].xy).rgb, weights);
    float Ltoptop = dot(SMAASamplePoint(colorTex, offset[2].zw).rgb, weights);
    delta.zw = abs(float2(Lleft, Ltop) - float2(Lleftleft, Ltoptop));

    // Calculate the final maximum delta:
    maxDelta = max(maxDelta.xy, delta.zw);
    float finalDelta = max(maxDelta.x, maxDelta.y);

    // Local contrast adaptation:
    edges.xy *= step(finalDelta, SMAA_LOCAL_CONTRAST_ADAPTATION_FACTOR * delta.xy);

    return edges;
}

float2 SMAALumaEdgePredicationDetectionPS(float2 texcoord,
                               float4 offset[3],
                               SMAATexture2D(colorTex)
                               , SMAATexture2D(predicationTex)
                               ) {
    // Calculate the threshold:
	float2 threshold = SMAACalculatePredicatedThreshold(texcoord, offset, SMAATexturePass2D(predicationTex));

    // Calculate lumas:
    float3 weights = float3(0.2126, 0.7152, 0.0722);
    float L = dot(SMAASamplePoint(colorTex, texcoord).rgb, weights);

    float Lleft = dot(SMAASamplePoint(colorTex, offset[0].xy).rgb, weights);
    float Ltop  = dot(SMAASamplePoint(colorTex, offset[0].zw).rgb, weights);

    // We do the usual threshold:
    float4 delta;
    delta.xy = abs(L - float2(Lleft, Ltop));
    float2 edges = step(threshold, delta.xy);

    // Then discard if there is no edge:
    if (dot(edges, float2(1.0, 1.0)) == 0.0)
        discard;

    // Calculate right and bottom deltas:
    float Lright = dot(SMAASamplePoint(colorTex, offset[1].xy).rgb, weights);
    float Lbottom  = dot(SMAASamplePoint(colorTex, offset[1].zw).rgb, weights);
    delta.zw = abs(L - float2(Lright, Lbottom));

    // Calculate the maximum delta in the direct neighborhood:
    float2 maxDelta = max(delta.xy, delta.zw);

    // Calculate left-left and top-top deltas:
    float Lleftleft = dot(SMAASamplePoint(colorTex, offset[2].xy).rgb, weights);
    float Ltoptop = dot(SMAASamplePoint(colorTex, offset[2].zw).rgb, weights);
    delta.zw = abs(float2(Lleft, Ltop) - float2(Lleftleft, Ltoptop));

    // Calculate the final maximum delta:
    maxDelta = max(maxDelta.xy, delta.zw);
    float finalDelta = max(maxDelta.x, maxDelta.y);

    // Local contrast adaptation:
    edges.xy *= step(finalDelta, SMAA_LOCAL_CONTRAST_ADAPTATION_FACTOR * delta.xy);

    return edges;
}

/**
 * Color Edge Detection
 *
 * IMPORTANT NOTICE: color edge detection requires gamma-corrected colors, and
 * thus 'colorTex' should be a non-sRGB texture.
 */
float2 SMAAColorEdgeDetectionPS(float2 texcoord,
                                float4 offset[3],
                                SMAATexture2D(colorTex)
                                ) {
    // Calculate the threshold:
    float2 threshold = float2(SMAA_THRESHOLD, SMAA_THRESHOLD);

    // Calculate color deltas:
    float4 delta;
    float3 C = SMAASamplePoint(colorTex, texcoord).rgb;

    float3 Cleft = SMAASamplePoint(colorTex, offset[0].xy).rgb;
    float3 t = abs(C - Cleft);
    delta.x = max(max(t.r, t.g), t.b);

    float3 Ctop  = SMAASamplePoint(colorTex, offset[0].zw).rgb;
    t = abs(C - Ctop);
    delta.y = max(max(t.r, t.g), t.b);

    // We do the usual threshold:
    float2 edges = step(threshold, delta.xy);

    // Then discard if there is no edge:
    if (dot(edges, float2(1.0, 1.0)) == 0.0)
        discard;

    // Calculate right and bottom deltas:
    float3 Cright = SMAASamplePoint(colorTex, offset[1].xy).rgb;
    t = abs(C - Cright);
    delta.z = max(max(t.r, t.g), t.b);

    float3 Cbottom  = SMAASamplePoint(colorTex, offset[1].zw).rgb;
    t = abs(C - Cbottom);
    delta.w = max(max(t.r, t.g), t.b);

    // Calculate the maximum delta in the direct neighborhood:
    float2 maxDelta = max(delta.xy, delta.zw);

    // Calculate left-left and top-top deltas:
    float3 Cleftleft  = SMAASamplePoint(colorTex, offset[2].xy).rgb;
    t = abs(Cleft - Cleftleft);
    delta.z = max(max(t.r, t.g), t.b);

    float3 Ctoptop = SMAASamplePoint(colorTex, offset[2].zw).rgb;
    t = abs(Ctop - Ctoptop);
    delta.w = max(max(t.r, t.g), t.b);

    // Calculate the final maximum delta:
    maxDelta = max(maxDelta.xy, delta.zw);
    float finalDelta = max(maxDelta.x, maxDelta.y);

    // Local contrast adaptation:
    edges.xy *= step(finalDelta, SMAA_LOCAL_CONTRAST_ADAPTATION_FACTOR * delta.xy);

    return edges;
}

float2 SMAAColorEdgePredicationDetectionPS(float2 texcoord,
                                float4 offset[3],
                                SMAATexture2D(colorTex)
                                , SMAATexture2D(predicationTex)
                                ) {
    // Calculate the threshold:
    float2 threshold = SMAACalculatePredicatedThreshold(texcoord, offset, predicationTex);

    // Calculate color deltas:
    float4 delta;
    float3 C = SMAASamplePoint(colorTex, texcoord).rgb;

    float3 Cleft = SMAASamplePoint(colorTex, offset[0].xy).rgb;
    float3 t = abs(C - Cleft);
    delta.x = max(max(t.r, t.g), t.b);

    float3 Ctop  = SMAASamplePoint(colorTex, offset[0].zw).rgb;
    t = abs(C - Ctop);
    delta.y = max(max(t.r, t.g), t.b);

    // We do the usual threshold:
    float2 edges = step(threshold, delta.xy);

    // Then discard if there is no edge:
    if (dot(edges, float2(1.0, 1.0)) == 0.0)
        discard;

    // Calculate right and bottom deltas:
    float3 Cright = SMAASamplePoint(colorTex, offset[1].xy).rgb;
    t = abs(C - Cright);
    delta.z = max(max(t.r, t.g), t.b);

    float3 Cbottom  = SMAASamplePoint(colorTex, offset[1].zw).rgb;
    t = abs(C - Cbottom);
    delta.w = max(max(t.r, t.g), t.b);

    // Calculate the maximum delta in the direct neighborhood:
    float2 maxDelta = max(delta.xy, delta.zw);

    // Calculate left-left and top-top deltas:
    float3 Cleftleft  = SMAASamplePoint(colorTex, offset[2].xy).rgb;
    t = abs(Cleft - Cleftleft);
    delta.z = max(max(t.r, t.g), t.b);

    float3 Ctoptop = SMAASamplePoint(colorTex, offset[2].zw).rgb;
    t = abs(Ctop - Ctoptop);
    delta.w = max(max(t.r, t.g), t.b);

    // Calculate the final maximum delta:
    maxDelta = max(maxDelta.xy, delta.zw);
    float finalDelta = max(maxDelta.x, maxDelta.y);

    // Local contrast adaptation:
    edges.xy *= step(finalDelta, SMAA_LOCAL_CONTRAST_ADAPTATION_FACTOR * delta.xy);

    return edges;
}

/**
 * Depth Edge Detection
 */
float2 SMAADepthEdgeDetectionPS(float2 texcoord,
                                float4 offset[3],
                                SMAATexture2D(depthTex)) {
    float3 neighbours = SMAAGatherNeighbours(texcoord, offset, SMAATexturePass2D(depthTex));
    float2 delta = abs(neighbours.xx - float2(neighbours.y, neighbours.z));
    float2 edges = step(SMAA_DEPTH_THRESHOLD, delta);

    if (dot(edges, float2(1.0, 1.0)) == 0.0)
        discard;

    return edges;
}

//-----------------------------------------------------------------------------
// Diagonal Search Functions

#if !defined(SMAA_DISABLE_DIAG_DETECTION)

/**
 * Allows to decode two binary values from a bilinear-filtered access.
 */
float2 SMAADecodeDiagBilinearAccess(float2 e) {
    // Bilinear access for fetching 'e' have a 0.25 offset, and we are
    // interested in the R and G edges:
    //
    // +---G---+-------+
    // |   x o R   x   |
    // +-------+-------+
    //
    // Then, if one of these edge is enabled:
    //   Red:   (0.75 * X + 0.25 * 1) => 0.25 or 1.0
    //   Green: (0.75 * 1 + 0.25 * X) => 0.75 or 1.0
    //
    // This function will unpack the values (mad + mul + round):
    // wolframalpha.com: round(x * abs(5 * x - 5 * 0.75)) plot 0 to 1
    e.r = e.r * abs(5.0 * e.r - 5.0 * 0.75);
    return round(e);
}

float4 SMAADecodeDiagBilinearAccess(float4 e) {
    e.rb = e.rb * abs(5.0 * e.rb - 5.0 * 0.75);
    return round(e);
}

/**
 * These functions allows to perform diagonal pattern searches.
 */
float2 SMAASearchDiag1(SMAATexture2D(edgesTex), float2 texcoord, float2 dir, out float2 e) {
    float4 coord = float4(texcoord, -1.0, 1.0);
    float3 t = float3(SMAA_RT_METRICS.xy, 1.0);
    while (coord.z < float(SMAA_MAX_SEARCH_STEPS_DIAG - 1) &&
           coord.w > 0.9) {
        coord.xyz = mad(t, float3(dir, 1.0), coord.xyz);
        e = SMAASampleLevelZero(edgesTex, coord.xy).rg;
        coord.w = dot(e, float2(0.5, 0.5));
    }
    return coord.zw;
}

float2 SMAASearchDiag2(SMAATexture2D(edgesTex), float2 texcoord, float2 dir, out float2 e) {
    float4 coord = float4(texcoord, -1.0, 1.0);
    coord.x += 0.25 * SMAA_RT_METRICS.x; // See @SearchDiag2Optimization
    float3 t = float3(SMAA_RT_METRICS.xy, 1.0);
    while (coord.z < float(SMAA_MAX_SEARCH_STEPS_DIAG - 1) &&
           coord.w > 0.9) {
        coord.xyz = mad(t, float3(dir, 1.0), coord.xyz);

        // @SearchDiag2Optimization
        // Fetch both edges at once using bilinear filtering:
        e = SMAASampleLevelZero(edgesTex, coord.xy).rg;
        e = SMAADecodeDiagBilinearAccess(e);

        // Non-optimized version:
        // e.g = SMAASampleLevelZero(edgesTex, coord.xy).g;
        // e.r = SMAASampleLevelZeroOffset(edgesTex, coord.xy, int2(1, 0)).r;

        coord.w = dot(e, float2(0.5, 0.5));
    }
    return coord.zw;
}

/** 
 * Similar to SMAAArea, this calculates the area corresponding to a certain
 * diagonal distance and crossing edges 'e'.
 */
float2 SMAAAreaDiag(SMAATexture2D(areaTex), float2 dist, float2 e, float offset) {
    float2 texcoord = mad(float2(SMAA_AREATEX_MAX_DISTANCE_DIAG, SMAA_AREATEX_MAX_DISTANCE_DIAG), e, dist);

    // We do a scale and bias for mapping to texel space:
    texcoord = mad(SMAA_AREATEX_PIXEL_SIZE, texcoord, 0.5 * SMAA_AREATEX_PIXEL_SIZE);

    // Diagonal areas are on the second half of the texture:
    texcoord.x += 0.5;

    // Move to proper place, according to the subpixel offset:
    texcoord.y += SMAA_AREATEX_SUBTEX_SIZE * offset;

    // Do it!
    return SMAA_AREATEX_SELECT(SMAASampleLevelZero(areaTex, texcoord));
}

/**
 * This searches for diagonal patterns and returns the corresponding weights.
 */
float2 SMAACalculateDiagWeights(SMAATexture2D(edgesTex), SMAATexture2D(areaTex), float2 texcoord, float2 e, float4 subsampleIndices) {
    float2 weights = float2(0.0, 0.0);

    // Search for the line ends:
    float4 d;
    float2 end;
    if (e.r > 0.0) {
        d.xz = SMAASearchDiag1(SMAATexturePass2D(edgesTex), texcoord, float2(-1.0,  1.0), end);
        d.x += float(end.y > 0.9);
    } else
        d.xz = float2(0.0, 0.0);
    d.yw = SMAASearchDiag1(SMAATexturePass2D(edgesTex), texcoord, float2(1.0, -1.0), end);

    SMAA_BRANCH
    if (d.x + d.y > 2.0) { // d.x + d.y + 1 > 3
        // Fetch the crossing edges:
        float4 coords = mad(float4(-d.x + 0.25, d.x, d.y, -d.y - 0.25), SMAA_RT_METRICS.xyxy, texcoord.xyxy);
        float4 c;
        c.xy = SMAASampleLevelZeroOffset(edgesTex, coords.xy, int2(-1,  0)).rg;
        c.zw = SMAASampleLevelZeroOffset(edgesTex, coords.zw, int2( 1,  0)).rg;
        c.yxwz = SMAADecodeDiagBilinearAccess(c.xyzw);

        // Non-optimized version:
        // float4 coords = mad(float4(-d.x, d.x, d.y, -d.y), SMAA_RT_METRICS.xyxy, texcoord.xyxy);
        // float4 c;
        // c.x = SMAASampleLevelZeroOffset(edgesTex, coords.xy, int2(-1,  0)).g;
        // c.y = SMAASampleLevelZeroOffset(edgesTex, coords.xy, int2( 0,  0)).r;
        // c.z = SMAASampleLevelZeroOffset(edgesTex, coords.zw, int2( 1,  0)).g;
        // c.w = SMAASampleLevelZeroOffset(edgesTex, coords.zw, int2( 1, -1)).r;

        // Merge crossing edges at each side into a single value:
        float2 cc = mad(float2(2.0, 2.0), c.xz, c.yw);

        // Remove the crossing edge if we didn't found the end of the line:
        SMAAMovc(bool2(step(0.9, d.zw)), cc, float2(0.0, 0.0));

        // Fetch the areas for this line:
        weights += SMAAAreaDiag(SMAATexturePass2D(areaTex), d.xy, cc, subsampleIndices.z);
    }

    // Search for the line ends:
    d.xz = SMAASearchDiag2(SMAATexturePass2D(edgesTex), texcoord, float2(-1.0, -1.0), end);
    if (SMAASampleLevelZeroOffset(edgesTex, texcoord, int2(1, 0)).r > 0.0) {
        d.yw = SMAASearchDiag2(SMAATexturePass2D(edgesTex), texcoord, float2(1.0, 1.0), end);
        d.y += float(end.y > 0.9);
    } else
        d.yw = float2(0.0, 0.0);

    SMAA_BRANCH
    if (d.x + d.y > 2.0) { // d.x + d.y + 1 > 3
        // Fetch the crossing edges:
        float4 coords = mad(float4(-d.x, -d.x, d.y, d.y), SMAA_RT_METRICS.xyxy, texcoord.xyxy);
        float4 c;
        c.x  = SMAASampleLevelZeroOffset(edgesTex, coords.xy, int2(-1,  0)).g;
        c.y  = SMAASampleLevelZeroOffset(edgesTex, coords.xy, int2( 0, -1)).r;
        c.zw = SMAASampleLevelZeroOffset(edgesTex, coords.zw, int2( 1,  0)).gr;
        float2 cc = mad(float2(2.0, 2.0), c.xz, c.yw);

        // Remove the crossing edge if we didn't found the end of the line:
        SMAAMovc(bool2(step(0.9, d.zw)), cc, float2(0.0, 0.0));

        // Fetch the areas for this line:
        weights += SMAAAreaDiag(SMAATexturePass2D(areaTex), d.xy, cc, subsampleIndices.w).gr;
    }

    return weights;
}
#endif

//-----------------------------------------------------------------------------
// Horizontal/Vertical Search Functions

/**
 * This allows to determine how much length should we add in the last step
 * of the searches. It takes the bilinearly interpolated edge (see 
 * @PSEUDO_GATHER4), and adds 0, 1 or 2, depending on which edges and
 * crossing edges are active.
 */
float SMAASearchLength(SMAATexture2D(searchTex), float2 e, float offset) {
    // The texture is flipped vertically, with left and right cases taking half
    // of the space horizontally:
    float2 scale = SMAA_SEARCHTEX_SIZE * float2(0.5, -1.0);
    float2 bias = SMAA_SEARCHTEX_SIZE * float2(offset, 1.0);

    // Scale and bias to access texel centers:
    scale += float2(-1.0,  1.0);
    bias  += float2( 0.5, -0.5);

    // Convert from pixel coordinates to texcoords:
    // (We use SMAA_SEARCHTEX_PACKED_SIZE because the texture is cropped)
    scale *= 1.0 / SMAA_SEARCHTEX_PACKED_SIZE;
    bias *= 1.0 / SMAA_SEARCHTEX_PACKED_SIZE;

    // Lookup the search texture:
    return SMAA_SEARCHTEX_SELECT(SMAASampleLevelZero(searchTex, mad(scale, e, bias)));
}

/**
 * Horizontal/vertical search functions for the 2nd pass.
 */
float SMAASearchXLeft(SMAATexture2D(edgesTex), SMAATexture2D(searchTex), float2 texcoord, float end) {
    /**
     * @PSEUDO_GATHER4
     * This texcoord has been offset by (-0.25, -0.125) in the vertex shader to
     * sample between edge, thus fetching four edges in a row.
     * Sampling with different offsets in each direction allows to disambiguate
     * which edges are active from the four fetched ones.
     */
    float2 e = float2(0.0, 1.0);
    while (texcoord.x > end && 
           e.g > 0.8281 && // Is there some edge not activated?
           e.r == 0.0) { // Or is there a crossing edge that breaks the line?
        e = SMAASampleLevelZero(edgesTex, texcoord).rg;
        texcoord = mad(-float2(2.0, 0.0), SMAA_RT_METRICS.xy, texcoord);
    }

    float offset = mad(-(255.0 / 127.0), SMAASearchLength(SMAATexturePass2D(searchTex), e, 0.0), 3.25);
    return mad(SMAA_RT_METRICS.x, offset, texcoord.x);

    // Non-optimized version:
    // We correct the previous (-0.25, -0.125) offset we applied:
    // texcoord.x += 0.25 * SMAA_RT_METRICS.x;

    // The searches are bias by 1, so adjust the coords accordingly:
    // texcoord.x += SMAA_RT_METRICS.x;

    // Disambiguate the length added by the last step:
    // texcoord.x += 2.0 * SMAA_RT_METRICS.x; // Undo last step
    // texcoord.x -= SMAA_RT_METRICS.x * (255.0 / 127.0) * SMAASearchLength(SMAATexturePass2D(searchTex), e, 0.0);
    // return mad(SMAA_RT_METRICS.x, offset, texcoord.x);
}

float SMAASearchXRight(SMAATexture2D(edgesTex), SMAATexture2D(searchTex), float2 texcoord, float end) {
    float2 e = float2(0.0, 1.0);
    while (texcoord.x < end && 
           e.g > 0.8281 && // Is there some edge not activated?
           e.r == 0.0) { // Or is there a crossing edge that breaks the line?
        e = SMAASampleLevelZero(edgesTex, texcoord).rg;
        texcoord = mad(float2(2.0, 0.0), SMAA_RT_METRICS.xy, texcoord);
    }
    float offset = mad(-(255.0 / 127.0), SMAASearchLength(SMAATexturePass2D(searchTex), e, 0.5), 3.25);
    return mad(-SMAA_RT_METRICS.x, offset, texcoord.x);
}

float SMAASearchYUp(SMAATexture2D(edgesTex), SMAATexture2D(searchTex), float2 texcoord, float end) {
    float2 e = float2(1.0, 0.0);
    while (texcoord.y > end && 
           e.r > 0.8281 && // Is there some edge not activated?
           e.g == 0.0) { // Or is there a crossing edge that breaks the line?
        e = SMAASampleLevelZero(edgesTex, texcoord).rg;
        texcoord = mad(-float2(0.0, 2.0), SMAA_RT_METRICS.xy, texcoord);
    }
    float offset = mad(-(255.0 / 127.0), SMAASearchLength(SMAATexturePass2D(searchTex), e.gr, 0.0), 3.25);
    return mad(SMAA_RT_METRICS.y, offset, texcoord.y);
}

float SMAASearchYDown(SMAATexture2D(edgesTex), SMAATexture2D(searchTex), float2 texcoord, float end) {
    float2 e = float2(1.0, 0.0);
    while (texcoord.y < end && 
           e.r > 0.8281 && // Is there some edge not activated?
           e.g == 0.0) { // Or is there a crossing edge that breaks the line?
        e = SMAASampleLevelZero(edgesTex, texcoord).rg;
        texcoord = mad(float2(0.0, 2.0), SMAA_RT_METRICS.xy, texcoord);
    }
    float offset = mad(-(255.0 / 127.0), SMAASearchLength(SMAATexturePass2D(searchTex), e.gr, 0.5), 3.25);
    return mad(-SMAA_RT_METRICS.y, offset, texcoord.y);
}

/** 
 * Ok, we have the distance and both crossing edges. So, what are the areas
 * at each side of current edge?
 */
float2 SMAAArea(SMAATexture2D(areaTex), float2 dist, float e1, float e2, float offset) {
    // Rounding prevents precision errors of bilinear filtering:
    float2 texcoord = mad(float2(SMAA_AREATEX_MAX_DISTANCE, SMAA_AREATEX_MAX_DISTANCE), round(4.0 * float2(e1, e2)), dist);
    
    // We do a scale and bias for mapping to texel space:
    texcoord = mad(SMAA_AREATEX_PIXEL_SIZE, texcoord, 0.5 * SMAA_AREATEX_PIXEL_SIZE);

    // Move to proper place, according to the subpixel offset:
    texcoord.y = mad(SMAA_AREATEX_SUBTEX_SIZE, offset, texcoord.y);

    // Do it!
    return SMAA_AREATEX_SELECT(SMAASampleLevelZero(areaTex, texcoord));
}

//-----------------------------------------------------------------------------
// Corner Detection Functions

void SMAADetectHorizontalCornerPattern(SMAATexture2D(edgesTex), inout float2 weights, float4 texcoord, float2 d) {
    #if !defined(SMAA_DISABLE_CORNER_DETECTION)
    float2 leftRight = step(d.xy, d.yx);
    float2 rounding = (1.0 - SMAA_CORNER_ROUNDING_NORM) * leftRight;

    rounding /= leftRight.x + leftRight.y; // Reduce blending for pixels in the center of a line.

    float2 factor = float2(1.0, 1.0);
    factor.x -= rounding.x * SMAASampleLevelZeroOffset(edgesTex, texcoord.xy, int2(0,  1)).r;
    factor.x -= rounding.y * SMAASampleLevelZeroOffset(edgesTex, texcoord.zw, int2(1,  1)).r;
    factor.y -= rounding.x * SMAASampleLevelZeroOffset(edgesTex, texcoord.xy, int2(0, -2)).r;
    factor.y -= rounding.y * SMAASampleLevelZeroOffset(edgesTex, texcoord.zw, int2(1, -2)).r;

    weights *= saturate(factor);
    #endif
}

void SMAADetectVerticalCornerPattern(SMAATexture2D(edgesTex), inout float2 weights, float4 texcoord, float2 d) {
    #if !defined(SMAA_DISABLE_CORNER_DETECTION)
    float2 leftRight = step(d.xy, d.yx);
    float2 rounding = (1.0 - SMAA_CORNER_ROUNDING_NORM) * leftRight;

    rounding /= leftRight.x + leftRight.y;

    float2 factor = float2(1.0, 1.0);
    factor.x -= rounding.x * SMAASampleLevelZeroOffset(edgesTex, texcoord.xy, int2( 1, 0)).g;
    factor.x -= rounding.y * SMAASampleLevelZeroOffset(edgesTex, texcoord.zw, int2( 1, 1)).g;
    factor.y -= rounding.x * SMAASampleLevelZeroOffset(edgesTex, texcoord.xy, int2(-2, 0)).g;
    factor.y -= rounding.y * SMAASampleLevelZeroOffset(edgesTex, texcoord.zw, int2(-2, 1)).g;

    weights *= saturate(factor);
    #endif
}

//-----------------------------------------------------------------------------
// Blending Weight Calculation Pixel Shader (Second Pass)

float4 SMAABlendingWeightCalculationPS(float2 texcoord,
                                       float2 pixcoord,
                                       float4 offset[3],
                                       SMAATexture2D(edgesTex),
                                       SMAATexture2D(areaTex),
                                       SMAATexture2D(searchTex),
                                       float4 subsampleIndices) { // Just pass zero for SMAA 1x, see @SUBSAMPLE_INDICES.
    float4 weights = float4(0.0, 0.0, 0.0, 0.0);

    float2 e = SMAASample(edgesTex, texcoord).rg;

    SMAA_BRANCH
    if (e.g > 0.0) { // Edge at north
        #if !defined(SMAA_DISABLE_DIAG_DETECTION)
        // Diagonals have both north and west edges, so searching for them in
        // one of the boundaries is enough.
        weights.rg = SMAACalculateDiagWeights(SMAATexturePass2D(edgesTex), SMAATexturePass2D(areaTex), texcoord, e, subsampleIndices);

        // We give priority to diagonals, so if we find a diagonal we skip 
        // horizontal/vertical processing.
        SMAA_BRANCH
        if (weights.r == -weights.g) { // weights.r + weights.g == 0.0
        #endif

        float2 d;

        // Find the distance to the left:
        float3 coords;
        coords.x = SMAASearchXLeft(SMAATexturePass2D(edgesTex), SMAATexturePass2D(searchTex), offset[0].xy, offset[2].x);
        coords.y = offset[1].y; // offset[1].y = texcoord.y - 0.25 * SMAA_RT_METRICS.y (@CROSSING_OFFSET)
        d.x = coords.x;

        // Now fetch the left crossing edges, two at a time using bilinear
        // filtering. Sampling at -0.25 (see @CROSSING_OFFSET) enables to
        // discern what value each edge has:
        float e1 = SMAASampleLevelZero(edgesTex, coords.xy).r;

        // Find the distance to the right:
        coords.z = SMAASearchXRight(SMAATexturePass2D(edgesTex), SMAATexturePass2D(searchTex), offset[0].zw, offset[2].y);
        d.y = coords.z;

        // We want the distances to be in pixel units (doing this here allow to
        // better interleave arithmetic and memory accesses):
        d = abs(round(mad(SMAA_RT_METRICS.zz, d, -pixcoord.xx)));

        // SMAAArea below needs a sqrt, as the areas texture is compressed
        // quadratically:
        float2 sqrt_d = sqrt(d);

        // Fetch the right crossing edges:
        float e2 = SMAASampleLevelZeroOffset(edgesTex, coords.zy, int2(1, 0)).r;

        // Ok, we know how this pattern looks like, now it is time for getting
        // the actual area:
        weights.rg = SMAAArea(SMAATexturePass2D(areaTex), sqrt_d, e1, e2, subsampleIndices.y);

        // Fix corners:
        coords.y = texcoord.y;
        SMAADetectHorizontalCornerPattern(SMAATexturePass2D(edgesTex), weights.rg, coords.xyzy, d);

        #if !defined(SMAA_DISABLE_DIAG_DETECTION)
        } else
            e.r = 0.0; // Skip vertical processing.
        #endif
    }

    SMAA_BRANCH
    if (e.r > 0.0) { // Edge at west
        float2 d;

        // Find the distance to the top:
        float3 coords;
        coords.y = SMAASearchYUp(SMAATexturePass2D(edgesTex), SMAATexturePass2D(searchTex), offset[1].xy, offset[2].z);
        coords.x = offset[0].x; // offset[1].x = texcoord.x - 0.25 * SMAA_RT_METRICS.x;
        d.x = coords.y;

        // Fetch the top crossing edges:
        float e1 = SMAASampleLevelZero(edgesTex, coords.xy).g;

        // Find the distance to the bottom:
        coords.z = SMAASearchYDown(SMAATexturePass2D(edgesTex), SMAATexturePass2D(searchTex), offset[1].zw, offset[2].w);
        d.y = coords.z;

        // We want the distances to be in pixel units:
        d = abs(round(mad(SMAA_RT_METRICS.ww, d, -pixcoord.yy)));

        // SMAAArea below needs a sqrt, as the areas texture is compressed 
        // quadratically:
        float2 sqrt_d = sqrt(d);

        // Fetch the bottom crossing edges:
        float e2 = SMAASampleLevelZeroOffset(edgesTex, coords.xz, int2(0, 1)).g;

        // Get the area for this direction:
        weights.ba = SMAAArea(SMAATexturePass2D(areaTex), sqrt_d, e1, e2, subsampleIndices.x);

        // Fix corners:
        coords.x = texcoord.x;
        SMAADetectVerticalCornerPattern(SMAATexturePass2D(edgesTex), weights.ba, coords.xyxz, d);
    }

    return weights;
}

//-----------------------------------------------------------------------------
// Neighborhood Blending Pixel Shader (Third Pass)

float4 SMAANeighborhoodBlendingPS(float2 texcoord,
                                  float4 offset,
                                  SMAATexture2D(colorTex),
                                  SMAATexture2D(blendTex)
                                  #if SMAA_REPROJECTION
                                  , SMAATexture2D(velocityTex)
                                  #endif
                                  ) {
    // Fetch the blending weights for current pixel:
    float4 a;
    a.x = SMAASample(blendTex, offset.xy).a; // Right
    a.y = SMAASample(blendTex, offset.zw).g; // Top
    a.wz = SMAASample(blendTex, texcoord).xz; // Bottom / Left

    // Is there any blending weight with a value greater than 0.0?
    SMAA_BRANCH
    if (dot(a, float4(1.0, 1.0, 1.0, 1.0)) < 1e-5) {
        float4 color = SMAASampleLevelZero(colorTex, texcoord);

        #if SMAA_REPROJECTION
        float2 velocity = SMAA_DECODE_VELOCITY(SMAASampleLevelZero(velocityTex, texcoord));

        // Pack velocity into the alpha channel:
        color.a = sqrt(5.0 * length(velocity));
        #endif

        return color;
    } else {
        bool h = max(a.x, a.z) > max(a.y, a.w); // max(horizontal) > max(vertical)

        // Calculate the blending offsets:
        float4 blendingOffset = float4(0.0, a.y, 0.0, a.w);
        float2 blendingWeight = a.yw;
        SMAAMovc(bool4(h, h, h, h), blendingOffset, float4(a.x, 0.0, a.z, 0.0));
        SMAAMovc(bool2(h, h), blendingWeight, a.xz);
        blendingWeight /= dot(blendingWeight, float2(1.0, 1.0));

        // Calculate the texture coordinates:
        float4 blendingCoord = mad(blendingOffset, float4(SMAA_RT_METRICS.xy, -SMAA_RT_METRICS.xy), texcoord.xyxy);

        // We exploit bilinear filtering to mix current pixel with the chosen
        // neighbor:
        float4 color = blendingWeight.x * SMAASampleLevelZero(colorTex, blendingCoord.xy);
        color += blendingWeight.y * SMAASampleLevelZero(colorTex, blendingCoord.zw);

        #if SMAA_REPROJECTION
        // Antialias velocity for proper reprojection in a later stage:
        float2 velocity = blendingWeight.x * SMAA_DECODE_VELOCITY(SMAASampleLevelZero(velocityTex, blendingCoord.xy));
        velocity += blendingWeight.y * SMAA_DECODE_VELOCITY(SMAASampleLevelZero(velocityTex, blendingCoord.zw));

        // Pack velocity into the alpha channel:
        color.a = sqrt(5.0 * length(velocity));
        #endif

        return color;
    }
}

//-----------------------------------------------------------------------------
// Temporal Resolve Pixel Shader (Optional Pass)

float4 SMAAResolvePS(float2 texcoord,
                     SMAATexture2D(currentColorTex),
                     SMAATexture2D(previousColorTex)
                     #if SMAA_REPROJECTION
                     , SMAATexture2D(velocityTex)
                     #endif
                     ) {
    #if SMAA_REPROJECTION
    // Velocity is assumed to be calculated for motion blur, so we need to
    // inverse it for reprojection:
    float2 velocity = -SMAA_DECODE_VELOCITY(SMAASamplePoint(velocityTex, texcoord).rg);

    // Fetch current pixel:
    float4 current = SMAASamplePoint(currentColorTex, texcoord);

    // Reproject current coordinates and fetch previous pixel:
    float4 previous = SMAASamplePoint(previousColorTex, texcoord + velocity);

    // Attenuate the previous pixel if the velocity is different:
    float delta = abs(current.a * current.a - previous.a * previous.a) / 5.0;
    float weight = 0.5 * saturate(1.0 - sqrt(delta) * SMAA_REPROJECTION_WEIGHT_SCALE);

    // Blend the pixels according to the calculated weight:
    return lerp(current, previous, weight);
    #else
    // Just blend the pixels:
    float4 current = SMAASamplePoint(currentColorTex, texcoord);
    float4 previous = SMAASamplePoint(previousColorTex, texcoord);
    return lerp(current, previous, 0.5);
    #endif
}

//-----------------------------------------------------------------------------
// Separate Multisamples Pixel Shader (Optional Pass)

#ifdef SMAALoad
void SMAASeparatePS(float4 position,
                    float2 texcoord,
                    out float4 target0,
                    out float4 target1,
                    SMAATexture2DMS2(colorTexMS)) {
    int2 pos = int2(position.xy);
    target0 = SMAALoad(colorTexMS, pos, 0);
    target1 = SMAALoad(colorTexMS, pos, 1);
}
#endif

//-----------------------------------------------------------------------------
#endif // SMAA_INCLUDE_PS
)SMAA_FXH";

// Pass wrappers around SMAA.fxh. Vertex input: clip-space position (already shifted
// by D3D9's half pixel on the CPU) and a 0..1 texcoord.
const char k_hlsl_smaa_glue[] = R"HLSL(
void PfxQuadVS(float4 pos : POSITION, float2 tc : TEXCOORD0,
               out float4 oPos : POSITION, out float2 oTc : TEXCOORD0) {
    oPos = pos;
    oTc = tc;
}

void PfxEdgeVS(float4 pos : POSITION, float2 tc : TEXCOORD0,
               out float4 oPos : POSITION, out float2 oTc : TEXCOORD0,
               out float4 oOffset[3] : TEXCOORD1) {
    oPos = pos;
    oTc = tc;
    SMAAEdgeDetectionVS(tc, oOffset);
}

void PfxWeightVS(float4 pos : POSITION, float2 tc : TEXCOORD0,
                 out float4 oPos : POSITION, out float2 oTc : TEXCOORD0,
                 out float2 oPix : TEXCOORD1, out float4 oOffset[3] : TEXCOORD2) {
    oPos = pos;
    oTc = tc;
    SMAABlendingWeightCalculationVS(tc, oPix, oOffset);
}

void PfxBlendVS(float4 pos : POSITION, float2 tc : TEXCOORD0,
                out float4 oPos : POSITION, out float2 oTc : TEXCOORD0,
                out float4 oOffset : TEXCOORD1) {
    oPos = pos;
    oTc = tc;
    SMAANeighborhoodBlendingVS(tc, oOffset);
}

// s0 = color (gamma, not sRGB-decoded)
float4 PfxLumaEdgePS(float2 tc : TEXCOORD0, float4 offset[3] : TEXCOORD1) : COLOR0 {
    return float4(SMAALumaEdgeDetectionPS(tc, offset, pfx_s0), 0.0, 0.0);
}
float4 PfxColorEdgePS(float2 tc : TEXCOORD0, float4 offset[3] : TEXCOORD1) : COLOR0 {
    return float4(SMAAColorEdgeDetectionPS(tc, offset, pfx_s0), 0.0, 0.0);
}

// s0 = edges (linear filter), s1 = AreaTex (linear), s2 = SearchTex (point)
float4 PfxWeightPS(float2 tc : TEXCOORD0, float2 pix : TEXCOORD1,
                   float4 offset[3] : TEXCOORD2) : COLOR0 {
    return SMAABlendingWeightCalculationPS(tc, pix, offset, pfx_s0, pfx_s1, pfx_s2,
                                           float4(0.0, 0.0, 0.0, 0.0));
}

// s0 = color (linear filter, sRGB-decoded when available), s1 = blend weights
float4 PfxBlendPS(float2 tc : TEXCOORD0, float4 offset : TEXCOORD1) : COLOR0 {
    return SMAANeighborhoodBlendingPS(tc, offset, pfx_s0, pfx_s1);
}
)HLSL";

// AMD FidelityFX CAS, ported from SweetFX CAS.fx (its D3D9 path): tex2Doffset became
// explicit texel offsets and rcp() (SM5 only) became 1.0 / x. Constants from c1.
const char k_hlsl_cas[] = R"HLSL(
// LICENSE
// =======
// Copyright (c) 2017-2019 Advanced Micro Devices, Inc. All rights reserved.
// -------
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation
// files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy,
// modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
// -------
// The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
// Software.
// -------
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
// WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR
// COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
// ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE

// ReShade port by SLSNe, with optimizations and fixes by Marty McFly, OopyDoopy,
// Lord of Lunacy and CeeJay.dk (SweetFX CAS.fx).

// s0 = color (sRGB-decoded when available, i.e. CAS works in linear space)
float4 PfxCasPS(float2 texcoord : TEXCOORD0) : COLOR0
{
    float2 px = pfx_rt_metrics.xy;

    // fetch a 3x3 neighborhood around the pixel 'e',
    //  a b c
    //  d(e)f
    //  g h i
    float3 b = tex2D(pfx_s0, texcoord + px * float2( 0.0, -1.0)).rgb;
    float3 d = tex2D(pfx_s0, texcoord + px * float2(-1.0,  0.0)).rgb;
    float3 e = tex2D(pfx_s0, texcoord).rgb;
    float3 f = tex2D(pfx_s0, texcoord + px * float2( 1.0,  0.0)).rgb;
    float3 h = tex2D(pfx_s0, texcoord + px * float2( 0.0,  1.0)).rgb;
    float3 i = tex2D(pfx_s0, texcoord + px * float2( 1.0,  1.0)).rgb;
    float3 g = tex2D(pfx_s0, texcoord + px * float2(-1.0,  1.0)).rgb;
    float3 a = tex2D(pfx_s0, texcoord + px * float2(-1.0, -1.0)).rgb;
    float3 c = tex2D(pfx_s0, texcoord + px * float2( 1.0, -1.0)).rgb;

    // Soft min and max.
    //  a b c             b
    //  d e f * 0.5  +  d e f * 0.5
    //  g h i             h
    // These are 2.0x bigger (factored out the extra multiply).
    float3 mnRGB = min(min(min(d, e), min(f, b)), h);
    float3 mnRGB2 = min(mnRGB, min(min(a, c), min(g, i)));
    mnRGB += mnRGB2;

    float3 mxRGB = max(max(max(d, e), max(f, b)), h);
    float3 mxRGB2 = max(mxRGB, max(max(a, c), max(g, i)));
    mxRGB += mxRGB2;

    // Smooth minimum distance to signal limit divided by smooth max.
    float3 rcpMRGB = 1.0 / mxRGB;
    float3 ampRGB = saturate(min(mnRGB, 2.0 - mxRGB) * rcpMRGB);

    // Shaping amount of sharpening.
    ampRGB = rsqrt(ampRGB);

    float peak = -3.0 * pfx_cas.x + 8.0;
    float3 wRGB = -1.0 / (ampRGB * peak);

    float3 rcpWeightRGB = 1.0 / (4.0 * wRGB + 1.0);

    //                          0 w 0
    //  Filter shape:           w 1 w
    //                          0 w 0
    float3 window = (b + d) + (f + h);
    float3 outColor = saturate((window * wRGB + e) * rcpWeightRGB);

    return float4(lerp(e, outColor, pfx_cas.y), 1.0);
}
)HLSL";

// Vibrance (written for this port): a saturation boost around the pixel's luma, scaled
// by how unsaturated the pixel already is, so dull colors gain more than vivid ones.
// s0 = color (gamma), c2.x = strength (0 = off, negative desaturates).
const char k_hlsl_vibrance[] = R"HLSL(
float4 PfxVibrancePS(float2 tc : TEXCOORD0) : COLOR0 {
    float4 color = tex2D(pfx_s0, tc);
    float luma = dot(color.rgb, float3(0.2126, 0.7152, 0.0722));
    float hi = max(color.r, max(color.g, color.b));
    float lo = min(color.r, min(color.g, color.b));
    float saturation = hi - lo;
    float amount = 1.0 + pfx_vibrance.x * (1.0 - saturation);
    return float4(saturate(lerp(luma.xxx, color.rgb, amount)), 1.0);
}
)HLSL";

enum { VS_QUAD, VS_EDGE, VS_WEIGHT, VS_BLEND, VS_COUNT };
enum { PS_LUMA_EDGE, PS_COLOR_EDGE, PS_WEIGHT, PS_BLEND, PS_CAS, PS_VIBRANCE, PS_COUNT };
const char *const k_vs_entry[VS_COUNT] = {"PfxQuadVS", "PfxEdgeVS", "PfxWeightVS", "PfxBlendVS"};
const char *const k_ps_entry[PS_COUNT] = {"PfxLumaEdgePS", "PfxColorEdgePS", "PfxWeightPS",
                                          "PfxBlendPS",    "PfxCasPS",       "PfxVibrancePS"};

std::string postfx_hlsl_source() {
    std::string s;
    s += k_hlsl_prelude;
    s += k_smaa_fxh;
    s += k_hlsl_smaa_glue;
    s += k_hlsl_cas;
    s += k_hlsl_vibrance;
    return s;
}

// ============================================================================ state
struct Config {
    bool ini_found = false;
    bool enabled = false;
    bool smaa = true;
    bool smaa_color_edges = false;  // false: luma edge detection
    bool cas = true;
    float cas_sharpness = 1.0f;     // CAS.fx "Sharpening intensity" default
    float cas_contrast = 0.0f;      // CAS.fx "Contrast Adaptation" default
    bool vibrance = true;
    float vibrance_strength = 0.15f;
};

std::recursive_mutex g_mu;
bool g_cfg_loaded;
Config g_cfg;
bool g_on;         // current toggle state (Scroll Lock flips it)
bool g_broken;     // compiler missing / compile or setup failed: off for the session
bool g_scroll_down;
bool g_logged_pure, g_logged_caps, g_logged_rt;

bool g_compiled;
std::vector<DWORD> g_vs_code[VS_COUNT], g_ps_code[PS_COUNT];

// Resources, all owned by g_dev. The pass never AddRefs the device itself, but every
// child object holds a device reference; g_*_refs count those (measured, see dev_refs).
IDirect3DDevice9 *g_dev;
IDirect3DVertexShader9 *g_vs[VS_COUNT];
IDirect3DPixelShader9 *g_ps[PS_COUNT];
IDirect3DVertexDeclaration9 *g_decl;
IDirect3DTexture9 *g_area, *g_search;          // D3DPOOL_MANAGED: survive Reset
ULONG g_static_refs;
bool g_static_ok;
IDirect3DTexture9 *g_color[2], *g_edges, *g_blend;  // D3DPOOL_DEFAULT render targets
UINT g_rt_w, g_rt_h, g_rt_fail_w, g_rt_fail_h;
D3DFORMAT g_rt_fmt, g_rt_bb_fmt;
bool g_srgb;
ULONG g_rt_refs;

template <class T> void safe_release(T *&p) {
    if (p) { p->Release(); p = nullptr; }
}

ULONG dev_refs(IDirect3DDevice9 *d) {
    d->AddRef();
    return d->Release();
}

ULONG refs_delta(ULONG before, ULONG after) { return after > before ? after - before : 0; }

// ============================================================================ config
std::wstring ini_path() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof buf / sizeof buf[0]));
    if (!n || n >= sizeof buf / sizeof buf[0]) return L"";
    std::wstring p(buf, n);
    size_t slash = p.find_last_of(L"\\/");
    p = slash == std::wstring::npos ? std::wstring() : p.substr(0, slash + 1);
    return p + L"lb_paths.ini";
}

bool ini_str(const std::wstring &ini, const wchar_t *key, wchar_t *out, DWORD n) {
    out[0] = 0;
    GetPrivateProfileStringW(L"postfx", key, L"", out, n, ini.c_str());
    // trim
    wchar_t *s = out;
    while (*s == L' ' || *s == L'\t') s++;
    if (s != out) memmove(out, s, (wcslen(s) + 1) * sizeof(wchar_t));
    size_t len = wcslen(out);
    while (len && (out[len - 1] == L' ' || out[len - 1] == L'\t')) out[--len] = 0;
    return out[0] != 0;
}

bool ini_bool(const std::wstring &ini, const wchar_t *key, bool def) {
    wchar_t b[64];
    if (!ini_str(ini, key, b, 64)) return def;
    if (!_wcsicmp(b, L"true") || !_wcsicmp(b, L"on") || !_wcsicmp(b, L"yes")) return true;
    if (!_wcsicmp(b, L"false") || !_wcsicmp(b, L"off") || !_wcsicmp(b, L"no")) return false;
    return wcstol(b, nullptr, 10) != 0;
}

float ini_float(const std::wstring &ini, const wchar_t *key, float def, float lo, float hi) {
    wchar_t b[64];
    if (!ini_str(ini, key, b, 64)) return def;
    wchar_t *end = nullptr;
    double v = wcstod(b, &end);
    if (end == b || !isfinite(v)) return def;
    return v < lo ? lo : v > hi ? hi : (float)v;
}

// [postfx] keys and defaults:
//   enabled            1 when lb_paths.ini exists, else 0 (LB_POSTFX=0/1 overrides)
//   smaa               1      SMAA 1x, preset HIGH
//   smaa_edges         luma   edge detection: luma or color
//   cas                1      AMD Contrast Adaptive Sharpening
//   cas_sharpness      1.0    CAS.fx "Sharpening intensity" (0..1)
//   cas_contrast       0.0    CAS.fx "Contrast Adaptation" (0..1)
//   vibrance           1
//   vibrance_strength  0.15   (-1..1)
void load_config() {
    if (g_cfg_loaded) return;
    g_cfg_loaded = true;
    Config c;
    std::wstring ini = ini_path();
    c.ini_found = !ini.empty() && GetFileAttributesW(ini.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (c.ini_found) {
        c.enabled = ini_bool(ini, L"enabled", true);
        c.smaa = ini_bool(ini, L"smaa", c.smaa);
        wchar_t b[64];
        if (ini_str(ini, L"smaa_edges", b, 64)) c.smaa_color_edges = !_wcsicmp(b, L"color");
        c.cas = ini_bool(ini, L"cas", c.cas);
        c.cas_sharpness = ini_float(ini, L"cas_sharpness", c.cas_sharpness, 0.0f, 1.0f);
        c.cas_contrast = ini_float(ini, L"cas_contrast", c.cas_contrast, 0.0f, 1.0f);
        c.vibrance = ini_bool(ini, L"vibrance", c.vibrance);
        c.vibrance_strength = ini_float(ini, L"vibrance_strength", c.vibrance_strength, -1.0f, 1.0f);
    }
    const char *env = getenv("LB_POSTFX");
    if (env && *env) c.enabled = atoi(env) != 0;
    g_cfg = c;
    g_on = c.enabled;
    LOG("postfx: %s (lb_paths.ini %s%s): smaa=%d (%s edges) cas=%d (sharpness %.2f, contrast %.2f) "
        "vibrance=%d (%.2f); Scroll Lock toggles",
        c.enabled ? "enabled" : "disabled", c.ini_found ? "found" : "not found",
        env && *env ? ", LB_POSTFX set" : "", c.smaa, c.smaa_color_edges ? "color" : "luma", c.cas,
        c.cas_sharpness, c.cas_contrast, c.vibrance, c.vibrance_strength);
}

// ============================================================================ compile
typedef HRESULT(WINAPI *PFN_D3DCompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *,
                                        LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);

bool compile_one(PFN_D3DCompile fn, const std::string &src, const char *entry, const char *target,
                 std::vector<DWORD> &out) {
    ID3DBlob *code = nullptr, *err = nullptr;
    HRESULT hr = fn(src.data(), src.size(), "postfx.hlsl", nullptr, nullptr, entry, target,
                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr) || !code) {
        LOG("postfx: compiling %s (%s) failed (%08lx): %s", entry, target, (unsigned long)hr,
            err ? (const char *)err->GetBufferPointer() : "(no message)");
        safe_release(code);
        safe_release(err);
        return false;
    }
    safe_release(err);
    size_t n = code->GetBufferSize();
    out.assign((n + 3) / 4, 0);
    memcpy(out.data(), code->GetBufferPointer(), n);
    code->Release();
    return true;
}

bool need_vs(int i) {
    switch (i) {
    case VS_QUAD: return g_cfg.cas || g_cfg.vibrance;
    default: return g_cfg.smaa;
    }
}
bool need_ps(int i) {
    switch (i) {
    case PS_LUMA_EDGE: return g_cfg.smaa && !g_cfg.smaa_color_edges;
    case PS_COLOR_EDGE: return g_cfg.smaa && g_cfg.smaa_color_edges;
    case PS_WEIGHT: case PS_BLEND: return g_cfg.smaa;
    case PS_CAS: return g_cfg.cas;
    default: return g_cfg.vibrance;
    }
}

bool compile_all() {
    if (g_compiled) return true;
    LARGE_INTEGER t0, t1, f;
    QueryPerformanceCounter(&t0);
    HMODULE m = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m) m = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!m) {
        LOG("postfx: d3dcompiler_47.dll not found (error %lu); post-processing disabled",
            (unsigned long)GetLastError());
        return false;
    }
    auto fn = (PFN_D3DCompile)(void *)GetProcAddress(m, "D3DCompile");
    if (!fn) {
        LOG("postfx: D3DCompile missing from d3dcompiler_47.dll; post-processing disabled");
        FreeLibrary(m);
        return false;
    }
    std::string src = postfx_hlsl_source();
    bool ok = true;
    for (int i = 0; i < VS_COUNT && ok; i++)
        if (need_vs(i)) ok = compile_one(fn, src, k_vs_entry[i], "vs_3_0", g_vs_code[i]);
    for (int i = 0; i < PS_COUNT && ok; i++)
        if (need_ps(i)) ok = compile_one(fn, src, k_ps_entry[i], "ps_3_0", g_ps_code[i]);
    FreeLibrary(m);
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    if (!ok) {
        LOG("postfx: shader compilation failed; post-processing disabled");
        return false;
    }
    LOG("postfx: shaders compiled in %.0f ms", (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart);
    g_compiled = true;
    return true;
}

// ============================================================================ resources
void release_targets() {
    safe_release(g_color[0]);
    safe_release(g_color[1]);
    safe_release(g_edges);
    safe_release(g_blend);
    g_rt_w = g_rt_h = 0;
    g_rt_refs = 0;
}

void release_all() {
    release_targets();
    for (auto &v : g_vs) safe_release(v);
    for (auto &p : g_ps) safe_release(p);
    safe_release(g_decl);
    safe_release(g_area);
    safe_release(g_search);
    g_static_refs = 0;
    g_static_ok = false;
    g_dev = nullptr;
}

struct DevInfo {
    IDirect3D9 *d3d = nullptr;
    D3DDEVICE_CREATION_PARAMETERS cp{};
    D3DFORMAT adapter_fmt = D3DFMT_X8R8G8B8;
    ~DevInfo() { safe_release(d3d); }
    bool init(IDirect3DDevice9 *dev) {
        if (FAILED(dev->GetDirect3D(&d3d)) || !d3d) return false;
        if (FAILED(dev->GetCreationParameters(&cp))) return false;
        D3DDISPLAYMODE m{};
        if (SUCCEEDED(d3d->GetAdapterDisplayMode(cp.AdapterOrdinal, &m)) && m.Format) adapter_fmt = m.Format;
        return true;
    }
    bool check(DWORD usage, D3DRESOURCETYPE type, D3DFORMAT fmt) const {
        return SUCCEEDED(d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, adapter_fmt, usage, type, fmt));
    }
};

// AreaTex (RG8) as A8L8 (L = R, A = G: the byte order of the data) or, without A8L8,
// as A8R8G8B8 with R = G = B = r and A = g; SMAA's HLSL3 path reads .ra either way.
// SearchTex (R8) as L8 or A8R8G8B8 with r in all color channels; read as .r.
IDirect3DTexture9 *make_lookup(IDirect3DDevice9 *dev, const DevInfo &di, const unsigned char *src,
                               UINT w, UINT h, int channels) {
    D3DFORMAT packed = channels == 2 ? D3DFMT_A8L8 : D3DFMT_L8;
    D3DFORMAT fmt = di.check(0, D3DRTYPE_TEXTURE, packed) ? packed : D3DFMT_A8R8G8B8;
    IDirect3DTexture9 *t = nullptr;
    if (FAILED(dev->CreateTexture(w, h, 1, 0, fmt, D3DPOOL_MANAGED, &t, nullptr)) || !t) return nullptr;
    D3DLOCKED_RECT lr;
    if (FAILED(t->LockRect(0, &lr, nullptr, 0))) { t->Release(); return nullptr; }
    for (UINT y = 0; y < h; y++) {
        unsigned char *row = (unsigned char *)lr.pBits + (size_t)y * lr.Pitch;
        const unsigned char *s = src + (size_t)y * w * channels;
        if (fmt == packed) {
            memcpy(row, s, (size_t)w * channels);
        } else {
            uint32_t *px = (uint32_t *)row;
            for (UINT x = 0; x < w; x++) {
                uint32_t r = s[x * channels], a = channels == 2 ? s[x * channels + 1] : 255;
                px[x] = a << 24 | r << 16 | r << 8 | r;
            }
        }
    }
    t->UnlockRect(0);
    return t;
}

bool create_static(IDirect3DDevice9 *dev, const DevInfo &di) {
    if (g_static_ok) return true;
    D3DCAPS9 caps{};
    dev->GetDeviceCaps(&caps);
    if (caps.PixelShaderVersion < D3DPS_VERSION(3, 0) || caps.VertexShaderVersion < D3DVS_VERSION(3, 0)) {
        LOG("postfx: device lacks vs_3_0/ps_3_0 (vs %lx ps %lx); post-processing disabled",
            (unsigned long)caps.VertexShaderVersion, (unsigned long)caps.PixelShaderVersion);
        return false;
    }
    ULONG before = dev_refs(dev);
    bool ok = true;
    for (int i = 0; i < VS_COUNT && ok; i++)
        if (!g_vs_code[i].empty()) ok = SUCCEEDED(dev->CreateVertexShader(g_vs_code[i].data(), &g_vs[i]));
    for (int i = 0; i < PS_COUNT && ok; i++)
        if (!g_ps_code[i].empty()) ok = SUCCEEDED(dev->CreatePixelShader(g_ps_code[i].data(), &g_ps[i]));
    if (!ok) LOG("postfx: creating shaders failed; post-processing disabled");
    if (ok) {
        const D3DVERTEXELEMENT9 el[] = {
            {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
            {0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
            D3DDECL_END()};
        ok = SUCCEEDED(dev->CreateVertexDeclaration(el, &g_decl));
        if (!ok) LOG("postfx: CreateVertexDeclaration failed; post-processing disabled");
    }
    if (ok && g_cfg.smaa) {
        g_area = make_lookup(dev, di, smaa_area_tex, SMAA_AREATEX_WIDTH, SMAA_AREATEX_HEIGHT, 2);
        g_search = make_lookup(dev, di, smaa_search_tex, SMAA_SEARCHTEX_WIDTH, SMAA_SEARCHTEX_HEIGHT, 1);
        ok = g_area && g_search;
        if (!ok) LOG("postfx: creating the SMAA lookup textures failed; post-processing disabled");
        D3DSURFACE_DESC a{}, s{};
        if (ok && SUCCEEDED(g_area->GetLevelDesc(0, &a)) && SUCCEEDED(g_search->GetLevelDesc(0, &s)))
            LOG("postfx: SMAA lookup textures: AreaTex format %d, SearchTex format %d", (int)a.Format,
                (int)s.Format);
    }
    g_static_refs = refs_delta(before, dev_refs(dev));
    if (!ok) {
        release_all();
        return false;
    }
    g_static_ok = true;
    return true;
}

bool create_targets(IDirect3DDevice9 *dev, const DevInfo &di, const D3DSURFACE_DESC &bb) {
    if (g_color[0] && g_rt_w == bb.Width && g_rt_h == bb.Height && g_rt_bb_fmt == bb.Format) return true;
    if (g_rt_fail_w == bb.Width && g_rt_fail_h == bb.Height) return false;  // don't retry every frame
    release_targets();
    D3DFORMAT cf = bb.Format;
    if (!di.check(D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, cf)) cf = D3DFMT_A8R8G8B8;
    ULONG before = dev_refs(dev);
    bool ok = true;
    for (int i = 0; i < 2 && ok; i++)
        ok = SUCCEEDED(dev->CreateTexture(bb.Width, bb.Height, 1, D3DUSAGE_RENDERTARGET, cf, D3DPOOL_DEFAULT,
                                          &g_color[i], nullptr));
    if (ok && g_cfg.smaa)
        ok = SUCCEEDED(dev->CreateTexture(bb.Width, bb.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                          D3DPOOL_DEFAULT, &g_edges, nullptr)) &&
             SUCCEEDED(dev->CreateTexture(bb.Width, bb.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                          D3DPOOL_DEFAULT, &g_blend, nullptr));
    g_rt_refs = refs_delta(before, dev_refs(dev));
    if (!ok) {
        if (!g_logged_rt) LOG("postfx: creating %ux%u render targets failed; skipping", bb.Width, bb.Height);
        g_logged_rt = true;
        release_targets();
        g_rt_fail_w = bb.Width;
        g_rt_fail_h = bb.Height;
        return false;
    }
    g_rt_fail_w = g_rt_fail_h = 0;
    g_rt_w = bb.Width;
    g_rt_h = bb.Height;
    g_rt_fmt = cf;
    g_rt_bb_fmt = bb.Format;
    // Linear-light filtering for the blend and CAS stages, as the ReShade versions do
    // (SRGBTexture / SRGBWriteEnable); without format support everything stays gamma.
    g_srgb = di.check(D3DUSAGE_QUERY_SRGBREAD, D3DRTYPE_TEXTURE, cf) &&
             di.check(D3DUSAGE_RENDERTARGET | D3DUSAGE_QUERY_SRGBWRITE, D3DRTYPE_TEXTURE, cf) &&
             di.check(D3DUSAGE_RENDERTARGET | D3DUSAGE_QUERY_SRGBWRITE, D3DRTYPE_SURFACE, bb.Format);
    LOG("postfx: %ux%u targets (format %d, back buffer %d, sRGB %s)", bb.Width, bb.Height, (int)cf,
        (int)bb.Format, g_srgb ? "yes" : "no");
    return true;
}

// ============================================================================ state save/restore
const D3DRENDERSTATETYPE k_rs[] = {
    D3DRS_ZENABLE,          D3DRS_ZWRITEENABLE,     D3DRS_STENCILENABLE,  D3DRS_ALPHABLENDENABLE,
    D3DRS_ALPHATESTENABLE,  D3DRS_CULLMODE,         D3DRS_FILLMODE,       D3DRS_SCISSORTESTENABLE,
    D3DRS_COLORWRITEENABLE, D3DRS_SRGBWRITEENABLE,  D3DRS_FOGENABLE,      D3DRS_CLIPPLANEENABLE,
    D3DRS_MULTISAMPLEMASK,  D3DRS_SEPARATEALPHABLENDENABLE,
};
const DWORD k_rs_val[] = {
    D3DZB_FALSE, FALSE, FALSE, FALSE,
    FALSE, D3DCULL_NONE, D3DFILL_SOLID, FALSE,
    0xF, FALSE, FALSE, 0,
    0xFFFFFFFF, FALSE,
};
const int k_nrs = sizeof k_rs / sizeof k_rs[0];
static_assert(sizeof k_rs / sizeof k_rs[0] == sizeof k_rs_val / sizeof k_rs_val[0], "render state table");

const D3DSAMPLERSTATETYPE k_ss[] = {D3DSAMP_ADDRESSU,  D3DSAMP_ADDRESSV,  D3DSAMP_MAGFILTER,
                                    D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER, D3DSAMP_SRGBTEXTURE,
                                    D3DSAMP_MAXMIPLEVEL, D3DSAMP_MIPMAPLODBIAS};
const int k_nss = sizeof k_ss / sizeof k_ss[0];
const int k_stages = 4;     // samplers 0-2 are used; 0-3 saved
const int k_max_rts = 4;

struct Saved {
    IDirect3DSurface9 *rt[k_max_rts] = {};
    IDirect3DSurface9 *ds = nullptr;
    D3DVIEWPORT9 vp{};
    RECT scissor{};
    IDirect3DVertexShader9 *vs = nullptr;
    IDirect3DPixelShader9 *ps = nullptr;
    IDirect3DVertexDeclaration9 *decl = nullptr;
    DWORD fvf = 0;
    IDirect3DVertexBuffer9 *vb = nullptr;
    UINT vb_off = 0, vb_stride = 0, freq = 1;
    IDirect3DBaseTexture9 *tex[k_stages] = {};
    DWORD ss[k_stages][k_nss] = {};
    DWORD rs[k_nrs] = {};
    float vsc[1][4] = {};
    float psc[3][4] = {};
    int nrt = 1;
};

void save_state(IDirect3DDevice9 *d, Saved &s, int nrt) {
    s.nrt = nrt;
    for (int i = 0; i < nrt; i++)
        if (FAILED(d->GetRenderTarget(i, &s.rt[i]))) s.rt[i] = nullptr;
    if (FAILED(d->GetDepthStencilSurface(&s.ds))) s.ds = nullptr;
    d->GetViewport(&s.vp);
    d->GetScissorRect(&s.scissor);
    d->GetVertexShader(&s.vs);
    d->GetPixelShader(&s.ps);
    d->GetVertexDeclaration(&s.decl);
    d->GetFVF(&s.fvf);
    d->GetStreamSource(0, &s.vb, &s.vb_off, &s.vb_stride);
    d->GetStreamSourceFreq(0, &s.freq);
    for (int t = 0; t < k_stages; t++) {
        d->GetTexture(t, &s.tex[t]);
        for (int k = 0; k < k_nss; k++) d->GetSamplerState(t, k_ss[k], &s.ss[t][k]);
    }
    for (int k = 0; k < k_nrs; k++) d->GetRenderState(k_rs[k], &s.rs[k]);
    d->GetVertexShaderConstantF(0, &s.vsc[0][0], 1);
    d->GetPixelShaderConstantF(0, &s.psc[0][0], 3);
}

void restore_state(IDirect3DDevice9 *d, Saved &s) {
    // SetRenderTarget(0) resets the viewport and scissor rect: targets first.
    if (s.rt[0]) d->SetRenderTarget(0, s.rt[0]);
    for (int i = 1; i < s.nrt; i++) d->SetRenderTarget(i, s.rt[i]);
    d->SetDepthStencilSurface(s.ds);
    d->SetViewport(&s.vp);
    d->SetScissorRect(&s.scissor);
    d->SetVertexShader(s.vs);
    d->SetPixelShader(s.ps);
    if (s.fvf) d->SetFVF(s.fvf);
    else d->SetVertexDeclaration(s.decl);
    d->SetStreamSource(0, s.vb, s.vb_off, s.vb_stride);
    d->SetStreamSourceFreq(0, s.freq);
    for (int t = 0; t < k_stages; t++) {
        d->SetTexture(t, s.tex[t]);
        for (int k = 0; k < k_nss; k++) d->SetSamplerState(t, k_ss[k], s.ss[t][k]);
    }
    for (int k = 0; k < k_nrs; k++) d->SetRenderState(k_rs[k], s.rs[k]);
    d->SetVertexShaderConstantF(0, &s.vsc[0][0], 1);
    d->SetPixelShaderConstantF(0, &s.psc[0][0], 3);
    for (auto &r : s.rt) safe_release(r);
    safe_release(s.ds);
    safe_release(s.vs);
    safe_release(s.ps);
    safe_release(s.decl);
    safe_release(s.vb);
    for (auto &t : s.tex) safe_release(t);
}

// ============================================================================ passes
void bind(IDirect3DDevice9 *d, int stage, IDirect3DTexture9 *t, bool linear, bool srgb) {
    d->SetTexture(stage, t);
    DWORD f = linear ? D3DTEXF_LINEAR : D3DTEXF_POINT;
    d->SetSamplerState(stage, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    d->SetSamplerState(stage, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    d->SetSamplerState(stage, D3DSAMP_MAGFILTER, f);
    d->SetSamplerState(stage, D3DSAMP_MINFILTER, f);
    d->SetSamplerState(stage, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    d->SetSamplerState(stage, D3DSAMP_SRGBTEXTURE, srgb);
    d->SetSamplerState(stage, D3DSAMP_MAXMIPLEVEL, 0);
    d->SetSamplerState(stage, D3DSAMP_MIPMAPLODBIAS, 0);
}

void set_target(IDirect3DDevice9 *d, IDirect3DTexture9 *t, IDirect3DSurface9 *bb) {
    if (!t) {
        d->SetRenderTarget(0, bb);
        return;
    }
    IDirect3DSurface9 *s = nullptr;
    if (SUCCEEDED(t->GetSurfaceLevel(0, &s)) && s) {
        d->SetRenderTarget(0, s);
        s->Release();
    }
}

void draw_quad(IDirect3DDevice9 *d, int vs, int ps, UINT w, UINT h) {
    d->SetVertexShader(g_vs[vs]);
    d->SetPixelShader(g_ps[ps]);
    // D3D9 puts pixel centers on integer coordinates: shift the quad by half a pixel
    // (1/w, 1/h in clip space) so texcoords land on texel centers.
    float dx = 1.0f / w, dy = 1.0f / h;
    const float q[4][6] = {
        {-1.0f - dx, 1.0f + dy, 0.0f, 1.0f, 0.0f, 0.0f},
        {1.0f - dx, 1.0f + dy, 0.0f, 1.0f, 1.0f, 0.0f},
        {-1.0f - dx, -1.0f + dy, 0.0f, 1.0f, 0.0f, 1.0f},
        {1.0f - dx, -1.0f + dy, 0.0f, 1.0f, 1.0f, 1.0f},
    };
    d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof q[0]);
}

void run_passes(IDirect3DDevice9 *d, IDirect3DSurface9 *bb, UINT w, UINT h) {
    // Common state. Stream 0 is consumed by DrawPrimitiveUP (and reset to NULL by it).
    for (int k = 0; k < k_nrs; k++) d->SetRenderState(k_rs[k], k_rs_val[k]);
    for (int i = 1; i < k_max_rts; i++) d->SetRenderTarget(i, nullptr);
    d->SetDepthStencilSurface(nullptr);
    d->SetVertexDeclaration(g_decl);
    d->SetStreamSourceFreq(0, 1);
    for (int t = 0; t < k_stages; t++) d->SetTexture(t, nullptr);
    const float metrics[4] = {1.0f / w, 1.0f / h, (float)w, (float)h};
    const float pconst[3][4] = {
        {metrics[0], metrics[1], metrics[2], metrics[3]},
        {g_cfg.cas_contrast, g_cfg.cas_sharpness, 0.0f, 0.0f},
        {g_cfg.vibrance_strength, 0.0f, 0.0f, 0.0f},
    };
    d->SetVertexShaderConstantF(0, metrics, 1);
    d->SetPixelShaderConstantF(0, &pconst[0][0], 3);

    struct Stage { int vs, ps; bool srgb, blend; };
    Stage stages[3];
    int n = 0;
    if (g_cfg.smaa) stages[n++] = {VS_BLEND, PS_BLEND, g_srgb, true};
    if (g_cfg.cas) stages[n++] = {VS_QUAD, PS_CAS, g_srgb, false};
    if (g_cfg.vibrance) stages[n++] = {VS_QUAD, PS_VIBRANCE, false, false};

    HRESULT scene = d->BeginScene();  // fails if the game is still inside a scene: draw anyway
    if (g_cfg.smaa) {
        // 1. Edge detection (gamma color) -> edges
        set_target(d, g_edges, bb);
        d->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
        bind(d, 0, g_color[0], true, false);
        draw_quad(d, VS_EDGE, g_cfg.smaa_color_edges ? PS_COLOR_EDGE : PS_LUMA_EDGE, w, h);
        // 2. Blending weights -> blend
        set_target(d, g_blend, bb);
        d->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
        bind(d, 0, g_edges, true, false);
        bind(d, 1, g_area, true, false);
        bind(d, 2, g_search, false, false);
        draw_quad(d, VS_WEIGHT, PS_WEIGHT, w, h);
        d->SetTexture(2, nullptr);
    }
    int cur = 0;
    for (int i = 0; i < n; i++) {
        const Stage &st = stages[i];
        bool last = i == n - 1;
        set_target(d, last ? nullptr : g_color[cur ^ 1], bb);
        d->SetRenderState(D3DRS_SRGBWRITEENABLE, st.srgb);
        bind(d, 0, g_color[cur], true, st.srgb);
        if (st.blend) bind(d, 1, g_blend, true, false);
        else d->SetTexture(1, nullptr);
        draw_quad(d, st.vs, st.ps, w, h);
        cur ^= 1;
    }
    if (SUCCEEDED(scene)) d->EndScene();
}

void poll_toggle() {
    bool down = (GetAsyncKeyState(VK_SCROLL) & 0x8000) != 0;
    bool pressed = down && !g_scroll_down;
    g_scroll_down = down;
    if (!pressed) return;
    // Only when one of our windows has the focus.
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    if (pid != GetCurrentProcessId()) return;
    g_on = !g_on;
    LOG("postfx: %s (Scroll Lock)", g_on ? "on" : "off");
}

}  // namespace

// ============================================================================ API
DWORD postfx_create_flags(DWORD flags) {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    load_config();
    if (g_cfg.enabled && (flags & D3DCREATE_PUREDEVICE)) {
        LOG("postfx: creating the device without D3DCREATE_PUREDEVICE (state save/restore needs Get*)");
        flags &= ~(DWORD)D3DCREATE_PUREDEVICE;
    }
    return flags;
}

void postfx_present(IDirect3DDevice9 *dev) {
    if (!dev) return;
    std::lock_guard<std::recursive_mutex> l(g_mu);
    load_config();
    poll_toggle();
    if (!g_on || g_broken || !(g_cfg.smaa || g_cfg.cas || g_cfg.vibrance)) return;
    if (dev != g_dev) {
        release_all();  // resources of a previous device (they kept it alive)
        g_dev = dev;
    }
    if (dev->TestCooperativeLevel() != D3D_OK) return;  // lost / needs Reset

    DevInfo di;
    if (!di.init(dev)) return;
    if (di.cp.BehaviorFlags & D3DCREATE_PUREDEVICE) {
        if (!g_logged_pure)
            LOG("postfx: pure device (state can't be saved); unavailable until the game restarts "
                "with [postfx] enabled=1");
        g_logged_pure = true;
        return;
    }
    if (!compile_all() || !create_static(dev, di)) {
        g_broken = true;
        release_all();
        return;
    }
    IDirect3DSurface9 *bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
    D3DSURFACE_DESC desc{};
    bb->GetDesc(&desc);
    if (desc.Width < 16 || desc.Height < 16 || !create_targets(dev, di, desc)) {
        bb->Release();
        return;
    }
    IDirect3DSurface9 *c0 = nullptr;
    if (FAILED(g_color[0]->GetSurfaceLevel(0, &c0)) || !c0) {
        bb->Release();
        return;
    }
    HRESULT hr = dev->StretchRect(bb, nullptr, c0, nullptr, D3DTEXF_NONE);
    c0->Release();
    if (SUCCEEDED(hr)) {
        D3DCAPS9 caps{};
        dev->GetDeviceCaps(&caps);
        int nrt = (int)caps.NumSimultaneousRTs;
        nrt = nrt < 1 ? 1 : nrt > k_max_rts ? k_max_rts : nrt;
        Saved s;
        save_state(dev, s, nrt);
        run_passes(dev, bb, desc.Width, desc.Height);
        restore_state(dev, s);
    } else if (!g_logged_caps) {
        LOG("postfx: StretchRect from the back buffer failed (%08lx); skipping", (unsigned long)hr);
        g_logged_caps = true;
    }
    bb->Release();
}

void postfx_present_swapchain(IDirect3DSwapChain9 *sc) {
    if (!sc) return;
    IDirect3DDevice9 *dev = nullptr;
    if (FAILED(sc->GetDevice(&dev)) || !dev) return;
    IDirect3DSwapChain9 *sc0 = nullptr;
    if (SUCCEEDED(dev->GetSwapChain(0, &sc0)) && sc0) {
        if (sc0 == sc) postfx_present(dev);
        sc0->Release();
    }
    dev->Release();
}

void postfx_device_lost(void) {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    release_targets();
    g_rt_fail_w = g_rt_fail_h = 0;
}

void postfx_device_release(IDirect3DDevice9 *dev) {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    if (!dev || dev != g_dev) return;
    ULONG held = g_static_refs + g_rt_refs;
    // After the guest's Release the count is n - 1; if that is all ours, let go first.
    ULONG n = dev_refs(dev);
    if (n <= held + 1) release_all();
}
#endif  // _WIN32
