LOCAL_PATH:= $(call my-dir)

ifeq ($(WPA_SUPPLICANT_VERSION),VER_2_1_DEVEL_WCS)
include $(call all-subdir-makefiles)
endif
