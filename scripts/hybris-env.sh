# Environment for libhybris on the M8 (PowerVR GE8300 blob from Android 12, bionic from Halium 13 arm64,
# i18n libs from the runtime apex last in the path).
# Install as /opt/hybris/env.sh
export LD_LIBRARY_PATH=/opt/hybris/lib
export HYBRIS_LD_LIBRARY_PATH=/opt/hybris/android/lib64:/vendor/lib64/egl:/vendor/lib64/hw:/vendor/lib64:/system/lib64:/opt/hybris/android/i18n
