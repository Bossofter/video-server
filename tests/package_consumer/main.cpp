#include "video_server/video_server.h"

int main()
{
    const auto mode = video_server::ExecutionMode::ManualStep;
    return mode == video_server::ExecutionMode::ManualStep ? 0 : 1;
}
