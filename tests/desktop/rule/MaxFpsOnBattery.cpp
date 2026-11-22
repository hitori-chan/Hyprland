#include <desktop/rule/windowRule/WindowRule.hpp>
#include <desktop/rule/windowRule/WindowRuleEffectContainer.hpp>

#include <gtest/gtest.h>

#include <variant>

using namespace Desktop::Rule;

TEST(MaxFpsOnBattery, namesResolve) {
    EXPECT_EQ(matchPropFromString("on_battery"), RULE_PROP_ON_BATTERY);
    EXPECT_EQ(windowEffects()->get("max_fps"), WINDOW_RULE_EFFECT_MAX_FPS);
    EXPECT_EQ(windowEffects()->get(WINDOW_RULE_EFFECT_MAX_FPS), "max_fps");
}

TEST(MaxFpsOnBattery, maxFpsTakesANonNegativeCount) {
    CWindowRule rule;

    ASSERT_TRUE(rule.addEffect(WINDOW_RULE_EFFECT_MAX_FPS, "60").has_value());
    ASSERT_EQ(rule.effects().size(), 1U);
    ASSERT_TRUE(std::holds_alternative<int64_t>(rule.effects()[0].value));
    EXPECT_EQ(std::get<int64_t>(rule.effects()[0].value), 60);

    EXPECT_TRUE(rule.addEffect(WINDOW_RULE_EFFECT_MAX_FPS, "0").has_value());
    EXPECT_FALSE(rule.addEffect(WINDOW_RULE_EFFECT_MAX_FPS, "-1").has_value());
    EXPECT_FALSE(rule.addEffect(WINDOW_RULE_EFFECT_MAX_FPS, "sixty").has_value());
}

TEST(MaxFpsOnBattery, onBatteryIsABoolMatch) {
    CWindowRule rule;
    rule.registerMatch(RULE_PROP_ON_BATTERY, "true");

    EXPECT_TRUE(rule.getPropertiesMask() & RULE_PROP_ON_BATTERY);
    EXPECT_TRUE(rule.matches(RULE_PROP_ON_BATTERY, true));
    EXPECT_FALSE(rule.matches(RULE_PROP_ON_BATTERY, false));
}
