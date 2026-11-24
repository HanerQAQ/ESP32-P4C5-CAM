#pragma once
#include "who_frame_cap.hpp"

#define MODEL_TIME 3
#define MODEL_INPUT_W 224
#define MODEL_INPUT_H 224

who::frame_cap::WhoFrameCap *get_lcd_mipi_csi_ppa_frame_cap_pipeline(
    who::frame_cap::WhoFrameCapNode **lcd_disp_frame_cap_node);
