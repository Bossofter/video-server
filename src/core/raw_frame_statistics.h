#pragma once

#include "video_server/raw_frame_statistics.h"
#include "video_server/video_frame_view.h"

namespace video_server
{

    bool calculate_raw_frame_statistics(const VideoFrameView &frame, RawFrameStatistics &statistics);

} // namespace video_server
