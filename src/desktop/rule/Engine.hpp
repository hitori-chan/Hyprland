#pragma once

#include "Rule.hpp"
#include "../../helpers/signal/Signal.hpp"

namespace Desktop::Rule {
    class CRuleEngine {
      public:
        CRuleEngine()  = default;
        ~CRuleEngine() = default;

        void                          registerRule(SP<IRule>&& rule);
        void                          unregisterRule(const std::string& name);
        void                          unregisterRule(const SP<IRule>& rule);
        void                          updateAllRules();
        void                          cleanExecRules();
        void                          clearAllRules();
        const std::vector<SP<IRule>>& rules();

        // the on_battery match: the power source, followed from the first rule that asks
        bool onBattery();

      private:
        std::vector<SP<IRule>> m_rules;
        CHyprSignalListener    m_powerSourceChanged;
    };

    SP<CRuleEngine> ruleEngine();
}