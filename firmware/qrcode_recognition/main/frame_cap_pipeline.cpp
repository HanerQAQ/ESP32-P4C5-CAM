#include "frame_cap_pipeline.hpp"
#include "who_cam.hpp"

using namespace who::cam;
using namespace who::frame_cap;

#define MODEL_TIME 1


WhoFrameCap *get_lcd_mipi_csi_ppa_frame_cap_pipeline(WhoFrameCapNode **lcd_disp_frame_cap_node)
{
    // Use ppa to resize the frame into model input shape to avoid doing this in the model inference which can reduce
    // cpu load. Compared to the one without ppa, the cam_fb_count must increase, because the process time since
    // WhoFetchNode equals ppa + detect, while without ppa, the process time equals detect.
    auto cam = new WhoP4Cam(V4L2_PIX_FMT_RGB565, MODEL_TIME + 4);
    auto frame_cap = new WhoFrameCap();
    frame_cap->add_node<WhoFetchNode>("FrameCapFetch", cam);
    frame_cap->add_node<WhoPPAResizeNode>(
        "FrameCapPPAResizeLCD", 320, 240, dl::image::DL_IMAGE_PIX_TYPE_RGB565LE, 5);

    *lcd_disp_frame_cap_node = frame_cap->get_node("FrameCapPPAResizeLCD");
    return frame_cap;
}