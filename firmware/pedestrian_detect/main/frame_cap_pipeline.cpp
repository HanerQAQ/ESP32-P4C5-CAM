#include "frame_cap_pipeline.hpp"
#include "who_cam.hpp"
#include "who_lcd.hpp"

using namespace who::cam;
using namespace who::frame_cap;

#define MODEL_TIME 3
#define MODEL_INPUT_W 224
#define MODEL_INPUT_H 224

WhoFrameCap *get_lcd_mipi_csi_ppa_frame_cap_pipeline(WhoFrameCapNode **lcd_disp_frame_cap_node)
{
    // Use ppa to resize the frame into model input shape to avoid doing this in the model inference which can reduce
    // cpu load. Compared to the one without ppa, the cam_fb_count must increase, because the process time since
    // WhoFetchNode equals ppa + detect, while without ppa, the process time equals detect.
    auto cam = new WhoP4Cam(V4L2_PIX_FMT_RGB565, MODEL_TIME + 2);
    auto frame_cap = new WhoFrameCap();
    frame_cap->add_node<WhoFetchNode>("FrameCapFetch", cam);
    frame_cap->add_node<WhoPPAResizeNode>(
        "FrameCapPPAResizeLCD", BSP_LCD_H_RES, BSP_LCD_V_RES, dl::image::DL_IMAGE_PIX_TYPE_RGB565LE, 1);
    frame_cap->add_node<WhoPPAResizeNode>(
        "FrameCapPPAResize", MODEL_INPUT_W, MODEL_INPUT_H, dl::image::DL_IMAGE_PIX_TYPE_RGB565LE, MODEL_TIME);

    *lcd_disp_frame_cap_node = frame_cap->get_node("FrameCapPPAResizeLCD");
    return frame_cap;
}