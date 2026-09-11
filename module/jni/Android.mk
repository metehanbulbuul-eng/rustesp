LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := zygisk
LOCAL_SRC_FILES := main.cpp il2cpp.cpp gl.cpp
LOCAL_CXXFLAGS := -std=c++17 -O2 -fvisibility=hidden -fvisibility-inlines-hidden -Wall -Wno-unused-variable -Wno-unused-function
LOCAL_LDFLAGS := -Wl,--gc-sections -Wl,--exclude-libs,ALL
LOCAL_LDLIBS := -llog -lEGL -lGLESv3 -landroid -ldl
include $(BUILD_SHARED_LIBRARY)