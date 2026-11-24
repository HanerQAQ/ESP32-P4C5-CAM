#pragma once
#include "dl_image.hpp"
#include "linux/videodev2.h"


namespace who {
namespace cam {
enum class cam_fb_fmt_t { CAM_FB_FMT_RGB565, CAM_FB_FMT_RGB888, CAM_FB_FMT_JPEG, CAM_FB_FMT_UKN };

inline cam_fb_fmt_t dl_pix_fmt2cam_fb_fmt(dl::image::pix_type_t dl_pix_fmt)
{
    switch (dl_pix_fmt) {
    case dl::image::DL_IMAGE_PIX_TYPE_RGB565LE:
    case dl::image::DL_IMAGE_PIX_TYPE_RGB565BE:
        return cam_fb_fmt_t::CAM_FB_FMT_RGB565;
    case dl::image::DL_IMAGE_PIX_TYPE_RGB888:
        return cam_fb_fmt_t::CAM_FB_FMT_RGB888;
    default:
        return cam_fb_fmt_t::CAM_FB_FMT_UKN;
    }
}

inline cam_fb_fmt_t v4l2_fmt2cam_fb_fmt(uint32_t v4l2_fmt)
{
    switch (v4l2_fmt) {
    case V4L2_PIX_FMT_RGB565:
        return cam_fb_fmt_t::CAM_FB_FMT_RGB565;
    case V4L2_PIX_FMT_RGB24:
        return cam_fb_fmt_t::CAM_FB_FMT_RGB888;
    case V4L2_PIX_FMT_MJPEG:
    case V4L2_PIX_FMT_JPEG:
        return cam_fb_fmt_t::CAM_FB_FMT_JPEG;
    default:
        return cam_fb_fmt_t::CAM_FB_FMT_UKN;
    }
}

typedef struct cam_fb_s {
    void *buf;
    size_t len;
    uint16_t width;
    uint16_t height;
    cam_fb_fmt_t format;
    struct timeval timestamp;
    void *ret;
    cam_fb_s() = default;
 
    cam_fb_s(const dl::image::img_t &img, const struct timeval &time)
    {
        buf = img.data;
        len = img.bytes();
        width = img.width;
        height = img.height;
        format = dl_pix_fmt2cam_fb_fmt(img.pix_type);
        timestamp = time;
        ret = nullptr;
    }
    operator dl::image::img_t() const
    {
        return {.data = buf,
                .width = width,
                .height = height,
                .pix_type = format == who::cam::cam_fb_fmt_t::CAM_FB_FMT_RGB565 ? dl::image::DL_IMAGE_PIX_TYPE_RGB565LE
                                                                                : dl::image::DL_IMAGE_PIX_TYPE_RGB888};
    }
} cam_fb_t;

} // namespace cam
} // namespace who
