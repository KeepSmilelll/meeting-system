#include <cassert>
#include <cstdint>
#include <limits>

#include "av/sync/AVSync.h"

int main() {
    using av::sync::AVSync;

    AVSync clock;
    clock.reset(48000, 48000, 960);
    assert(clock.hasAudioPts());
    assert(clock.audioPts() == 48000);
    assert(clock.audioTimeMs() == 1000);

    assert(AVSync::audioPtsToTimeMs(960, 48000) == 20);
    assert(AVSync::videoPts90kToTimeMs(1800) == 20);

    assert(AVSync::videoAudioSkewMs(1800, 960, 48000) == 0);
    assert(AVSync::videoAudioSkewMs(-1, 960, 48000) == std::numeric_limits<int64_t>::min());

    assert(!AVSync::shouldDropVideoFrameByAudioClock(1800, 960, 48000));
    assert(!AVSync::shouldDropVideoFrameByAudioClock(1800, -1, 48000));

    assert(AVSync::shouldDropVideoFrameByAudioClock(270000, 0, 48000, 1200, 1500));
    assert(AVSync::shouldDropVideoFrameByAudioClock(0, 192000, 48000, 1200, 1500));
    assert(!AVSync::shouldDropVideoFrameByAudioClock(225000, 0, 48000, 3000, 3000));

    assert(AVSync::suggestVideoRenderDelayMsByAudioClock(108900, 48000, 48000, 250) == 210);
    assert(AVSync::suggestVideoRenderDelayMsByAudioClock(135000, 48000, 48000, 250) == 250);
    assert(AVSync::suggestVideoRenderDelayMsByAudioClock(90000, 48000, 48000, 250) == 0);
    assert(AVSync::suggestVideoRenderDelayMsByAudioClock(-1, 48000, 48000, 250) == 0);

    return 0;
}
