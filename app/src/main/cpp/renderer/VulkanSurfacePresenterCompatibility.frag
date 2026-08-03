#version 450

layout(set = 0, binding = 0) uniform sampler2D uTexture;
layout(set = 0, binding = 1, rgba8) uniform readonly image2D u3dImage;
layout(set = 0, binding = 4, rgba8) uniform readonly image2D u3dPreviousTopImage;
layout(set = 0, binding = 6, rgba8) uniform readonly image2D u3dPreviousBottomImage;

layout(set = 0, binding = 2, std430) readonly buffer TopPackedBuffer
{
    uint topPacked[];
};

layout(set = 0, binding = 3, std430) readonly buffer BottomPackedBuffer
{
    uint bottomPacked[];
};

layout(set = 0, binding = 5, std430) readonly buffer Capture3dBuffer
{
    uint capture3dPacked[];
};

layout(push_constant) uniform PresenterPushConstants
{
    uint drawMode;
    uint scale;
    uint rendererWidth;
    uint rendererHeight;
    uint packedStride;
    uint screenSwap;
    uint filtering;
    uint previousTopSourceValid;
    uint previousBottomSourceValid;
    uint captureSourceValid;
    uint captureSourceScreenSwapValid;
    uint captureSourceScreenSwap;
    uint liveSourceScreenSwap;
    uint class4VramStructuredPair;
    uint class4NoAboveVramStructuredPair;
    uint class4PreservePackedVramValid;
    uint class4PreservePackedVramScreenSwap;
    uint topStructuredHandoffNoCurrent3d;
    uint bottomStructuredHandoffNoCurrent3d;
    uint topStructuredHandoffSuppress3d;
    uint bottomStructuredHandoffSuppress3d;
    float viewportWidth;
    float viewportHeight;
} pushConstants;

const uint kMetaFlagRegularCaptureUses3d = 1u << 21u;
const uint kMetaFlagVramCaptureUses3d = 1u << 22u;
const uint kMetaFlagForceLive3dCompMode7 = 1u << 18u;
const uint kMetaFlagStructuredAboveDominant = 1u << 19u;
const uint kMetaFlagCompMode2StructuredPair = 1u << 20u;
const uint kFilterLinear = 1u;
const uint kFilterXbr2 = 2u;
const uint kFilterHq2x = 3u;
const uint kFilterHq4x = 4u;
const uint kFilterQuilez = 5u;
const uint kFilterLcd = 6u;
const uint kFilterScanlines = 7u;

layout(location = 0) in vec2 fragUv;
layout(location = 1) in float fragAlpha;

layout(location = 0) out vec4 outColor;

struct Rgba6
{
    int r;
    int g;
    int b;
    int a;
};

bool isPacked3dPlaceholder(Rgba6 color)
{
    return color.r == 0
        && color.g == 0
        && color.b == 0
        && color.a == 0x20;
}

bool isPacked3dLayerSlot(Rgba6 color)
{
    return color.a == 0x40;
}

bool hasPackedVisibleColor(Rgba6 color)
{
    return !isPacked3dPlaceholder(color)
        && ((color.r | color.g | color.b) != 0);
}

bool isRegularCaptureBlankPixel(Rgba6 color)
{
    return isPacked3dPlaceholder(color)
        || (!isPacked3dLayerSlot(color)
            && color.a != 0
            && ((color.r | color.g | color.b) == 0));
}

bool hasStructured2D3DSlot(Rgba6 control)
{
    return (control.a & 0x40) != 0;
}

bool hasStructured2DAbovePlane(Rgba6 control)
{
    return (control.a & 0x80) != 0;
}

bool hasStructured2DProtectedBlack(Rgba6 control)
{
    return (control.a & 0x20) != 0;
}

bool hasStructured2DNo3DCoverage(Rgba6 control)
{
    return (control.a & 0x10) != 0;
}

bool isStructured2DOnly(Rgba6 control)
{
    return !hasStructured2D3DSlot(control)
        && hasStructured2DAbovePlane(control);
}

bool isStructured2DVisible(Rgba6 color)
{
    return color.a != 0
        && !isPacked3dPlaceholder(color)
        && !isPacked3dLayerSlot(color);
}

bool rgbClose6(Rgba6 a, Rgba6 b, int tolerance)
{
    return abs(a.r - b.r) <= tolerance
        && abs(a.g - b.g) <= tolerance
        && abs(a.b - b.b) <= tolerance;
}

Rgba6 makeScreenWhite()
{
    Rgba6 color;
    color.r = 63;
    color.g = 63;
    color.b = 63;
    color.a = 0;
    return color;
}

int clampColor6(int value)
{
    return clamp(value, 0, 63);
}

int toColor8(int value6)
{
    int base = clampColor6(value6) << 2;
    return base | (base >> 6);
}

Rgba6 unpackColor6(uint packedColor)
{
    Rgba6 color;
    color.r = int(packedColor & 0xFFu);
    color.g = int((packedColor >> 8u) & 0xFFu);
    color.b = int((packedColor >> 16u) & 0xFFu);
    color.a = int((packedColor >> 24u) & 0xFFu);
    return color;
}

void applyBrightnessUp(inout Rgba6 color, int evy)
{
    color.r = clampColor6(color.r + (((63 - color.r) * evy) >> 4));
    color.g = clampColor6(color.g + (((63 - color.g) * evy) >> 4));
    color.b = clampColor6(color.b + (((63 - color.b) * evy) >> 4));
}

void applyBrightnessDown(inout Rgba6 color, int evy, int roundingBias)
{
    color.r = clampColor6(color.r - (((color.r * evy) + roundingBias) >> 4));
    color.g = clampColor6(color.g - (((color.g * evy) + roundingBias) >> 4));
    color.b = clampColor6(color.b - (((color.b * evy) + roundingBias) >> 4));
}

Rgba6 sample3DColorAtScaledCoord(float scaledX, float scaledY)
{
    Rgba6 zero;
    zero.r = 0;
    zero.g = 0;
    zero.b = 0;
    zero.a = 0;

    if (scaledX < 0.0
        || scaledX >= float(pushConstants.rendererWidth)
        || scaledY < 0.0
        || scaledY >= float(pushConstants.rendererHeight))
        return zero;

    vec4 color3d = imageLoad(u3dImage, ivec2(int(scaledX), int(scaledY)));

    Rgba6 color;
    color.r = int(clamp(color3d.r * 255.0 + 0.5, 0.0, 255.0)) >> 2;
    color.g = int(clamp(color3d.g * 255.0 + 0.5, 0.0, 255.0)) >> 2;
    color.b = int(clamp(color3d.b * 255.0 + 0.5, 0.0, 255.0)) >> 2;
    color.a = int(clamp(color3d.a * 255.0 + 0.5, 0.0, 255.0)) >> 3;
    return color;
}

bool hasVisibleLive3DSupportAtScaledCoord(float scaledX, float scaledY, float step)
{
    for (int dy = -1; dy <= 1; dy++)
    {
        for (int dx = -1; dx <= 1; dx++)
        {
            if (dx == 0 && dy == 0)
                continue;
            Rgba6 neighbor = sample3DColorAtScaledCoord(
                scaledX + (float(dx) * step),
                scaledY + (float(dy) * step));
            if ((neighbor.a & 0x1F) > 0
                && ((neighbor.r | neighbor.g | neighbor.b) != 0))
                return true;
        }
    }
    return false;
}

Rgba6 samplePreviousTop3DColorAtScaledCoord(float scaledX, float scaledY)
{
    Rgba6 zero;
    zero.r = 0;
    zero.g = 0;
    zero.b = 0;
    zero.a = 0;

    if (scaledX < 0.0
        || scaledX >= float(pushConstants.rendererWidth)
        || scaledY < 0.0
        || scaledY >= float(pushConstants.rendererHeight))
        return zero;

    vec4 color3d = imageLoad(u3dPreviousTopImage, ivec2(int(scaledX), int(scaledY)));

    Rgba6 color;
    color.r = int(clamp(color3d.r * 255.0 + 0.5, 0.0, 255.0)) >> 2;
    color.g = int(clamp(color3d.g * 255.0 + 0.5, 0.0, 255.0)) >> 2;
    color.b = int(clamp(color3d.b * 255.0 + 0.5, 0.0, 255.0)) >> 2;
    color.a = int(clamp(color3d.a * 255.0 + 0.5, 0.0, 255.0)) >> 3;
    return color;
}

Rgba6 samplePreviousBottom3DColorAtScaledCoord(float scaledX, float scaledY)
{
    Rgba6 zero;
    zero.r = 0;
    zero.g = 0;
    zero.b = 0;
    zero.a = 0;

    if (scaledX < 0.0
        || scaledX >= float(pushConstants.rendererWidth)
        || scaledY < 0.0
        || scaledY >= float(pushConstants.rendererHeight))
        return zero;

    vec4 color3d = imageLoad(u3dPreviousBottomImage, ivec2(int(scaledX), int(scaledY)));

    Rgba6 color;
    color.r = int(clamp(color3d.r * 255.0 + 0.5, 0.0, 255.0)) >> 2;
    color.g = int(clamp(color3d.g * 255.0 + 0.5, 0.0, 255.0)) >> 2;
    color.b = int(clamp(color3d.b * 255.0 + 0.5, 0.0, 255.0)) >> 2;
    color.a = int(clamp(color3d.a * 255.0 + 0.5, 0.0, 255.0)) >> 3;
    return color;
}

Rgba6 sampleCapture3DColorAtDsPixel(int dsX, int dsY)
{
    Rgba6 zero;
    zero.r = 0;
    zero.g = 0;
    zero.b = 0;
    zero.a = 0;

    if (pushConstants.captureSourceValid == 0u
        || dsX < 0
        || dsX >= 256
        || dsY < 0
        || dsY >= 192)
        return zero;

    return unpackColor6(capture3dPacked[uint(dsY) * 256u + uint(dsX)]);
}

uint readTopPacked(int y, int x)
{
    uint offset = uint(y) * pushConstants.packedStride + uint(x);
    return topPacked[offset];
}

uint readBottomPacked(int y, int x)
{
    uint offset = uint(y) * pushConstants.packedStride + uint(x);
    return bottomPacked[offset];
}

vec3 color6ToRgb01(Rgba6 color)
{
    return vec3(
        float(toColor8(color.r)) * (1.0 / 255.0),
        float(toColor8(color.g)) * (1.0 / 255.0),
        float(toColor8(color.b)) * (1.0 / 255.0)
    );
}

vec3 color6ToVec3(Rgba6 color)
{
    return vec3(float(color.r), float(color.g), float(color.b));
}

Rgba6 vec3ToColor6(vec3 color, int alpha)
{
    Rgba6 result;
    result.r = clampColor6(int(floor(color.r + 0.5)));
    result.g = clampColor6(int(floor(color.g + 0.5)));
    result.b = clampColor6(int(floor(color.b + 0.5)));
    result.a = alpha;
    return result;
}

bool packedTapIsUnsafeFor2DFilter(Rgba6 color)
{
    return isPacked3dPlaceholder(color) || isPacked3dLayerSlot(color);
}

#define DEFINE_SAMPLE_PACKED_WITH_BRIGHTNESS(FUNC_NAME, READ_PACKED_FUNC) \
Rgba6 FUNC_NAME( \
    int sourceX, \
    int sourceY, \
    int displayMode, \
    int brightnessMode, \
    int brightnessFactor) \
{ \
    Rgba6 pixel = unpackColor6(READ_PACKED_FUNC(sourceY, sourceX)); \
 \
    if (displayMode != 0) \
    { \
        if (brightnessMode == 1) \
            applyBrightnessUp(pixel, brightnessFactor); \
        else if (brightnessMode == 2) \
            applyBrightnessDown(pixel, brightnessFactor, 0xF); \
    } \
 \
    return pixel; \
}

DEFINE_SAMPLE_PACKED_WITH_BRIGHTNESS(sampleTopPackedWithBrightness, readTopPacked)
DEFINE_SAMPLE_PACKED_WITH_BRIGHTNESS(sampleBottomPackedWithBrightness, readBottomPacked)

#define DEFINE_SAMPLE_FILTERED_PACKED_LAYER(FUNC_NAME, READ_PACKED_FUNC) \
Rgba6 FUNC_NAME(int sourceX, int sourceY, float sourceXFloat, float sourceYFloat, int layerOffset) \
{ \
    Rgba6 nearest = unpackColor6(READ_PACKED_FUNC(sourceY, layerOffset + sourceX)); \
    if (pushConstants.filtering != kFilterLinear) \
        return nearest; \
    float sharpX = clamp(sourceXFloat - 0.5, 0.0, 255.0); \
    float sharpY = clamp(sourceYFloat - 0.5, 0.0, 191.0); \
    int x0 = int(floor(sharpX)); \
    int y0 = int(floor(sharpY)); \
    int x1 = min(x0 + 1, 255); \
    int y1 = min(y0 + 1, 191); \
    float tx = sharpX - float(x0); \
    float ty = sharpY - float(y0); \
    Rgba6 c00 = unpackColor6(READ_PACKED_FUNC(y0, layerOffset + x0)); \
    Rgba6 c10 = unpackColor6(READ_PACKED_FUNC(y0, layerOffset + x1)); \
    Rgba6 c01 = unpackColor6(READ_PACKED_FUNC(y1, layerOffset + x0)); \
    Rgba6 c11 = unpackColor6(READ_PACKED_FUNC(y1, layerOffset + x1)); \
    if (packedTapIsUnsafeFor2DFilter(c00) \
        || packedTapIsUnsafeFor2DFilter(c10) \
        || packedTapIsUnsafeFor2DFilter(c01) \
        || packedTapIsUnsafeFor2DFilter(c11)) \
        return nearest; \
    vec3 v00 = color6ToVec3(c00); \
    vec3 v10 = color6ToVec3(c10); \
    vec3 v01 = color6ToVec3(c01); \
    vec3 v11 = color6ToVec3(c11); \
    vec3 blended = mix(mix(v00, v10, tx), mix(v01, v11, tx), ty); \
    return vec3ToColor6(blended, nearest.a); \
}

DEFINE_SAMPLE_FILTERED_PACKED_LAYER(sampleTopFilteredPackedLayer, readTopPacked)
DEFINE_SAMPLE_FILTERED_PACKED_LAYER(sampleBottomFilteredPackedLayer, readBottomPacked)

#undef DEFINE_SAMPLE_FILTERED_PACKED_LAYER

#define DEFINE_COMPOSE_SCREEN_COLOR(FUNC_NAME, READ_PACKED_FUNC, SAMPLE_PACKED_FUNC, SCREEN_IS_TOP) \
vec4 FUNC_NAME() \
{ \
    float sourceXFloat = clamp(fragUv.x * 256.0, 0.0, 255.0); \
    float sourceYFloat = clamp((1.0 - fragUv.y) * 192.0, 0.0, 191.0); \
    float scaledXFloat = clamp(fragUv.x * float(pushConstants.rendererWidth), 0.0, float(pushConstants.rendererWidth - 1u)); \
    float scaledYFloat = clamp((1.0 - fragUv.y) * float(pushConstants.rendererHeight), 0.0, float(pushConstants.rendererHeight - 1u)); \
    int sourceX = int(sourceXFloat); \
    int sourceY = int(sourceYFloat); \
    const bool packedTopScreen = SCREEN_IS_TOP; \
 \
    uint masterBrightness = READ_PACKED_FUNC(sourceY, 256 * 3); \
    int displayMode = int((masterBrightness >> 16u) & 0x3u); \
    int brightnessMode = int(((masterBrightness >> 8u) & 0xFFu) >> 6u); \
    int brightnessFactor = min(16, int(masterBrightness & 0x1Fu)); \
    int xOffset = int((masterBrightness >> 24u) & 0xFFu) \
        - ((((masterBrightness >> 16u) & 0x80u) != 0u) ? 256 : 0); \
    bool regularCaptureUses3d = (masterBrightness & kMetaFlagRegularCaptureUses3d) != 0u; \
    bool vramCaptureUses3d = (masterBrightness & kMetaFlagVramCaptureUses3d) != 0u; \
    bool forceLive3dCompMode7 = (masterBrightness & kMetaFlagForceLive3dCompMode7) != 0u; \
    bool structuredAboveDominant = (masterBrightness & kMetaFlagStructuredAboveDominant) != 0u; \
    bool compMode2StructuredPair = (masterBrightness & kMetaFlagCompMode2StructuredPair) != 0u; \
        bool screenOwnsLive3D = SCREEN_IS_TOP ? (pushConstants.liveSourceScreenSwap != 0u) : (pushConstants.liveSourceScreenSwap == 0u); \
        bool structuredHandoffNoCurrent3D = SCREEN_IS_TOP ? (pushConstants.topStructuredHandoffNoCurrent3d != 0u) : (pushConstants.bottomStructuredHandoffNoCurrent3d != 0u); \
        bool oppositeStructuredHandoffNoCurrent3D = SCREEN_IS_TOP ? (pushConstants.bottomStructuredHandoffNoCurrent3d != 0u) : (pushConstants.topStructuredHandoffNoCurrent3d != 0u); \
        bool structuredHandoffSuppress3D = SCREEN_IS_TOP ? (pushConstants.topStructuredHandoffSuppress3d != 0u) : (pushConstants.bottomStructuredHandoffSuppress3d != 0u); \
        bool screenMatchesCapture3DSource = pushConstants.captureSourceScreenSwapValid == 0u \
            || (SCREEN_IS_TOP ? (pushConstants.captureSourceScreenSwap != 0u) : (pushConstants.captureSourceScreenSwap == 0u)); \
\
    Rgba6 pixel = SCREEN_IS_TOP \
        ? sampleTopFilteredPackedLayer(sourceX, sourceY, sourceXFloat, sourceYFloat, 0) \
        : sampleBottomFilteredPackedLayer(sourceX, sourceY, sourceXFloat, sourceYFloat, 0); \
    Rgba6 nearestPixel = unpackColor6(READ_PACKED_FUNC(sourceY, sourceX)); \
\
    if (displayMode == 1) \
    { \
	        Rgba6 val1 = pixel; \
	        Rgba6 val2 = SCREEN_IS_TOP \
                ? sampleTopFilteredPackedLayer(sourceX, sourceY, sourceXFloat, sourceYFloat, 256) \
                : sampleBottomFilteredPackedLayer(sourceX, sourceY, sourceXFloat, sourceYFloat, 256); \
	        Rgba6 val3 = unpackColor6(READ_PACKED_FUNC(sourceY, 512 + sourceX)); \
\
        int compMode = val3.a & 0xF; \
        bool structured2DSlot = hasStructured2D3DSlot(val3); \
        bool structured2DAbove = hasStructured2DAbovePlane(val3); \
        bool structured2DProtectedBlack = hasStructured2DProtectedBlack(val3); \
        bool structured2DNo3DCoverage = hasStructured2DNo3DCoverage(val3); \
        bool structured2DOnly = isStructured2DOnly(val3); \
        bool both3dPlaceholders = isPacked3dPlaceholder(val1) && isPacked3dPlaceholder(val2); \
        bool captureBackedComp4 = compMode == 4 && both3dPlaceholders; \
        bool packedPlaneHas3DLayerSlot = isPacked3dLayerSlot(val1) || isPacked3dLayerSlot(val2); \
        bool screenHasPrevious3D = SCREEN_IS_TOP ? (pushConstants.previousTopSourceValid != 0u) : (pushConstants.previousBottomSourceValid != 0u); \
        Rgba6 comp4ProbeTopVal1 = unpackColor6(READ_PACKED_FUNC(8, 128)); \
        Rgba6 comp4ProbeTopVal2 = unpackColor6(READ_PACKED_FUNC(8, 256 + 128)); \
        Rgba6 comp4ProbeTopControl = unpackColor6(READ_PACKED_FUNC(8, 512 + 128)); \
        uint topProbeMeta = READ_PACKED_FUNC(8, 256 * 3); \
        Rgba6 comp4ProbeMiddleVal1 = unpackColor6(READ_PACKED_FUNC(96, 128)); \
        Rgba6 comp4ProbeMiddleVal2 = unpackColor6(READ_PACKED_FUNC(96, 256 + 128)); \
        Rgba6 comp4ProbeMiddleControl = unpackColor6(READ_PACKED_FUNC(96, 512 + 128)); \
        uint middleProbeMeta = READ_PACKED_FUNC(96, 256 * 3); \
        Rgba6 comp4ProbeBottomVal1 = unpackColor6(READ_PACKED_FUNC(184, 128)); \
        Rgba6 comp4ProbeBottomVal2 = unpackColor6(READ_PACKED_FUNC(184, 256 + 128)); \
        Rgba6 comp4ProbeBottomControl = unpackColor6(READ_PACKED_FUNC(184, 512 + 128)); \
        uint bottomProbeMeta = READ_PACKED_FUNC(184, 256 * 3); \
        bool topProbeCaptureBackedComp4 = ((comp4ProbeTopControl.a & 0xF) == 4) \
            && isPacked3dPlaceholder(comp4ProbeTopVal1) \
            && isPacked3dPlaceholder(comp4ProbeTopVal2); \
        bool middleProbeCaptureBackedComp4 = ((comp4ProbeMiddleControl.a & 0xF) == 4) \
            && isPacked3dPlaceholder(comp4ProbeMiddleVal1) \
            && isPacked3dPlaceholder(comp4ProbeMiddleVal2); \
        bool bottomProbeCaptureBackedComp4 = ((comp4ProbeBottomControl.a & 0xF) == 4) \
            && isPacked3dPlaceholder(comp4ProbeBottomVal1) \
            && isPacked3dPlaceholder(comp4ProbeBottomVal2); \
        bool screenWideCaptureBackedComp4 = topProbeCaptureBackedComp4 \
            && middleProbeCaptureBackedComp4 \
            && bottomProbeCaptureBackedComp4; \
        bool partialCaptureBackedComp4Screen = (topProbeCaptureBackedComp4 \
                || middleProbeCaptureBackedComp4 \
                || bottomProbeCaptureBackedComp4) \
            && !screenWideCaptureBackedComp4; \
        bool topProbeRegularCaptureUses3d = (topProbeMeta & kMetaFlagRegularCaptureUses3d) != 0u; \
        bool middleProbeRegularCaptureUses3d = (middleProbeMeta & kMetaFlagRegularCaptureUses3d) != 0u; \
        bool bottomProbeRegularCaptureUses3d = (bottomProbeMeta & kMetaFlagRegularCaptureUses3d) != 0u; \
        bool screenHasPartialRegularCapture3d = (topProbeRegularCaptureUses3d \
                || middleProbeRegularCaptureUses3d \
                || bottomProbeRegularCaptureUses3d) \
            && !(topProbeRegularCaptureUses3d \
                && middleProbeRegularCaptureUses3d \
                && bottomProbeRegularCaptureUses3d); \
        bool partialCaptureBackedComp4BorderLine = partialCaptureBackedComp4Screen \
            && screenHasPartialRegularCapture3d \
            && ((SCREEN_IS_TOP && sourceY <= 20) \
                || (!SCREEN_IS_TOP && sourceY >= 171)); \
        bool partialRegularCaptureOppositeFringeLine = screenHasPartialRegularCapture3d \
            && !regularCaptureUses3d \
            && ((SCREEN_IS_TOP && sourceY >= 171) \
                || (!SCREEN_IS_TOP && sourceY <= 20)); \
        bool screenWideRegularCaptureBlank = regularCaptureUses3d \
            && isRegularCaptureBlankPixel(comp4ProbeTopVal1) \
            && isRegularCaptureBlankPixel(comp4ProbeMiddleVal1) \
            && isRegularCaptureBlankPixel(comp4ProbeBottomVal1); \
        bool temporalCompMode7Uses3D = compMode == 7 \
            && (regularCaptureUses3d || forceLive3dCompMode7 || partialRegularCaptureOppositeFringeLine); \
        bool compModeSamples3D = !structured2DOnly \
            && (compMode <= 4 || temporalCompMode7Uses3D || structured2DSlot); \
        Rgba6 pixel3D; \
        pixel3D.r = 0; \
        pixel3D.g = 0; \
        pixel3D.b = 0; \
        pixel3D.a = 0; \
        bool pixel3DFromLive = false; \
        if (compModeSamples3D && screenOwnsLive3D) \
        { \
            pixel3D = sample3DColorAtScaledCoord( \
                scaledXFloat + (float(xOffset) * float(max(pushConstants.scale, 1u))), \
                scaledYFloat \
            ); \
            pixel3DFromLive = true; \
        } \
        else if (compModeSamples3D && screenHasPrevious3D) \
        { \
            pixel3D = SCREEN_IS_TOP \
                ? samplePreviousTop3DColorAtScaledCoord( \
                    scaledXFloat + (float(xOffset) * float(max(pushConstants.scale, 1u))), \
                    scaledYFloat \
                ) \
                : samplePreviousBottom3DColorAtScaledCoord( \
                    scaledXFloat + (float(xOffset) * float(max(pushConstants.scale, 1u))), \
                    scaledYFloat \
                ); \
        } \
        if (structured2DNo3DCoverage) \
        { \
            pixel3D.r = 0; \
            pixel3D.g = 0; \
            pixel3D.b = 0; \
            pixel3D.a = 0; \
        } \
        if (regularCaptureUses3d \
            && structuredAboveDominant \
            && compMode == 2 \
            && structured2DSlot \
            && screenHasPrevious3D) \
        { \
            pixel3D = SCREEN_IS_TOP \
                ? samplePreviousTop3DColorAtScaledCoord( \
                    scaledXFloat + (float(xOffset) * float(max(pushConstants.scale, 1u))), \
                    scaledYFloat \
                ) \
                : samplePreviousBottom3DColorAtScaledCoord( \
                    scaledXFloat + (float(xOffset) * float(max(pushConstants.scale, 1u))), \
                    scaledYFloat \
                ); \
            pixel3DFromLive = false; \
        } \
        if (compModeSamples3D \
            && screenOwnsLive3D \
            && screenHasPrevious3D \
            && brightnessMode == 0 \
            && !structured2DProtectedBlack \
            && !partialCaptureBackedComp4BorderLine \
            && !regularCaptureUses3d \
            && (structured2DSlot || regularCaptureUses3d || vramCaptureUses3d) \
            && (pixel3D.a & 0x1F) > 0 \
            && ((pixel3D.r | pixel3D.g | pixel3D.b) == 0)) \
        { \
            Rgba6 history3D = SCREEN_IS_TOP \
                ? samplePreviousTop3DColorAtScaledCoord( \
                    scaledXFloat + (float(xOffset) * float(max(pushConstants.scale, 1u))), \
                    scaledYFloat \
                ) \
                : samplePreviousBottom3DColorAtScaledCoord( \
                    scaledXFloat + (float(xOffset) * float(max(pushConstants.scale, 1u))), \
                    scaledYFloat \
                ); \
            if ((history3D.a & 0x1F) > 0 && ((history3D.r | history3D.g | history3D.b) != 0)) \
            { \
                pixel3D = history3D; \
                pixel3DFromLive = false; \
            } \
        } \
        Rgba6 capture3D = sampleCapture3DColorAtDsPixel(sourceX, sourceY); \
        bool pixel3DLiveBlackHasSupport = pixel3DFromLive \
            && regularCaptureUses3d \
            && (pixel3D.a & 0x1F) > 0 \
            && ((pixel3D.r | pixel3D.g | pixel3D.b) == 0) \
            && hasVisibleLive3DSupportAtScaledCoord( \
                scaledXFloat + (float(xOffset) * float(max(pushConstants.scale, 1u))), \
                scaledYFloat, \
                float(max(pushConstants.scale, 1u))); \
        bool pixel3DHasVisibleColor = (pixel3D.a & 0x1F) > 0 \
            && (((pixel3D.r | pixel3D.g | pixel3D.b) != 0) \
                || pixel3DLiveBlackHasSupport \
                || (!pixel3DFromLive && screenHasPrevious3D)); \
            bool pixel3DHasUsefulColor = (pixel3D.a & 0x1F) > 0 \
                && (pixel3DHasVisibleColor \
                    || regularCaptureUses3d); \
            bool structuredHandoffFringeLine = SCREEN_IS_TOP \
                ? (sourceY >= 160) \
                : (sourceY <= 31); \
            if (structuredHandoffSuppress3D \
                && structuredHandoffFringeLine \
                && structured2DSlot \
                && compMode == 7 \
                && !regularCaptureUses3d \
                && !vramCaptureUses3d \
                && !forceLive3dCompMode7) \
            { \
                pixel3D.r = 0; \
                pixel3D.g = 0; \
                pixel3D.b = 0; \
                pixel3D.a = 0; \
                pixel3DFromLive = false; \
                pixel3DLiveBlackHasSupport = false; \
                pixel3DHasVisibleColor = false; \
                pixel3DHasUsefulColor = false; \
            } \
            bool capture3DHasCoverage = (capture3D.a & 0x1F) > 0; \
        bool captureBackedComp4Valid = captureBackedComp4 \
            && capture3DHasCoverage \
            && screenMatchesCapture3DSource \
            && (screenOwnsLive3D || screenHasPrevious3D || forceLive3dCompMode7); \
        bool capture3DHasVisibleColor = capture3DHasCoverage \
            && ((capture3D.r | capture3D.g | capture3D.b) != 0); \
        bool class4StructuredCaptureOnly = (pushConstants.class4NoAboveVramStructuredPair != 0u) \
            && structured2DSlot \
            && !structured2DAbove \
            && !screenOwnsLive3D \
            && screenMatchesCapture3DSource \
            && capture3DHasVisibleColor; \
        if (class4StructuredCaptureOnly) \
        { \
            pixel3D = capture3D; \
            pixel3DFromLive = false; \
            pixel3DLiveBlackHasSupport = false; \
            pixel3DHasVisibleColor = true; \
            pixel3DHasUsefulColor = true; \
        } \
        bool allowCaptureHighresFromLive = screenOwnsLive3D \
            || forceLive3dCompMode7; \
        Rgba6 captureHighresFromLive = allowCaptureHighresFromLive \
            ? sample3DColorAtScaledCoord( \
                scaledXFloat + (float(xOffset) * float(max(pushConstants.scale, 1u))), \
                scaledYFloat \
            ) \
            : Rgba6(0, 0, 0, 0); \
        bool captureHighresHasCoverage = (captureHighresFromLive.a & 0x1F) > 0; \
        bool captureHighresHasVisibleColor = captureHighresHasCoverage \
            && ((captureHighresFromLive.r | captureHighresFromLive.g | captureHighresFromLive.b) != 0); \
        bool suppressCaptureBackedComp4Border = captureBackedComp4 \
            && ((partialCaptureBackedComp4BorderLine && !capture3DHasVisibleColor) \
                || (!screenWideCaptureBackedComp4 && !capture3DHasVisibleColor)); \
        bool liveBlackClearsCaptureBackedComp4 = captureBackedComp4 \
            && screenWideCaptureBackedComp4 \
            && screenOwnsLive3D \
            && !regularCaptureUses3d \
            && !vramCaptureUses3d \
            && !structured2DProtectedBlack \
            && !structured2DNo3DCoverage \
            && !suppressCaptureBackedComp4Border \
            && ((pixel3D.a & 0x1F) > 0) \
            && !pixel3DHasVisibleColor; \
        if (liveBlackClearsCaptureBackedComp4) \
            pixel3DHasUsefulColor = true; \
        if (compMode <= 4 \
            && packedPlaneHas3DLayerSlot \
            && !structured2DSlot \
            && !regularCaptureUses3d \
            && !vramCaptureUses3d \
            && !forceLive3dCompMode7 \
            && !captureBackedComp4) \
        { \
            pixel3D.r = 0; \
            pixel3D.g = 0; \
            pixel3D.b = 0; \
            pixel3D.a = 0; \
            pixel3DHasVisibleColor = false; \
            pixel3DHasUsefulColor = false; \
        } \
 \
        if (compMode == 4 && both3dPlaceholders && !suppressCaptureBackedComp4Border && pixel3DHasUsefulColor) \
        { \
            val1 = pixel3D; \
        } \
        else if (compMode == 4 && both3dPlaceholders && !suppressCaptureBackedComp4Border) \
        { \
            if (captureHighresHasVisibleColor) \
            { \
                pixel3D = captureHighresFromLive; \
                pixel3DFromLive = allowCaptureHighresFromLive; \
                pixel3DLiveBlackHasSupport = false; \
            } \
            else if (captureBackedComp4Valid) \
            { \
                pixel3D = capture3D; \
                pixel3DFromLive = false; \
                pixel3DLiveBlackHasSupport = false; \
            } \
        } \
        else if ((pixel3D.a & 0x1F) == 0 && captureBackedComp4Valid) \
        { \
            if (captureHighresHasVisibleColor) \
            { \
                pixel3D = captureHighresFromLive; \
                pixel3DFromLive = allowCaptureHighresFromLive; \
                pixel3DLiveBlackHasSupport = false; \
            } \
            else \
            { \
                pixel3D = capture3D; \
                pixel3DFromLive = false; \
                pixel3DLiveBlackHasSupport = false; \
            } \
        } \
 \
        if (structured2DOnly) \
        { \
            val1 = pixel; \
        } \
        else if (structured2DSlot) \
        { \
            Rgba6 below2D = val1; \
            Rgba6 above2D = val2; \
            Rgba6 composed = below2D; \
            bool structuredFullRegularCapture3d = regularCaptureUses3d \
                && !screenHasPartialRegularCapture3d; \
            if (compMode == 7 \
                && structuredFullRegularCapture3d \
                && !structured2DAbove \
                && !structured2DProtectedBlack \
                && (screenOwnsLive3D || forceLive3dCompMode7) \
                && !screenHasPrevious3D \
                && screenMatchesCapture3DSource \
                && capture3DHasVisibleColor \
                && !pixel3DHasVisibleColor) \
            { \
                pixel3D = capture3D; \
                pixel3DFromLive = false; \
                pixel3DLiveBlackHasSupport = false; \
                pixel3DHasVisibleColor = true; \
                pixel3DHasUsefulColor = true; \
            } \
            if (compMode == 7 \
                && structuredFullRegularCapture3d \
                && !structured2DAbove \
                && !structured2DProtectedBlack \
                && screenOwnsLive3D \
                && screenHasPrevious3D \
                && screenMatchesCapture3DSource \
                && (pixel3D.a & 0x1F) > 0 \
                && !pixel3DHasVisibleColor) \
            { \
                Rgba6 history3D = SCREEN_IS_TOP \
                    ? samplePreviousTop3DColorAtScaledCoord( \
                        scaledXFloat + (float(xOffset) * float(max(pushConstants.scale, 1u))), \
                        scaledYFloat) \
                    : samplePreviousBottom3DColorAtScaledCoord( \
                        scaledXFloat + (float(xOffset) * float(max(pushConstants.scale, 1u))), \
                        scaledYFloat); \
                if ((history3D.a & 0x1F) > 0 && ((history3D.r | history3D.g | history3D.b) != 0)) \
                { \
                    pixel3D = history3D; \
                    pixel3DFromLive = false; \
                    pixel3DLiveBlackHasSupport = false; \
                    pixel3DHasVisibleColor = true; \
                    pixel3DHasUsefulColor = true; \
                } \
            } \
            bool structuredFullRegularVisible3D = compMode == 7 \
                && structuredFullRegularCapture3d \
                && !structured2DProtectedBlack \
                && pixel3DHasVisibleColor; \
            bool regularCaptureVisible2DAbove = regularCaptureUses3d \
                && structured2DAbove \
                && !structuredAboveDominant \
                && isStructured2DVisible(above2D); \
            bool structuredPlane1Usable2D = isStructured2DVisible(above2D) \
                || structured2DProtectedBlack; \
            bool regularCaptureDominant2DPlane = regularCaptureUses3d \
                && structuredAboveDominant \
                && structuredPlane1Usable2D; \
            bool plainStructuredOppositeNoCurrentHandoff = compMode == 7 \
                && oppositeStructuredHandoffNoCurrent3D \
                && !regularCaptureUses3d \
                && !vramCaptureUses3d \
                && !forceLive3dCompMode7 \
                && !screenOwnsLive3D; \
            bool structured3DOverDominant2D = compMode == 7 \
                && structuredFullRegularCapture3d \
                && structuredAboveDominant \
                && (screenOwnsLive3D || screenHasPrevious3D) \
                && !plainStructuredOppositeNoCurrentHandoff \
                && !structured2DProtectedBlack \
                && pixel3DHasVisibleColor; \
            bool preserveStructuredPair3DFromBrightness = compMode == 2 \
                && compMode2StructuredPair \
                && !structured2DAbove \
                && !regularCaptureUses3d \
                && !vramCaptureUses3d; \
            bool structuredPackedOnlyHandoff = compMode == 7 \
                && !regularCaptureUses3d \
                && !vramCaptureUses3d \
                && !forceLive3dCompMode7 \
                && !screenOwnsLive3D \
                && (!screenHasPrevious3D || plainStructuredOppositeNoCurrentHandoff); \
            if (!structuredPackedOnlyHandoff \
                && (pixel3D.a & 0x1F) > 0 \
                && (!regularCaptureUses3d || pixel3DHasVisibleColor)) \
            { \
                composed = pixel3D; \
                if (compMode == 4) \
                { \
                    int eva = (pixel3D.a & 0x1F) + 1; \
                    int evb = 32 - eva; \
                    composed.r = clampColor6(((pixel3D.r * eva) + (below2D.r * evb) + 0x10) >> 5); \
                    composed.g = clampColor6(((pixel3D.g * eva) + (below2D.g * evb) + 0x10) >> 5); \
                    composed.b = clampColor6(((pixel3D.b * eva) + (below2D.b * evb) + 0x10) >> 5); \
                } \
                else if (compMode == 1 && structured2DAbove) \
                { \
                    int eva = val3.g; \
                    int evb = val3.b; \
                    composed.r = clampColor6(((above2D.r * eva) + (pixel3D.r * evb) + 0x8) >> 4); \
                    composed.g = clampColor6(((above2D.g * eva) + (pixel3D.g * evb) + 0x8) >> 4); \
                    composed.b = clampColor6(((above2D.b * eva) + (pixel3D.b * evb) + 0x8) >> 4); \
                } \
                else if (compMode == 2) \
                { \
                    if (!preserveStructuredPair3DFromBrightness) \
                    { \
                        int evy = val3.g; \
                        composed.r = clampColor6(composed.r + ((((63 - composed.r) * evy) + 0x8) >> 4)); \
                        composed.g = clampColor6(composed.g + ((((63 - composed.g) * evy) + 0x8) >> 4)); \
                        composed.b = clampColor6(composed.b + ((((63 - composed.b) * evy) + 0x8) >> 4)); \
                    } \
                } \
                else if (compMode == 3) \
                { \
                    applyBrightnessDown(composed, val3.g, 0x7); \
                } \
                if (compMode == 7 \
                    && structuredFullRegularCapture3d \
                    && !structured2DAbove \
                    && !structured2DProtectedBlack \
                    && !structured3DOverDominant2D \
                    && !structuredFullRegularVisible3D \
                    && screenMatchesCapture3DSource \
                    && isStructured2DVisible(below2D) \
                    && ((below2D.r | below2D.g | below2D.b) != 0)) \
                { \
                    composed = below2D; \
                } \
                if (compMode != 1 && structured2DAbove && !structured3DOverDominant2D) \
                    composed = above2D; \
            } \
            else if (structuredHandoffNoCurrent3D) \
            { \
                bool aboveVisibleNonBlack = structured2DAbove \
                    && isStructured2DVisible(above2D) \
                    && ((above2D.r | above2D.g | above2D.b) != 0); \
                bool belowVisible = isStructured2DVisible(below2D); \
                if (aboveVisibleNonBlack) \
                    composed = above2D; \
                else if (belowVisible) \
                    composed = below2D; \
            } \
            else if (structured2DAbove && structuredPlane1Usable2D) \
            { \
                composed = above2D; \
            } \
            if (regularCaptureVisible2DAbove) \
                composed = above2D; \
            if (regularCaptureDominant2DPlane) \
                composed = above2D; \
            if (compMode == 2 \
                && (compMode2StructuredPair || (SCREEN_IS_TOP && regularCaptureUses3d && structuredAboveDominant)) \
                && structuredPlane1Usable2D) \
                composed = above2D; \
            val1 = composed; \
        } \
        else if (compMode == 4) \
        { \
            if (suppressCaptureBackedComp4Border) \
            { \
                val1 = val2; \
            } \
            else if (both3dPlaceholders && pixel3DHasUsefulColor) \
            { \
                val1 = pixel3D; \
            } \
            else if (both3dPlaceholders && captureHighresHasVisibleColor) \
            { \
                val1 = captureHighresFromLive; \
            } \
            else if (both3dPlaceholders && captureBackedComp4Valid) \
            { \
                val1 = capture3D; \
            } \
            else if (both3dPlaceholders && brightnessFactor > 0) \
            { \
                val1 = val2; \
            } \
            else if ((pixel3D.a & 0x1F) > 0) \
            { \
                int eva = (pixel3D.a & 0x1F) + 1; \
                int evb = 32 - eva; \
                val1.r = clampColor6(((pixel3D.r * eva) + (val1.r * evb) + 0x10) >> 5); \
                val1.g = clampColor6(((pixel3D.g * eva) + (val1.g * evb) + 0x10) >> 5); \
                val1.b = clampColor6(((pixel3D.b * eva) + (val1.b * evb) + 0x10) >> 5); \
            } \
            else \
            { \
                val1 = val2; \
            } \
        } \
        else if (compMode == 1) \
        { \
            if ((pixel3D.a & 0x1F) > 0) \
            { \
                int eva = val3.g; \
                int evb = val3.b; \
                val1.r = clampColor6(((val1.r * eva) + (pixel3D.r * evb) + 0x8) >> 4); \
                val1.g = clampColor6(((val1.g * eva) + (pixel3D.g * evb) + 0x8) >> 4); \
                val1.b = clampColor6(((val1.b * eva) + (pixel3D.b * evb) + 0x8) >> 4); \
            } \
            else \
            { \
                val1 = val2; \
            } \
        } \
        else if (compMode <= 3) \
        { \
            if ((pixel3D.a & 0x1F) > 0) \
            { \
                val1 = pixel3D; \
                int evy = val3.g; \
                if (compMode == 2) \
                { \
                    val1.r = clampColor6(val1.r + ((((63 - val1.r) * evy) + 0x8) >> 4)); \
                    val1.g = clampColor6(val1.g + ((((63 - val1.g) * evy) + 0x8) >> 4)); \
                    val1.b = clampColor6(val1.b + ((((63 - val1.b) * evy) + 0x8) >> 4)); \
                } \
                else if (compMode == 3) \
                { \
                    applyBrightnessDown(val1, evy, 0x7); \
                } \
            } \
            else \
            { \
                val1 = val2; \
            } \
            if (compMode == 2 \
                && (compMode2StructuredPair || (SCREEN_IS_TOP && regularCaptureUses3d && structuredAboveDominant)) \
                && (isStructured2DVisible(val2) || structured2DProtectedBlack)) \
                val1 = val2; \
        } \
        else if (compMode == 7) \
        { \
            bool plane1Is3dLayer = isPacked3dLayerSlot(val2); \
            bool plane2HadOverlayMarker = (val3.a & 0x80) != 0; \
            bool plane2HasBlendControl = ((val3.r | val3.g | val3.b) != 0); \
            bool overlayOver3d = plane1Is3dLayer || plane2HadOverlayMarker || plane2HasBlendControl; \
            bool regularCaptureBackdropPixel = regularCaptureUses3d && isPacked3dPlaceholder(nearestPixel); \
            bool regularCaptureVisibleBlackPixel = regularCaptureUses3d \
                && !regularCaptureBackdropPixel \
                && !isPacked3dLayerSlot(nearestPixel) \
                && nearestPixel.a != 0 \
                && ((nearestPixel.r | nearestPixel.g | nearestPixel.b) == 0); \
            bool regularCaptureVisibleBlackHasLocalSupport = false; \
            if (regularCaptureVisibleBlackPixel) \
            { \
                for (int supportDy = -1; supportDy <= 1; supportDy++) \
                { \
                    int supportY = sourceY + supportDy; \
                    if (supportY < 0 || supportY >= 192) \
                        continue; \
                    for (int supportDx = -1; supportDx <= 1; supportDx++) \
                    { \
                        if (supportDx == 0 && supportDy == 0) \
                            continue; \
                        int supportX = sourceX + supportDx; \
                        if (supportX < 0 || supportX >= 256) \
                            continue; \
                        Rgba6 support0 = unpackColor6(READ_PACKED_FUNC(supportY, supportX)); \
                        Rgba6 support1 = unpackColor6(READ_PACKED_FUNC(supportY, 256 + supportX)); \
                        if (hasPackedVisibleColor(support0) || hasPackedVisibleColor(support1)) \
                            regularCaptureVisibleBlackHasLocalSupport = true; \
                    } \
                } \
            } \
            bool regularCaptureProtectedBlackAbove = regularCaptureUses3d \
                && structured2DSlot \
                && structured2DAbove \
                && structured2DProtectedBlack; \
            bool fullScreenRegularCapture3d = regularCaptureUses3d \
                && !screenHasPartialRegularCapture3d; \
            bool regularCapture3DMatchesThisScreen = !fullScreenRegularCapture3d \
                || ((screenOwnsLive3D || forceLive3dCompMode7) && screenMatchesCapture3DSource); \
            bool regularCaptureWideBlackPixel = regularCaptureVisibleBlackPixel \
                && fullScreenRegularCapture3d \
                && !regularCaptureVisibleBlackHasLocalSupport \
                && !regularCaptureProtectedBlackAbove; \
            bool regularCaptureBlackResolvedByCapture = regularCaptureVisibleBlackPixel \
                && fullScreenRegularCapture3d \
                && regularCapture3DMatchesThisScreen \
                && structured2DSlot \
                && capture3DHasVisibleColor \
                && !regularCaptureProtectedBlackAbove; \
            if (regularCaptureBlackResolvedByCapture) \
                val1 = regularCaptureVisibleBlackHasLocalSupport || !pixel3DHasVisibleColor ? capture3D : pixel3D; \
            else if (regularCaptureVisibleBlackPixel && !regularCaptureWideBlackPixel) \
                val1 = nearestPixel; \
            bool regularCaptureProtectedBlackBand = regularCaptureUses3d \
                && screenHasPartialRegularCapture3d \
                && ((SCREEN_IS_TOP && sourceY <= 20) \
                    || (!SCREEN_IS_TOP && sourceY >= 171)); \
            bool pixel3DHasCoverage = (pixel3D.a & 0x1F) > 0; \
            bool pixel3DHasVisibleColor = (pixel3D.a & 0x1F) > 0 \
                && (((pixel3D.r | pixel3D.g | pixel3D.b) != 0) \
                    || pixel3DLiveBlackHasSupport \
                    || (!pixel3DFromLive && screenHasPrevious3D)); \
            if (regularCaptureBackdropPixel \
                && screenOwnsLive3D \
                && !pixel3DHasCoverage \
                && !capture3DHasVisibleColor \
                && screenHasPrevious3D) \
            { \
                pixel3D = SCREEN_IS_TOP \
                    ? samplePreviousTop3DColorAtScaledCoord( \
                        scaledXFloat + (float(xOffset) * float(max(pushConstants.scale, 1u))), \
                        scaledYFloat \
                    ) \
                    : samplePreviousBottom3DColorAtScaledCoord( \
                        scaledXFloat + (float(xOffset) * float(max(pushConstants.scale, 1u))), \
                        scaledYFloat \
                    ); \
                pixel3DHasCoverage = (pixel3D.a & 0x1F) > 0; \
                pixel3DFromLive = false; \
                pixel3DHasVisibleColor = (pixel3D.a & 0x1F) > 0 \
                    && (((pixel3D.r | pixel3D.g | pixel3D.b) != 0) \
                        || screenHasPrevious3D); \
            } \
            bool regularCaptureExplicit3dPixel = regularCaptureUses3d \
                && (regularCaptureBackdropPixel || isPacked3dLayerSlot(nearestPixel)); \
            bool regularCaptureProtectedBlackPixel = regularCaptureVisibleBlackPixel \
                && screenHasPartialRegularCapture3d \
                && !regularCaptureExplicit3dPixel; \
            bool regularCaptureBlank3dPixel = regularCaptureUses3d \
                && !overlayOver3d \
                && (regularCaptureBackdropPixel || regularCaptureProtectedBlackPixel || regularCaptureWideBlackPixel); \
            bool compMode7LineHas3D = forceLive3dCompMode7 \
                || (capture3DHasVisibleColor && regularCapture3DMatchesThisScreen) \
                || (regularCaptureUses3d \
                    && (fullScreenRegularCapture3d ? pixel3DHasVisibleColor : pixel3DHasCoverage) \
                    && !regularCaptureProtectedBlackBand \
                    && !screenWideRegularCaptureBlank); \
            compMode7LineHas3D = compMode7LineHas3D \
                && !partialCaptureBackedComp4BorderLine; \
            bool nonLiveFullRegularHistoryPixel = temporalCompMode7Uses3D \
                && fullScreenRegularCapture3d \
                && !screenOwnsLive3D \
                && screenHasPrevious3D \
                && pixel3DHasVisibleColor \
                && (regularCaptureBackdropPixel \
                    || regularCaptureVisibleBlackPixel \
                    || regularCaptureProtectedBlackAbove); \
            if (regularCaptureBlackResolvedByCapture) \
                val1 = regularCaptureVisibleBlackHasLocalSupport || !pixel3DHasVisibleColor ? capture3D : pixel3D; \
            else if (nonLiveFullRegularHistoryPixel) \
                val1 = pixel3D; \
            else if (temporalCompMode7Uses3D && capture3DHasVisibleColor && regularCapture3DMatchesThisScreen && regularCaptureBackdropPixel && !overlayOver3d) \
                val1 = capture3D; \
            else if (temporalCompMode7Uses3D && partialRegularCaptureOppositeFringeLine && pixel3DHasCoverage) \
                val1 = pixel3D; \
            else if (temporalCompMode7Uses3D && compMode7LineHas3D && regularCaptureWideBlackPixel && pixel3DHasVisibleColor) \
                val1 = pixel3D; \
            else if (temporalCompMode7Uses3D && compMode7LineHas3D && regularCaptureBlank3dPixel && screenWideRegularCaptureBlank && pixel3DHasVisibleColor) \
                val1 = pixel3D; \
            else if (temporalCompMode7Uses3D && compMode7LineHas3D && pixel3DHasCoverage && !overlayOver3d && regularCaptureExplicit3dPixel && !regularCaptureProtectedBlackPixel) \
                val1 = pixel3D; \
        } \
        pixel = val1; \
    } \
    else if (displayMode == 2 && vramCaptureUses3d) \
    { \
        Rgba6 val2 = SCREEN_IS_TOP \
            ? sampleTopFilteredPackedLayer(sourceX, sourceY, sourceXFloat, sourceYFloat, 256) \
            : sampleBottomFilteredPackedLayer(sourceX, sourceY, sourceXFloat, sourceYFloat, 256); \
        Rgba6 val3 = unpackColor6(READ_PACKED_FUNC(sourceY, 512 + sourceX)); \
        int compMode = val3.a & 0xF; \
        bool structured2DSlot = hasStructured2D3DSlot(val3); \
        bool structured2DAbove = hasStructured2DAbovePlane(val3); \
        bool structured2DProtectedBlack = hasStructured2DProtectedBlack(val3); \
        bool structured2DOnly = isStructured2DOnly(val3); \
        bool screenHasPrevious3D = SCREEN_IS_TOP ? (pushConstants.previousTopSourceValid != 0u) : (pushConstants.previousBottomSourceValid != 0u); \
        bool class4VramStructuredPair = pushConstants.class4VramStructuredPair != 0u; \
        bool class4PreservePackedVram = (pushConstants.class4PreservePackedVramValid != 0u) \
            && (SCREEN_IS_TOP ? (pushConstants.class4PreservePackedVramScreenSwap != 0u) : (pushConstants.class4PreservePackedVramScreenSwap == 0u)); \
        bool class4NoAbovePreservePackedVram = class4PreservePackedVram \
            && (pushConstants.class4NoAboveVramStructuredPair != 0u); \
        Rgba6 vram3D; \
        vram3D.r = 0; \
        vram3D.g = 0; \
        vram3D.b = 0; \
        vram3D.a = 0; \
        bool vram3DFromLive = false; \
        if (screenOwnsLive3D && !class4PreservePackedVram) \
        { \
            vram3D = sample3DColorAtScaledCoord( \
                scaledXFloat + (float(xOffset) * float(max(pushConstants.scale, 1u))), \
                scaledYFloat \
            ); \
            vram3DFromLive = true; \
        } \
        else if (screenHasPrevious3D && !class4NoAbovePreservePackedVram) \
        { \
            vram3D = SCREEN_IS_TOP \
                ? samplePreviousTop3DColorAtScaledCoord( \
                    scaledXFloat + (float(xOffset) * float(max(pushConstants.scale, 1u))), \
                    scaledYFloat \
                ) \
                : samplePreviousBottom3DColorAtScaledCoord( \
                    scaledXFloat + (float(xOffset) * float(max(pushConstants.scale, 1u))), \
                    scaledYFloat \
                ); \
        } \
        if (structured2DOnly) \
        { \
        } \
        else if (structured2DSlot) \
        { \
            Rgba6 composed = pixel; \
            if ((vram3D.a & 0x1F) > 0) \
            { \
                composed = vram3D; \
                if (compMode == 1 && structured2DAbove) \
                { \
                    int eva = val3.g; \
                    int evb = val3.b; \
                    composed.r = clampColor6(((val2.r * eva) + (vram3D.r * evb) + 0x8) >> 4); \
                    composed.g = clampColor6(((val2.g * eva) + (vram3D.g * evb) + 0x8) >> 4); \
                    composed.b = clampColor6(((val2.b * eva) + (vram3D.b * evb) + 0x8) >> 4); \
                } \
                else if (compMode == 4) \
                { \
                    int eva = (vram3D.a & 0x1F) + 1; \
                    int evb = 32 - eva; \
                    composed.r = clampColor6(((vram3D.r * eva) + (pixel.r * evb) + 0x10) >> 5); \
                    composed.g = clampColor6(((vram3D.g * eva) + (pixel.g * evb) + 0x10) >> 5); \
                    composed.b = clampColor6(((vram3D.b * eva) + (pixel.b * evb) + 0x10) >> 5); \
                } \
                else if (compMode == 2) \
                { \
                    int evy = val3.g; \
                    composed.r = clampColor6(composed.r + ((((63 - composed.r) * evy) + 0x8) >> 4)); \
                    composed.g = clampColor6(composed.g + ((((63 - composed.g) * evy) + 0x8) >> 4)); \
                    composed.b = clampColor6(composed.b + ((((63 - composed.b) * evy) + 0x8) >> 4)); \
                } \
                else if (compMode == 3) \
                { \
                    applyBrightnessDown(composed, val3.g, 0x7); \
                } \
                else if (compMode != 1 && structured2DAbove) \
                { \
                    composed = val2; \
                } \
                if (structured2DProtectedBlack && structured2DAbove) \
                    composed = val2; \
            } \
            else if (structured2DAbove) \
            { \
                composed = val2; \
            } \
            pixel = composed; \
        } \
        else \
        { \
            bool packedMatchesHighres3D = (vram3D.a & 0x1F) > 0 \
            && rgbClose6(pixel, vram3D, 2); \
            bool packedHasNonBlackColor = ((pixel.r | pixel.g | pixel.b) != 0); \
            bool packedIsPure3DSlot = isPacked3dLayerSlot(pixel) && !packedHasNonBlackColor; \
            bool packedCarries2DOverlay = packedHasNonBlackColor \
            && !isPacked3dPlaceholder(pixel) \
            && !packedIsPure3DSlot \
            && !packedMatchesHighres3D \
            && !class4VramStructuredPair; \
            bool class4PreserveBlocksLive3D = class4PreservePackedVram && vram3DFromLive; \
            if ((vram3D.a & 0x1F) > 0 && !packedCarries2DOverlay && !class4PreserveBlocksLive3D && !class4NoAbovePreservePackedVram) \
                pixel = vram3D; \
        } \
    } \
 \
    if (displayMode != 0 && !compMode2StructuredPair) \
    { \
        if (brightnessMode == 1) \
            applyBrightnessUp(pixel, brightnessFactor); \
        else if (brightnessMode == 2) \
            applyBrightnessDown(pixel, brightnessFactor, 0xF); \
    } \
 \
    if (displayMode == 1) \
        return vec4(color6ToRgb01(pixel), fragAlpha); \
 \
	    if (pushConstants.filtering == kFilterLinear) \
    { \
        float linearX = clamp(sourceXFloat - 0.5, 0.0, 255.0); \
        float linearY = clamp(sourceYFloat - 0.5, 0.0, 191.0); \
        int x0 = int(floor(linearX)); \
        int y0 = int(floor(linearY)); \
        int x1 = min(x0 + 1, 255); \
        int y1 = min(y0 + 1, 191); \
        float tx = linearX - float(x0); \
        float ty = linearY - float(y0); \
 \
        vec3 c00 = color6ToRgb01(SAMPLE_PACKED_FUNC(x0, y0, displayMode, brightnessMode, brightnessFactor)); \
        vec3 c10 = color6ToRgb01(SAMPLE_PACKED_FUNC(x1, y0, displayMode, brightnessMode, brightnessFactor)); \
        vec3 c01 = color6ToRgb01(SAMPLE_PACKED_FUNC(x0, y1, displayMode, brightnessMode, brightnessFactor)); \
        vec3 c11 = color6ToRgb01(SAMPLE_PACKED_FUNC(x1, y1, displayMode, brightnessMode, brightnessFactor)); \
        vec3 cx0 = mix(c00, c10, tx); \
        vec3 cx1 = mix(c01, c11, tx); \
        vec3 finalColor = mix(cx0, cx1, ty); \
        return vec4(finalColor, fragAlpha); \
    } \
 \
    pixel = SAMPLE_PACKED_FUNC(sourceX, sourceY, displayMode, brightnessMode, brightnessFactor); \
    return vec4(color6ToRgb01(pixel), fragAlpha); \
}

DEFINE_COMPOSE_SCREEN_COLOR(composeTopScreenColor, readTopPacked, sampleTopPackedWithBrightness, true)
DEFINE_COMPOSE_SCREEN_COLOR(composeBottomScreenColor, readBottomPacked, sampleBottomPackedWithBrightness, false)

vec2 compositeTexelSize()
{
    uint safeScale = max(pushConstants.scale, 1u);
    return vec2(
        1.0 / float(256u * safeScale),
        1.0 / float((384u + 2u) * safeScale)
    );
}

vec2 compositeDsTexelSize()
{
    return compositeTexelSize() * float(max(pushConstants.scale, 1u));
}

vec4 screenUvBounds(bool topScreen)
{
    float gapUv = 1.0 / 386.0;
    float minY = topScreen ? 0.0 : (0.5 + gapUv);
    float maxY = topScreen ? (0.5 - gapUv) : 1.0;
    return vec4(0.0, minY, 1.0, maxY);
}

vec2 clampCompositeUvToScreen(vec2 uv, bool topScreen)
{
    vec2 texel = compositeTexelSize();
    vec4 bounds = screenUvBounds(topScreen);
    return vec2(
        clamp(uv.x, texel.x * 0.5, 1.0 - texel.x * 0.5),
        clamp(uv.y, bounds.y + (texel.y * 0.5), bounds.w - (texel.y * 0.5))
    );
}

vec3 sampleCompositeRgb(vec2 uv, bool topScreen)
{
    return texture(uTexture, clampCompositeUvToScreen(uv, topScreen)).bgr;
}

vec2 screenLocalCoord(vec2 uv, bool topScreen)
{
    vec4 bounds = screenUvBounds(topScreen);
    vec2 clamped = clampCompositeUvToScreen(uv, topScreen);
    return vec2(clamped.x, clamp((clamped.y - bounds.y) / max(bounds.w - bounds.y, 0.0001), 0.0, 1.0));
}

vec2 uvFromScreenLocal(vec2 local, bool topScreen)
{
    vec4 bounds = screenUvBounds(topScreen);
    return vec2(clamp(local.x, 0.0, 1.0), mix(bounds.y, bounds.w, clamp(local.y, 0.0, 1.0)));
}

vec2 uvFromScreenTexel(vec2 texelCoord, bool topScreen)
{
    return uvFromScreenLocal((texelCoord + vec2(0.5)) / vec2(256.0, 192.0), topScreen);
}

vec3 filterQuilez(vec2 uv, bool topScreen)
{
    vec2 size = vec2(256.0, 192.0);
    vec2 local = screenLocalCoord(uv, topScreen);
    vec2 p = (local * size) + vec2(0.5);
    vec2 i = floor(p);
    vec2 f = p - i;
    f = f * f * f * ((f * ((f * 6.0) - vec2(15.0))) + vec2(10.0));
    vec2 filteredLocal = (i + f - vec2(0.5)) / size;
    return sampleCompositeRgb(uvFromScreenLocal(filteredLocal, topScreen), topScreen);
}

vec3 filterScanlines(vec2 uv, bool topScreen)
{
    vec3 color = sampleCompositeRgb(uv, topScreen);
    vec2 local = screenLocalCoord(uv, topScreen);
    vec2 omega = vec2(3.1415 * 256.0, 2.0 * 3.1415 * 192.0);
    const float baseBrightness = 0.95;
    const vec2 sineComp = vec2(0.05, 0.15);
    return clamp(color * (baseBrightness + dot(sineComp * sin(local * omega), vec2(1.0))), 0.0, 1.0);
}

vec3 filterLcd(vec2 uv, bool topScreen)
{
    vec3 color = sampleCompositeRgb(uv, topScreen);
    vec2 local = screenLocalCoord(uv, topScreen);
    vec2 angle = local * (3.141592654 * 2.0 * vec2(256.0, 192.0));
    const float brightenScanlines = 16.0;
    const float brightenLcd = 4.0;
    const vec3 offsets = 3.141592654 * vec3(0.5, 0.5 - (2.0 / 3.0), 0.5 - (4.0 / 3.0));
    float yFactor = (brightenScanlines + sin(angle.y)) / (brightenScanlines + 1.0);
    vec3 xFactors = (brightenLcd + sin(angle.x + offsets)) / (brightenLcd + 1.0);
    return clamp(color * yFactor * xFactors, 0.0, 1.0);
}

vec3 filterXbr2(vec2 uv, bool topScreen)
{
    vec2 texel = compositeDsTexelSize();
    vec2 local = screenLocalCoord(uv, topScreen);
    vec2 fp = fract(local * vec2(256.0, 192.0));
    vec2 g1 = vec2(0.0, -texel.y) * (step(0.5, fp.x) + step(0.5, fp.y) - 1.0)
        + vec2(-texel.x, 0.0) * (step(0.5, fp.x) - step(0.5, fp.y));
    vec2 g2 = vec2(0.0, -texel.y) * (step(0.5, fp.y) - step(0.5, fp.x))
        + vec2(-texel.x, 0.0) * (step(0.5, fp.x) + step(0.5, fp.y) - 1.0);
    vec3 c = sampleCompositeRgb(uv + g1 - g2, topScreen);
    vec3 e = sampleCompositeRgb(uv, topScreen);
    vec3 f = sampleCompositeRgb(uv - g2, topScreen);
    vec3 h = sampleCompositeRgb(uv - g1, topScreen);
    vec3 i = sampleCompositeRgb(uv - g1 - g2, topScreen);
    float de = length(e - f) + length(e - h);
    float edge = step(0.015, de) * step(length(h - f), 0.015) * step(0.015, length(h - e));
    vec3 blended = mix(e, mix(f, h, 0.5), 0.5);
    return mix(e, blended, edge * step(length(e - c), length(e - i) + 0.0001));
}

vec3 filterHq2x(vec2 uv, bool topScreen)
{
    vec2 dg1 = 0.5 * compositeDsTexelSize();
    vec2 dg2 = vec2(-dg1.x, dg1.y);
    vec2 dx = vec2(dg1.x, 0.0);
    vec2 dy = vec2(0.0, dg1.y);
    vec3 c00 = sampleCompositeRgb(uv - dg1, topScreen);
    vec3 c10 = sampleCompositeRgb(uv - dy, topScreen);
    vec3 c20 = sampleCompositeRgb(uv - dg2, topScreen);
    vec3 c01 = sampleCompositeRgb(uv - dx, topScreen);
    vec3 c11 = sampleCompositeRgb(uv, topScreen);
    vec3 c21 = sampleCompositeRgb(uv + dx, topScreen);
    vec3 c02 = sampleCompositeRgb(uv + dg2, topScreen);
    vec3 c12 = sampleCompositeRgb(uv + dy, topScreen);
    vec3 c22 = sampleCompositeRgb(uv + dg1, topScreen);
    vec3 dt = vec3(1.0);
    const float mx = 0.325;
    const float k = -0.250;
    const float maxW = 0.25;
    const float minW = -0.05;
    const float lumAdd = 0.25;
    float md1 = dot(abs(c00 - c22), dt);
    float md2 = dot(abs(c02 - c20), dt);
    float w1 = dot(abs(c22 - c11), dt) * md2;
    float w2 = dot(abs(c02 - c11), dt) * md1;
    float w3 = dot(abs(c00 - c11), dt) * md2;
    float w4 = dot(abs(c20 - c11), dt) * md1;
    float t1 = w1 + w3;
    float t2 = w2 + w4;
    float ww = max(t1, t2) + 0.001;
    c11 = (w1 * c00 + w2 * c20 + w3 * c22 + w4 * c02 + ww * c11) / (t1 + t2 + ww);
    float lc1 = k / (0.12 * dot(c10 + c12 + c11, dt) + lumAdd);
    float lc2 = k / (0.12 * dot(c01 + c21 + c11, dt) + lumAdd);
    w1 = clamp(lc1 * dot(abs(c11 - c10), dt) + mx, minW, maxW);
    w2 = clamp(lc2 * dot(abs(c11 - c21), dt) + mx, minW, maxW);
    w3 = clamp(lc1 * dot(abs(c11 - c12), dt) + mx, minW, maxW);
    w4 = clamp(lc2 * dot(abs(c11 - c01), dt) + mx, minW, maxW);
    return clamp(w1 * c10 + w2 * c21 + w3 * c12 + w4 * c01 + (1.0 - w1 - w2 - w3 - w4) * c11, 0.0, 1.0);
}

vec3 filterHq4x(vec2 uv, bool topScreen)
{
    vec2 dg1 = 0.5 * compositeDsTexelSize();
    vec2 dg2 = vec2(-dg1.x, dg1.y);
    vec2 sd1 = dg1 * 0.5;
    vec2 sd2 = dg2 * 0.5;
    vec2 ddx = vec2(dg1.x, 0.0);
    vec2 ddy = vec2(0.0, dg1.y);
    vec3 c = sampleCompositeRgb(uv, topScreen);
    vec3 i1 = sampleCompositeRgb(uv - sd1, topScreen);
    vec3 i2 = sampleCompositeRgb(uv - sd2, topScreen);
    vec3 i3 = sampleCompositeRgb(uv + sd1, topScreen);
    vec3 i4 = sampleCompositeRgb(uv + sd2, topScreen);
    vec3 o1 = sampleCompositeRgb(uv - dg1, topScreen);
    vec3 o3 = sampleCompositeRgb(uv + dg1, topScreen);
    vec3 o2 = sampleCompositeRgb(uv - dg2, topScreen);
    vec3 o4 = sampleCompositeRgb(uv + dg2, topScreen);
    vec3 s1 = sampleCompositeRgb(uv - ddy, topScreen);
    vec3 s2 = sampleCompositeRgb(uv + ddx, topScreen);
    vec3 s3 = sampleCompositeRgb(uv + ddy, topScreen);
    vec3 s4 = sampleCompositeRgb(uv - ddx, topScreen);
    vec3 dt = vec3(1.0);
    const float mx = 1.00;
    const float k = -1.10;
    const float maxW = 0.75;
    const float minW = 0.03;
    const float lumAdd = 0.33;
    float ko1 = dot(abs(o1 - c), dt);
    float ko2 = dot(abs(o2 - c), dt);
    float ko3 = dot(abs(o3 - c), dt);
    float ko4 = dot(abs(o4 - c), dt);
    float k1 = min(dot(abs(i1 - i3), dt), max(ko1, ko3));
    float k2 = min(dot(abs(i2 - i4), dt), max(ko2, ko4));
    float w1 = k2;
    if (ko3 < ko1)
        w1 *= ko3 / max(ko1, 0.000001);
    float w2 = k1;
    if (ko4 < ko2)
        w2 *= ko4 / max(ko2, 0.000001);
    float w3 = k2;
    if (ko1 < ko3)
        w3 *= ko1 / max(ko3, 0.000001);
    float w4 = k1;
    if (ko2 < ko4)
        w4 *= ko2 / max(ko4, 0.000001);
    c = (w1 * o1 + w2 * o2 + w3 * o3 + w4 * o4 + 0.001 * c) / (w1 + w2 + w3 + w4 + 0.001);
    w1 = k * dot(abs(i1 - c) + abs(i3 - c), dt) / (0.125 * dot(i1 + i3, dt) + lumAdd);
    w2 = k * dot(abs(i2 - c) + abs(i4 - c), dt) / (0.125 * dot(i2 + i4, dt) + lumAdd);
    w3 = k * dot(abs(s1 - c) + abs(s3 - c), dt) / (0.125 * dot(s1 + s3, dt) + lumAdd);
    w4 = k * dot(abs(s2 - c) + abs(s4 - c), dt) / (0.125 * dot(s2 + s4, dt) + lumAdd);
    w1 = clamp(w1 + mx, minW, maxW);
    w2 = clamp(w2 + mx, minW, maxW);
    w3 = clamp(w3 + mx, minW, maxW);
    w4 = clamp(w4 + mx, minW, maxW);
    return clamp((w1 * (i1 + i3) + w2 * (i2 + i4) + w3 * (s1 + s3) + w4 * (s2 + s4) + c) / (2.0 * (w1 + w2 + w3 + w4) + 1.0), 0.0, 1.0);
}

vec3 applyCompositePostFilter(vec2 uv, bool topScreen)
{
    if (pushConstants.filtering == kFilterQuilez)
        return filterQuilez(uv, topScreen);
    if (pushConstants.filtering == kFilterXbr2)
        return filterXbr2(uv, topScreen);
    if (pushConstants.filtering == kFilterHq2x)
        return filterHq2x(uv, topScreen);
    if (pushConstants.filtering == kFilterHq4x)
        return filterHq4x(uv, topScreen);
    if (pushConstants.filtering == kFilterLcd)
        return filterLcd(uv, topScreen);
    if (pushConstants.filtering == kFilterScanlines)
        return filterScanlines(uv, topScreen);
    return sampleCompositeRgb(uv, topScreen);
}

void main()
{
    if (pushConstants.drawMode == 0u)
    {
        vec4 sampledColor = texture(uTexture, fragUv);
        outColor = vec4(sampledColor.rgb, fragAlpha);
        return;
    }

    if (pushConstants.drawMode == 1u)
    {
        vec4 sampledColor = texture(uTexture, fragUv);
        outColor = vec4(sampledColor.bgr, fragAlpha);
        return;
    }

    if (pushConstants.drawMode == 6u)
    {
        vec4 sampledColor = texture(uTexture, fragUv);
        outColor = vec4(sampledColor.rgb, fragAlpha);
        return;
    }

    if (pushConstants.drawMode == 4u || pushConstants.drawMode == 5u)
    {
        bool topScreen = pushConstants.drawMode == 4u;
        outColor = vec4(applyCompositePostFilter(fragUv, topScreen), fragAlpha);
        return;
    }

    if (pushConstants.drawMode == 2u)
    {
        outColor = composeTopScreenColor();
        return;
    }

    outColor = composeBottomScreenColor();
}
