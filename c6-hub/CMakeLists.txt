cmake_minimum_required(VERSION 3.16)

# Pin PROJECT_VER to the short git SHA so esp_app_desc_t.version is
# always exactly that — git describe falls back to the nearest tag
# otherwise, which entangles fw-* release tags into future builds.
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    OUTPUT_VARIABLE GIT_SHORT_SHA
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
execute_process(
    COMMAND git status --porcelain
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    OUTPUT_VARIABLE GIT_DIRTY
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(GIT_DIRTY)
    set(PROJECT_VER "${GIT_SHORT_SHA}-dirty")
else()
    set(PROJECT_VER "${GIT_SHORT_SHA}")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(c6-hub)
