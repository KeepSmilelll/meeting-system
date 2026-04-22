#include "VideoRecvErrorPipeline.h"

namespace av::session {

bool VideoRecvErrorPipeline::shouldReportSocketError(bool running, bool transientSocketError) const {
    return running && !transientSocketError;
}

bool VideoRecvErrorPipeline::extractDecisionError(const VideoRecvHandlingDecision& decision,
                                                  std::string& outErrorMessage) const {
    outErrorMessage.clear();
    if (decision.action != VideoRecvHandlingAction::RequestKeyFrameAndError) {
        return false;
    }
    outErrorMessage = decision.errorMessage.empty() ? "video decode failed" : decision.errorMessage;
    return true;
}

}  // namespace av::session
