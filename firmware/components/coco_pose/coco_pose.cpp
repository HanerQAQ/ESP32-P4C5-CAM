#include "coco_pose.hpp"
#include "esp_log.h"
#include <filesystem>

#if CONFIG_COCO_POSE_MODEL_IN_FLASH_RODATA
extern const uint8_t coco_pose_espdl[] asm("_binary_coco_pose_espdl_start");
static const char *path = (const char *)coco_pose_espdl;
#elif CONFIG_COCO_POSE_MODEL_IN_FLASH_PARTITION
static const char *path = "coco_pose";
#else
#if !defined(CONFIG_BSP_SD_MOUNT_POINT)
#define CONFIG_BSP_SD_MOUNT_POINT "/sdcard"
#endif
#endif
namespace coco_pose {
Yolo11nPose::Yolo11nPose(const char *model_name, float score_thr, float nms_thr)
{
#if !CONFIG_COCO_POSE_MODEL_IN_SDCARD
    bool param_copy = true;
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) < 1024 * 1024 * 9) {
        param_copy = false;
    }
    m_model = new dl::Model(path,
                            model_name,
                            static_cast<fbs::model_location_type_t>(CONFIG_COCO_POSE_MODEL_LOCATION),
                            0,
                            dl::MEMORY_MANAGER_GREEDY,
                            nullptr,
                            param_copy);
#else
    auto sd_path = std::filesystem::path(CONFIG_BSP_SD_MOUNT_POINT) / CONFIG_COCO_POSE_MODEL_SDCARD_DIR / model_name;
    m_model = new dl::Model(sd_path.c_str(), fbs::MODEL_LOCATION_IN_SDCARD);
#endif
    m_model->minimize();
    m_image_preprocessor = new dl::image::ImagePreprocessor(m_model, {0, 0, 0}, {255, 255, 255});
    m_image_preprocessor->enable_letterbox({114, 114, 114});
    m_postprocessor = new dl::detect::yolo11posePostProcessor(
        m_model, m_image_preprocessor, score_thr, nms_thr, 10, {{8, 8, 4, 4}, {16, 16, 8, 8}, {32, 32, 16, 16}});
}

} // namespace coco_pose

COCOPose::COCOPose(model_type_t model_type, bool lazy_load) : m_model_type(model_type)
{
    switch (model_type) {
    case model_type_t::YOLO11N_POSE_S8_V1:
    case model_type_t::YOLO11N_POSE_S8_V2:
    case model_type_t::YOLO11N_POSE_320_S8_V2:
    case model_type_t::YOLO11N_POSE_320_P4_V3:
    case model_type_t::YOLO11N_POSE_256_P4:
    case model_type_t::YOLO11N_POSE_224_P4:
    case model_type_t::YOLO11N_POSE_224_P4_TRAINED:
    case model_type_t::YOLO11N_POSE_224_P4_NEW:
    case model_type_t::YOLO11N_POSE_224_P4_CUSTOM:
        m_score_thr[0] = coco_pose::Yolo11nPose::default_score_thr;
        m_nms_thr[0] = coco_pose::Yolo11nPose::default_nms_thr;
        break;
    }
    if (lazy_load) {
        m_model = nullptr;
    } else {
        load_model();
    }
}

void COCOPose::load_model()
{
    switch (m_model_type) {
    case model_type_t::YOLO11N_POSE_S8_V1:
#if CONFIG_COCO_POSE_YOLO11N_POSE_S8_V1 || CONFIG_COCO_POSE_MODEL_IN_SDCARD
        m_model = new coco_pose::Yolo11nPose("coco_pose_yolo11n_pose_s8_v1.espdl", m_score_thr[0], m_nms_thr[0]);
#else
        ESP_LOGE("coco_pose", "coco_pose_yolo11n_pose_s8_v1 is not selected in menuconfig.");
#endif
        break;
    case model_type_t::YOLO11N_POSE_S8_V2:
#if CONFIG_COCO_POSE_YOLO11N_POSE_S8_V2 || CONFIG_COCO_POSE_MODEL_IN_SDCARD
        m_model = new coco_pose::Yolo11nPose("coco_pose_yolo11n_pose_s8_v2.espdl", m_score_thr[0], m_nms_thr[0]);
#else
        ESP_LOGE("coco_pose", "coco_pose_yolo11n_pose_s8_v2 is not selected in menuconfig.");
#endif
        break;
    case model_type_t::YOLO11N_POSE_320_S8_V2:
#if CONFIG_COCO_POSE_YOLO11N_POSE_320_S8_V2 || CONFIG_COCO_POSE_MODEL_IN_SDCARD
        m_model = new coco_pose::Yolo11nPose("coco_pose_yolo11n_pose_320_s8_v2.espdl", m_score_thr[0], m_nms_thr[0]);
#else
        ESP_LOGE("coco_pose", "coco_pose_yolo11n_pose_320_s8_v2 is not selected in menuconfig.");
#endif
        break;
    case model_type_t::YOLO11N_POSE_320_P4_V3:
#if CONFIG_COCO_POSE_YOLO11N_POSE_320_P4_V3 || CONFIG_COCO_POSE_MODEL_IN_SDCARD
        m_model = new coco_pose::Yolo11nPose("coco_pose_yolo11n_pose_320_p4_v3.espdl", m_score_thr[0], m_nms_thr[0]);
#else
        ESP_LOGE("coco_pose", "coco_pose_yolo11n_pose_320_p4_v3 is not selected in menuconfig.");
#endif
        break;
    case model_type_t::YOLO11N_POSE_256_P4:
#if CONFIG_COCO_POSE_YOLO11N_POSE_256_P4 || CONFIG_COCO_POSE_MODEL_IN_SDCARD
        m_model = new coco_pose::Yolo11nPose("coco_pose_yolo11n_pose_256_p4.espdl", m_score_thr[0], m_nms_thr[0]);
#else
        ESP_LOGE("coco_pose", "coco_pose_yolo11n_pose_256_p4 is not selected in menuconfig.");
#endif
        break;
    case model_type_t::YOLO11N_POSE_224_P4:
#if CONFIG_COCO_POSE_YOLO11N_POSE_224_P4 || CONFIG_COCO_POSE_MODEL_IN_SDCARD
        m_model = new coco_pose::Yolo11nPose("coco_pose_yolo11n_pose_224_p4.espdl", m_score_thr[0], m_nms_thr[0]);
#else
        ESP_LOGE("coco_pose", "coco_pose_yolo11n_pose_224_p4 is not selected in menuconfig.");
#endif
        break;
    case model_type_t::YOLO11N_POSE_224_P4_TRAINED:
#if CONFIG_COCO_POSE_YOLO11N_POSE_224_P4_TRAINED || CONFIG_COCO_POSE_MODEL_IN_SDCARD
        m_model = new coco_pose::Yolo11nPose("coco_pose_yolo11n_pose_224_p4_trained.espdl", m_score_thr[0], m_nms_thr[0]);
#else
        ESP_LOGE("coco_pose", "coco_pose_yolo11n_pose_224_p4_trained is not selected in menuconfig.");
#endif
        break;
    case model_type_t::YOLO11N_POSE_224_P4_NEW:
#if CONFIG_COCO_POSE_YOLO11N_POSE_224_P4_NEW || CONFIG_COCO_POSE_MODEL_IN_SDCARD
        m_model = new coco_pose::Yolo11nPose("coco_pose_yolo11n_pose_224_p4.espdl", m_score_thr[0], m_nms_thr[0]);
#else
        ESP_LOGE("coco_pose", "coco_pose_yolo11n_pose_224_p4_new is not selected in menuconfig.");
#endif
        break;
    case model_type_t::YOLO11N_POSE_224_P4_CUSTOM:
#if CONFIG_COCO_POSE_YOLO11N_POSE_224_P4_CUSTOM || CONFIG_COCO_POSE_MODEL_IN_SDCARD
        m_model = new coco_pose::Yolo11nPose("coco_pose_yolo11n_pose_224_p4.espdl", m_score_thr[0], m_nms_thr[0]);
#else
        ESP_LOGE("coco_pose", "coco_pose_yolo11n_pose_224_p4_custom is not selected in menuconfig.");
#endif
        break;
    }
}