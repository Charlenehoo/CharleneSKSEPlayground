#include "Plugin.h"
#include "Probe.h"

#include <SKSE/SKSE.h>

SKSEPluginInfo(.Version = Plugin::kVersion, .Name = Plugin::kName, .Author = Plugin::kAuthor,
               .SupportEmail = Plugin::kAuthorEmail);

namespace {

void OnMessage(SKSE::MessagingInterface::Message* a_msg) {
    if (a_msg == nullptr) {
        return;
    }

    switch (a_msg->type) {
        case SKSE::MessagingInterface::kPostPostLoad: {
            break;
        }
        default:
            break;
    }
}

}  // namespace

SKSE_PLUGIN_LOAD(const SKSE::LoadInterface* a_skse) {
    SKSE::Init(a_skse, {
                           .log = true,
                           .logLevel = spdlog::level::debug,
                           .trampoline = true,
                           .trampolineSize = 1024,
                       });

    spdlog::set_pattern("[%T.%e] [%=5t] [%L] [%s] %v");

    const auto* messaging = SKSE::GetMessagingInterface();
    if (messaging == nullptr) {
        return false;
    }
    if (!messaging->RegisterListener(OnMessage)) {
        return false;
    }

    try {
        Probe::Install();
    } catch (const std::exception& e) {
        SKSE::log::error("[PROBE] Install threw: {}", e.what());
    } catch (...) {
        SKSE::log::error("[PROBE] Install threw (unknown)");
    }

    return true;
}
