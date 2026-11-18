# NVS credential-reset tool — see platformio.ini and README.md here.
#
# The partition table MUST be the root one (not the 20 KB helper table): see
# the comment at the top of platformio.ini. Copied verbatim from ../../.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(nvs_reset_auth)
