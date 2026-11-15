#include <helpers/cm/ColorManagement.hpp>
#include <render/types.hpp>

#include <cmath>
#include <gtest/gtest.h>

using namespace NColorManagement;

static RGBAColor convertWithoutTonemapping(RGBAColor color, PImageDescription source, PImageDescription target) {
    const Render::SCMSettings settings = {
        .sourceTF   = source->value().transferFunction,
        .targetTF   = target->value().transferFunction,
        .srcTFRange = {.min = source->value().getTFMinLuminance(), .max = source->value().getTFMaxLuminance()},
        .dstTFRange = {.min = target->value().getTFMinLuminance(), .max = target->value().getTFMaxLuminance()},
    };
    return convertColor(color, source, target, settings);
}

static void expectNeutralColor(const RGBAColor& color, double luminance, double alpha = 1.0) {
    EXPECT_NEAR(color.c.r, luminance, 0.0001);
    EXPECT_NEAR(color.c.g, luminance, 0.0001);
    EXPECT_NEAR(color.c.b, luminance, 0.0001);
    EXPECT_DOUBLE_EQ(color.c.a, alpha);
}

static PImageDescription nativeLinearDescription(const SImageDescription::SPCLuminances& luminances) {
    return CImageDescription::from(SImageDescription{
        .transferFunction = CM_TRANSFER_FUNCTION_EXT_LINEAR,
        .primariesNameSet = true,
        .primariesNamed   = CM_PRIMARIES_SRGB,
        .luminances       = luminances,
    });
}

TEST(ColorManagement, WindowsScRGBDecodesReferenceWhite) {
    const auto color = convertWithoutTonemapping({{.r = 2.5375, .g = 2.5375, .b = 2.5375, .a = 1.0}}, SCRGB_IMAGE_DESCRIPTION, LINEAR_NN_IMAGE_DESCRIPTION);
    expectNeutralColor(color, 203.0);
}

TEST(ColorManagement, WorkBufferPreservesScRGBEncodingWithMonitorMetadata) {
    for (const auto& luminances : {
             SImageDescription::SPCLuminances{.min = 0.2f, .max = 80, .reference = 80},
             SImageDescription::SPCLuminances{.min = 0.005f, .max = 600, .reference = 203},
             SImageDescription::SPCLuminances{.min = 0.0f, .max = 10000, .reference = 203},
         }) {
        const auto workBuffer = LINEAR_IMAGE_DESCRIPTION->with(luminances);
        EXPECT_EQ(workBuffer->value().luminances, luminances);

        const auto encoded = convertWithoutTonemapping({{.r = 203.0, .g = 203.0, .b = 203.0, .a = 1.0}}, LINEAR_NN_IMAGE_DESCRIPTION, workBuffer);
        expectNeutralColor(encoded, 2.5375);

        const auto decoded = convertWithoutTonemapping(encoded, workBuffer, LINEAR_NN_IMAGE_DESCRIPTION);
        expectNeutralColor(decoded, 203.0);
    }
}

TEST(ColorManagement, EquivalentEncodingsDoNotRequireConversion) {
    const auto workBuffer = LINEAR_IMAGE_DESCRIPTION->with({.min = 0.0f, .max = 80, .reference = 203});
    auto       native     = workBuffer->value();
    native.isWindows      = false;
    const auto source     = CImageDescription::from(native);

    ASSERT_NE(source->id(), workBuffer->id());
    EXPECT_FALSE(source->needsCM(workBuffer));
    EXPECT_FALSE(workBuffer->needsCM(source));
    EXPECT_FALSE(workBuffer->needsCM(workBuffer));
}

TEST(ColorManagement, NativeLinearReferenceWhiteUsesDeclaredRange) {
    const auto source     = nativeLinearDescription({.min = 0.0f, .max = 10000, .reference = 203});
    const auto workBuffer = LINEAR_IMAGE_DESCRIPTION->with(source->value().luminances);

    const auto encoded = convertWithoutTonemapping({{.r = 0.0203, .g = 0.0203, .b = 0.0203, .a = 1.0}}, source, workBuffer);
    expectNeutralColor(encoded, 2.5375);

    const auto decoded = convertWithoutTonemapping(encoded, workBuffer, LINEAR_NN_IMAGE_DESCRIPTION);
    expectNeutralColor(decoded, 203.0);
}

TEST(ColorManagement, MatchingMetadataWithDifferentEncodingRequiresConversion) {
    for (const auto& luminances : {
             SImageDescription::SPCLuminances{.min = 0.2f, .max = 80, .reference = 80},
             SImageDescription::SPCLuminances{.min = 0.0f, .max = 10000, .reference = 203},
         }) {
        const auto source     = nativeLinearDescription(luminances);
        const auto workBuffer = LINEAR_IMAGE_DESCRIPTION->with(luminances);

        ASSERT_EQ(source->value().luminances, workBuffer->value().luminances);
        ASSERT_EQ(source->value().transferFunction, workBuffer->value().transferFunction);
        ASSERT_EQ(source->value().getPrimaries(), workBuffer->value().getPrimaries());
        EXPECT_TRUE(source->needsCM(workBuffer));
        EXPECT_TRUE(workBuffer->needsCM(source));
    }
}

TEST(ColorManagement, NativeLinearDecodingIncludesMinimumLuminance) {
    const auto source = nativeLinearDescription({.min = 1.0f, .max = 1001, .reference = 101});
    for (const auto value : {0.0, 0.1, 1.0}) {
        const auto decoded = convertWithoutTonemapping({{.r = value, .g = value, .b = value, .a = 1.0}}, source, LINEAR_NN_IMAGE_DESCRIPTION);
        expectNeutralColor(decoded, 1.0 + value * 1000.0);
    }
}

TEST(ColorManagement, NativeLinearConversionPreservesPremultipliedAlpha) {
    const auto source = nativeLinearDescription({.min = 1.0f, .max = 1001, .reference = 101});
    for (const auto alpha : {0.0, 0.25, 0.5, 1.0}) {
        const double value   = 0.1 * alpha;
        const auto   encoded = convertWithoutTonemapping({{.r = value, .g = value, .b = value, .a = alpha}}, source, LINEAR_IMAGE_DESCRIPTION);
        expectNeutralColor(encoded, 101.0 / 80.0 * alpha, alpha);

        const auto roundTrip = convertWithoutTonemapping(encoded, LINEAR_IMAGE_DESCRIPTION, source);
        expectNeutralColor(roundTrip, value, alpha);
    }
}

TEST(ColorManagement, NativeLinearConversionPreservesExtendedRange) {
    const auto source = nativeLinearDescription({.min = 0.0f, .max = 10000, .reference = 203});
    const auto color  = convertWithoutTonemapping({{.r = -0.0001, .g = 0.0203, .b = 1.25, .a = 1.0}}, source, LINEAR_NN_IMAGE_DESCRIPTION);
    EXPECT_NEAR(color.c.r, -1.0, 0.0001);
    EXPECT_NEAR(color.c.g, 203.0, 0.0001);
    EXPECT_NEAR(color.c.b, 12500.0, 0.0001);
    EXPECT_DOUBLE_EQ(color.c.a, 1.0);
}

TEST(ColorManagement, BT1886DecodesWithTheDisplayPower) {
    // a display EOTF: a 2.4 power over the TF's range, not the inverse of
    // BT.709's camera curve (0.5 decodes to 0.19 of the range, not 0.26)
    const auto source = CImageDescription::from(SImageDescription{
        .transferFunction = CM_TRANSFER_FUNCTION_BT1886,
        .primariesNameSet = true,
        .primariesNamed   = CM_PRIMARIES_SRGB,
    });
    for (const auto value : {0.0, 0.5, 1.0}) {
        const auto decoded = convertWithoutTonemapping({{.r = value, .g = value, .b = value, .a = 1.0}}, source, LINEAR_NN_IMAGE_DESCRIPTION);
        expectNeutralColor(decoded, 0.01 + std::pow(value, 2.4) * (100.0 - 0.01));
        expectNeutralColor(convertWithoutTonemapping(decoded, LINEAR_NN_IMAGE_DESCRIPTION, source), value);
    }
}

// render:hdr_blend_space = srgb: the HDR work buffer as CMonitor::workBufferImageDescription makes it for sdr_min_luminance 0.2 and sdr_max_luminance 203
static PImageDescription extendedSRGBWorkBuffer() {
    return CImageDescription::from(SImageDescription{
        .transferFunction = CM_TRANSFER_FUNCTION_EXT_SRGB,
        .primariesNameSet = true,
        .primariesNamed   = CM_PRIMARIES_SRGB,
        .primaries        = NColorPrimaries::BT709,
        .luminances       = {.min = 0.005f, .max = 1600, .reference = 203},
        .tfRange          = SImageDescription::SPCTFRange{.min = 0.2f, .max = 203},
    });
}

// an SDR surface into the HDR work buffer, with getCMSettings' SDR range
static RGBAColor convertSDRToWorkBuffer(RGBAColor color, PImageDescription target, float sdrBrightness = 1.0f) {
    const Render::SCMSettings settings = {
        .sourceTF                = CM_TRANSFER_FUNCTION_SRGB,
        .targetTF                = target->value().transferFunction,
        .srcTFRange              = {.min = 0.2f, .max = 203},
        .dstTFRange              = {.min = target->value().getTFMinLuminance(0.2f), .max = target->value().getTFMaxLuminance(203)},
        .needsSDRmod             = sdrBrightness != 1.0f,
        .sdrBrightnessMultiplier = sdrBrightness,
    };
    return convertColor(color, DEFAULT_SRGB_IMAGE_DESCRIPTION, target, settings);
}

TEST(ColorManagement, ExtendedSRGBWorkBufferKeepsItsRange) {
    const auto workBuffer = extendedSRGBWorkBuffer();
    // whatever SDR setting the caller passes
    EXPECT_FLOAT_EQ(workBuffer->value().getTFMinLuminance(), 0.2f);
    EXPECT_FLOAT_EQ(workBuffer->value().getTFMaxLuminance(), 203.0f);
    EXPECT_FLOAT_EQ(workBuffer->value().getTFMaxLuminance(80), 203.0f);

    // a client's extended sRGB is not the work buffer
    auto client    = workBuffer->value();
    client.tfRange = std::nullopt;
    EXPECT_NE(CImageDescription::from(client)->id(), workBuffer->id());
    EXPECT_TRUE(CImageDescription::from(client)->needsCM(workBuffer));
}

TEST(ColorManagement, ExtendedSRGBWorkBufferKeepsSDRCodes) {
    const auto workBuffer = extendedSRGBWorkBuffer();
    for (const auto value : {0.0, 0.1, 0.5, 1.0})
        expectNeutralColor(convertSDRToWorkBuffer({{.r = value, .g = value, .b = value, .a = 1.0}}, workBuffer), value);
}

TEST(ColorManagement, ExtendedSRGBWorkBufferBlendsAsSDR) {
    // a white at 10 % over black, blended in the buffer's encoding: SDR mode's code 0.1, not 10 % of SDR white's light
    const auto workBuffer = extendedSRGBWorkBuffer();
    const auto white      = convertSDRToWorkBuffer({{.r = 0.1, .g = 0.1, .b = 0.1, .a = 0.1}}, workBuffer);
    const auto black      = convertSDRToWorkBuffer({{.r = 0.0, .g = 0.0, .b = 0.0, .a = 1.0}}, workBuffer);
    const auto blended =
        RGBAColor{{.r = white.c.r + (1.0 - white.c.a) * black.c.r, .g = white.c.g + (1.0 - white.c.a) * black.c.g, .b = white.c.b + (1.0 - white.c.a) * black.c.b, .a = 1.0}};
    expectNeutralColor(blended, 0.1);

    const double SRGB_EOTF = std::pow((0.1 + 0.055) / 1.055, 2.4);
    expectNeutralColor(convertWithoutTonemapping(blended, workBuffer, LINEAR_NN_IMAGE_DESCRIPTION), 0.2 + SRGB_EOTF * (203.0 - 0.2));
}

TEST(ColorManagement, ExtendedSRGBWorkBufferKeepsHDRAndWideGamut) {
    const auto workBuffer = extendedSRGBWorkBuffer();
    const auto encoded    = convertWithoutTonemapping({{.r = -1.0, .g = 203.0, .b = 1000.0, .a = 1.0}}, LINEAR_NN_IMAGE_DESCRIPTION, workBuffer);
    EXPECT_LT(encoded.c.r, 0.0);
    EXPECT_NEAR(encoded.c.g, 1.0, 0.0001);
    EXPECT_GT(encoded.c.b, 1.0);

    const auto decoded = convertWithoutTonemapping(encoded, workBuffer, LINEAR_NN_IMAGE_DESCRIPTION);
    EXPECT_NEAR(decoded.c.r, -1.0, 0.001);
    EXPECT_NEAR(decoded.c.g, 203.0, 0.001);
    EXPECT_NEAR(decoded.c.b, 1000.0, 0.001);
}

TEST(ColorManagement, ExtendedSRGBWorkBufferTakesSDRBrightnessInLinearLight) {
    // sdrbrightness 2 puts SDR white at 406 cd/m², as the linear work buffer does, not at the light of code 2.0
    const auto workBuffer = extendedSRGBWorkBuffer();
    const auto encoded    = convertSDRToWorkBuffer({{.r = 1.0, .g = 1.0, .b = 1.0, .a = 1.0}}, workBuffer, 2.0f);
    expectNeutralColor(convertWithoutTonemapping(encoded, workBuffer, LINEAR_NN_IMAGE_DESCRIPTION), 406.0);
}
