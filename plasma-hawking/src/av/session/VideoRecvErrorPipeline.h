#pragma once

#include "VideoRecvPipeline.h"

namespace av::session {

class VideoRecvErrorPipeline {
public:
    bool shouldReportSocketError(bool running, bool transientSocketError) const;

    bool extractDecisionError(const VideoRecvHandlingDecision& decision,
                              std::string& outErrorMessage) const;
};

}  // namespace av::session
