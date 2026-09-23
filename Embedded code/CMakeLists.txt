# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Flipper-side integration lives in flipper/. Its ESP32 half is an ordinary
# IDF component; the Flipper app under flipper/app/ is built separately with
# ufbt and is not part of this build.
set(EXTRA_COMPONENT_DIRS "flipper/esp32")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(proximity_lamp)
