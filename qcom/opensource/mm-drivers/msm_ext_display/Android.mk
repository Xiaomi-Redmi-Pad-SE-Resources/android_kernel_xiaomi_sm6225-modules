LOCAL_PATH := $(call my-dir)
LOCAL_MODULE_DDK_BUILD := true
include $(CLEAR_VARS)

BOARD_OPENSOURCE_DIR ?= vendor/qcom/opensource
BOARD_COMMON_DIR ?= device/qcom/common

# This makefile is only for DLKM
ifneq ($(findstring vendor,$(LOCAL_PATH)),)

ifneq ($(findstring opensource,$(LOCAL_PATH)),)
	MSM_EXT_DISPLAY_BLD_DIR := $(TOP)/$(BOARD_OPENSOURCE_DIR)/mm-drivers/msm_ext_display
endif # opensource

DLKM_DIR := $(TOP)/$(BOARD_COMMON_DIR)/dlkm

LOCAL_ADDITIONAL_DEPENDENCIES := $(wildcard $(LOCAL_PATH)/**/*) $(wildcard $(LOCAL_PATH)/*)

###########################################################
# This is set once per LOCAL_PATH, not per (kernel) module
KBUILD_OPTIONS := MSM_EXT_DISPLAY_ROOT=$(MSM_EXT_DISPLAY_BLD_DIR)
KBUILD_OPTIONS += MODNAME=msm_ext_display
KBUILD_OPTIONS += BOARD_PLATFORM=$(TARGET_BOARD_PLATFORM)

###########################################################
include $(CLEAR_VARS)
# For incremental compilation
LOCAL_SRC_FILES           := $(wildcard $(LOCAL_PATH)/**/*) $(wildcard $(LOCAL_PATH)/*)
LOCAL_MODULE              := msm-ext-disp-module-symvers
LOCAL_MODULE_STEM         := Module.symvers
LOCAL_MODULE_KBUILD_NAME  := Module.symvers
LOCAL_MODULE_PATH         := $(KERNEL_MODULES_OUT)

include $(DLKM_DIR)/Build_external_kernelmodule.mk
###########################################################
include $(CLEAR_VARS)
LOCAL_SRC_FILES   := $(wildcard $(LOCAL_PATH)/**/*) $(wildcard $(LOCAL_PATH)/*)
LOCAL_MODULE              := msm_ext_display.ko
LOCAL_MODULE_KBUILD_NAME  := msm_ext_display.ko
LOCAL_MODULE_TAGS         := optional
LOCAL_MODULE_DEBUG_ENABLE := true
LOCAL_MODULE_PATH         := $(KERNEL_MODULES_OUT)

include $(DLKM_DIR)/Build_external_kernelmodule.mk
###########################################################
endif # DLKM check
