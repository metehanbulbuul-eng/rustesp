LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := rustesp
LOCAL_SRC_FILES := main.cpp il2cpp.cpp gl.cpp
LOCAL_C_INCLUDES := $(LOCAL_PATH)
LOCAL_CXXFLAGS := -std=c++17 -O2 -fvisibility=hidden -fvisibility-inlines-hidden -Wall
LOCAL_LDLIBS := -llog -lEGL -lGLESv3 -landroid -ldl
include $(BUILD_SHARED_LIBRARY)