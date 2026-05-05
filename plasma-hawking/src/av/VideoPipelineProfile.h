#pragma once

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

namespace av {

enum class VideoPipelineProfile {
    SoftwareE2E,
    HardwareE2E,
};

struct VideoPipelineTelemetry {
    VideoPipelineProfile profile{VideoPipelineProfile::SoftwareE2E};
    bool hardwareDecode{false};
    bool hardwareTextureInterop{false};
    bool cpuFrameTransfer{false};
    bool cpuFrameCopy{false};
    bool cpuTextureUpload{false};
    std::string backendName;
};

inline VideoPipelineProfile defaultVideoPipelineProfile() {
#ifdef _WIN32
    return VideoPipelineProfile::HardwareE2E;
#else
    return VideoPipelineProfile::SoftwareE2E;
#endif
}

inline const char* videoPipelineProfileName(VideoPipelineProfile profile) {
    switch (profile) {
    case VideoPipelineProfile::HardwareE2E:
        return "hardware";
    case VideoPipelineProfile::SoftwareE2E:
    default:
        return "software";
    }
}

inline bool isHardwareE2E(VideoPipelineProfile profile) {
    return profile == VideoPipelineProfile::HardwareE2E;
}

inline std::string normalizeVideoPipelineProfileValue(const char* rawValue) {
    if (rawValue == nullptr) {
        return {};
    }
    std::string normalized(rawValue);
    normalized.erase(std::remove_if(normalized.begin(),
                                    normalized.end(),
                                    [](unsigned char ch) { return std::isspace(ch) != 0; }),
                     normalized.end());
    std::transform(normalized.begin(),
                   normalized.end(),
                   normalized.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return normalized;
}

inline VideoPipelineProfile parseVideoPipelineProfile(const char* rawValue,
                                                      VideoPipelineProfile defaultProfile =
                                                          VideoPipelineProfile::SoftwareE2E) {
    const std::string normalized = normalizeVideoPipelineProfileValue(rawValue);
    if (normalized.empty() || normalized == "auto" || normalized == "default") {
        return defaultProfile;
    }
    if (normalized == "hardware" ||
        normalized == "hardwaree2e" ||
        normalized == "hw" ||
        normalized == "gpu") {
        return VideoPipelineProfile::HardwareE2E;
    }
    if (normalized == "software" ||
        normalized == "softwaree2e" ||
        normalized == "sw" ||
        normalized == "cpu") {
        return VideoPipelineProfile::SoftwareE2E;
    }
    return defaultProfile;
}

inline VideoPipelineProfile videoPipelineProfileFromEnvironment() {
    return parseVideoPipelineProfile(std::getenv("MEETING_VIDEO_PIPELINE_PROFILE"),
                                     defaultVideoPipelineProfile());
}

inline bool videoPipelineProfileExplicitlySet() {
    return !normalizeVideoPipelineProfileValue(std::getenv("MEETING_VIDEO_PIPELINE_PROFILE")).empty();
}

}  // namespace av
