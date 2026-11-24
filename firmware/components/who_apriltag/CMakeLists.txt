set(include_dirs    .)

set(priv_include_dirs
    third_party/openmv_apriltag
    third_party/openmv_apriltag/common)

set(requires who_frame_cap bsp)

set(openmv_apriltag_srcs
    who_apriltag.cpp
    third_party/openmv_apriltag/apriltag.c
    third_party/openmv_apriltag/apriltag_quad_thresh.c
    third_party/openmv_apriltag/tag16h5.c
    third_party/openmv_apriltag/tag36h11.c
    third_party/openmv_apriltag/common/g2d.c
    third_party/openmv_apriltag/common/homography.c
    third_party/openmv_apriltag/common/image_u8.c
    third_party/openmv_apriltag/common/image_u8x3.c
    third_party/openmv_apriltag/common/matd.c
    third_party/openmv_apriltag/common/pnm.c
    third_party/openmv_apriltag/common/string_util.c
    third_party/openmv_apriltag/common/svd22.c
    third_party/openmv_apriltag/common/time_util.c
    third_party/openmv_apriltag/common/unionfind.c
    third_party/openmv_apriltag/common/workerpool.c
    third_party/openmv_apriltag/common/zarray.c
    third_party/openmv_apriltag/common/zhash.c
    third_party/openmv_apriltag/common/zmaxheap.c)

idf_component_register(
    SRCS ${openmv_apriltag_srcs}
    INCLUDE_DIRS ${include_dirs}
    PRIV_INCLUDE_DIRS ${priv_include_dirs}
    REQUIRES ${requires})

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error -w)
