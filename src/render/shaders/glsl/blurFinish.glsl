#ifndef ALLOW_INCLUDES
#define ALLOW_INCLUDES
#extension GL_ARB_shading_language_include : enable
#endif

#include "defines.h"

#if USE_CM
#include "cm_helpers.glsl"
#endif

// the luminance the highlights roll off toward (0 = off)
uniform float highlights;

float hash(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 1689.1984);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

vec4 blurFinish(vec4 pixColor, vec2 v_texcoord, float noise, float brightness
#if USE_CM
                ,
                int sourceTF, int targetTF, mat3 convertMatrix, vec2 srcTFRange, vec2 dstTFRange
#endif
) {
    float brightnessMultiplier = min(1.0, brightness);

    // noise
    float noiseHash   = hash(v_texcoord);
    float noiseAmount = noiseHash - 0.5;
#if USE_CM
    // Preserve the configured SDR strength while adding noise in source-linear light.
    float sdrNoise    = noiseAmount * noise * brightnessMultiplier;
    float linearNoise = sign(sdrNoise) * toLinearRGB(vec3(abs(sdrNoise)), sourceTF).r * (SDR_MAX_LUMINANCE - SDR_MIN_LUMINANCE) /
        max(srcTFRange.y - srcTFRange.x, 0.001);
#else
    pixColor.rgb += noiseAmount * noise * pixColor.a;
#endif

    // brightness
    pixColor.rgb *= brightnessMultiplier;

    // highlights roll off toward a ceiling while shadows pass (slope 1 at
    // black), by luminance so hue holds: light text over the blur stays
    // readable on any backdrop without greying a dark one
    if (highlights > 0.0) {
        float luma = dot(pixColor.rgb, vec3(0.2126, 0.7152, 0.0722)) / max(pixColor.a, 0.0001);
        if (luma > 0.0)
            pixColor.rgb *= highlights * (1.0 - exp(-luma / highlights)) / luma;
    }

#if USE_CM
    pixColor = doColorManagement(pixColor, 1.0, sourceTF, targetTF, convertMatrix, srcTFRange, dstTFRange, linearNoise);
#endif

    return pixColor;
}
