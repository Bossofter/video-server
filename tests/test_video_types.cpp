#include <string>

#include <gtest/gtest.h>

#include "video_server/video_types.h"
#include "libav_pixel_format_mapping.h"

TEST(VideoTypesTest, ConvertsPixelFormatsToStrings)
{
    using video_server::VideoPixelFormat;

    EXPECT_EQ(std::string(video_server::to_string(VideoPixelFormat::RGB24)), "RGB24");
    EXPECT_EQ(std::string(video_server::to_string(VideoPixelFormat::BGR24)), "BGR24");
    EXPECT_EQ(std::string(video_server::to_string(VideoPixelFormat::RGBA32)), "RGBA32");
    EXPECT_EQ(std::string(video_server::to_string(VideoPixelFormat::BGRA32)), "BGRA32");
    EXPECT_EQ(std::string(video_server::to_string(VideoPixelFormat::GRAY8)), "GRAY8");
    EXPECT_EQ(std::string(video_server::to_string(VideoPixelFormat::GRAY10LE)), "GRAY10LE");
    EXPECT_EQ(std::string(video_server::to_string(VideoPixelFormat::GRAY12LE)), "GRAY12LE");
    EXPECT_EQ(std::string(video_server::to_string(VideoPixelFormat::GRAY16LE)), "GRAY16LE");
    EXPECT_EQ(std::string(video_server::to_string(VideoPixelFormat::NV12)), "NV12");
    EXPECT_EQ(std::string(video_server::to_string(VideoPixelFormat::I420)), "I420");
}

TEST(VideoTypesTest, ParsesPixelFormatsFromStringsAndGrayscaleAliases)
{
    using video_server::VideoPixelFormat;

    ASSERT_TRUE(video_server::video_pixel_format_from_string("rgb24").has_value());
    EXPECT_EQ(*video_server::video_pixel_format_from_string("rgb24"), VideoPixelFormat::RGB24);
    ASSERT_TRUE(video_server::video_pixel_format_from_string("RGBA32").has_value());
    EXPECT_EQ(*video_server::video_pixel_format_from_string("RGBA32"), VideoPixelFormat::RGBA32);
    ASSERT_TRUE(video_server::video_pixel_format_from_string("gray8").has_value());
    EXPECT_EQ(*video_server::video_pixel_format_from_string("gray8"), VideoPixelFormat::GRAY8);
    ASSERT_TRUE(video_server::video_pixel_format_from_string("GRAY10").has_value());
    EXPECT_EQ(*video_server::video_pixel_format_from_string("GRAY10"), VideoPixelFormat::GRAY10LE);
    ASSERT_TRUE(video_server::video_pixel_format_from_string("gray12le").has_value());
    EXPECT_EQ(*video_server::video_pixel_format_from_string("gray12le"), VideoPixelFormat::GRAY12LE);
    ASSERT_TRUE(video_server::video_pixel_format_from_string("gray_16").has_value());
    EXPECT_EQ(*video_server::video_pixel_format_from_string("gray_16"), VideoPixelFormat::GRAY16LE);
    ASSERT_TRUE(video_server::video_pixel_format_from_string("nv12").has_value());
    EXPECT_EQ(*video_server::video_pixel_format_from_string("nv12"), VideoPixelFormat::NV12);
    ASSERT_TRUE(video_server::video_pixel_format_from_string("i420").has_value());
    EXPECT_EQ(*video_server::video_pixel_format_from_string("i420"), VideoPixelFormat::I420);
    EXPECT_FALSE(video_server::video_pixel_format_from_string("gray14").has_value());
    EXPECT_FALSE(video_server::video_pixel_format_from_string(nullptr).has_value());
}

TEST(VideoTypesTest, MapsPixelFormatsIntoLibavFormats)
{
    using video_server::VideoPixelFormat;

    EXPECT_EQ(video_server::libav_pixel_format_from_video_pixel_format(VideoPixelFormat::RGB24), AV_PIX_FMT_RGB24);
    EXPECT_EQ(video_server::libav_pixel_format_from_video_pixel_format(VideoPixelFormat::BGR24), AV_PIX_FMT_BGR24);
    EXPECT_EQ(video_server::libav_pixel_format_from_video_pixel_format(VideoPixelFormat::RGBA32), AV_PIX_FMT_RGBA);
    EXPECT_EQ(video_server::libav_pixel_format_from_video_pixel_format(VideoPixelFormat::BGRA32), AV_PIX_FMT_BGRA);
    EXPECT_EQ(video_server::libav_pixel_format_from_video_pixel_format(VideoPixelFormat::GRAY8), AV_PIX_FMT_GRAY8);
#ifdef AV_PIX_FMT_GRAY10LE
    EXPECT_EQ(video_server::libav_pixel_format_from_video_pixel_format(VideoPixelFormat::GRAY10LE), AV_PIX_FMT_GRAY10LE);
#else
    EXPECT_FALSE(video_server::libav_pixel_format_from_video_pixel_format(VideoPixelFormat::GRAY10LE).has_value());
#endif
#ifdef AV_PIX_FMT_GRAY12LE
    EXPECT_EQ(video_server::libav_pixel_format_from_video_pixel_format(VideoPixelFormat::GRAY12LE), AV_PIX_FMT_GRAY12LE);
#else
    EXPECT_FALSE(video_server::libav_pixel_format_from_video_pixel_format(VideoPixelFormat::GRAY12LE).has_value());
#endif
#ifdef AV_PIX_FMT_GRAY16LE
    EXPECT_EQ(video_server::libav_pixel_format_from_video_pixel_format(VideoPixelFormat::GRAY16LE), AV_PIX_FMT_GRAY16LE);
#else
    EXPECT_FALSE(video_server::libav_pixel_format_from_video_pixel_format(VideoPixelFormat::GRAY16LE).has_value());
#endif
    EXPECT_EQ(video_server::libav_pixel_format_from_video_pixel_format(VideoPixelFormat::NV12), AV_PIX_FMT_NV12);
    EXPECT_EQ(video_server::libav_pixel_format_from_video_pixel_format(VideoPixelFormat::I420), AV_PIX_FMT_YUV420P);
}
