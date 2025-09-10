cmake_minimum_required(VERSION 3.12)

find_package(Git QUIET)

if(Git_FOUND AND EXISTS "${SCRIPT_WORKING_DIR}/../../.git")
    execute_process(COMMAND ${GIT_EXECUTABLE} fetch origin --tags --force WORKING_DIRECTORY ${SCRIPT_WORKING_DIR})
    execute_process(COMMAND ${GIT_EXECUTABLE} describe --tags --exact-match HEAD --exclude=latest --exclude=nightly
                    OUTPUT_VARIABLE FLIPPERMCE_VERSION WORKING_DIRECTORY ${SCRIPT_WORKING_DIR}
                    OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
                    OUTPUT_VARIABLE FLIPPERMCE_COMMIT WORKING_DIRECTORY ${SCRIPT_WORKING_DIR}
                    OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --abbrev-ref HEAD
                    OUTPUT_VARIABLE FLIPPERMCE_BRANCH WORKING_DIRECTORY ${SCRIPT_WORKING_DIR}
                    OUTPUT_STRIP_TRAILING_WHITESPACE)
    string(REGEX REPLACE "\n$" "" FLIPPERMCE_VERSION "${FLIPPERMCE_VERSION}")
    string(REGEX REPLACE "\n$" "" FLIPPERMCE_COMMIT "${FLIPPERMCE_COMMIT}")
    string(REGEX REPLACE "\n$" "" FLIPPERMCE_BRANCH "${FLIPPERMCE_BRANCH}")
    if("${FLIPPERMCE_VERSION}" STREQUAL "")
        set(FLIPPERMCE_VERSION "nightly")
    endif()
else()
    set(FLIPPERMCE_VERSION "None")
    set(FLIPPERMCE_COMMIT "None")
    set(FLIPPERMCE_BRANCH "None")
endif()

file(READ ${SCRIPT_TEMPLATE} template_file)

string(REPLACE "@FLIPPERMCE_VERSION@" ${FLIPPERMCE_VERSION} template_file "${template_file}")
string(REPLACE "@FLIPPERMCE_COMMIT@" ${FLIPPERMCE_COMMIT} template_file "${template_file}")
string(REPLACE "@FLIPPERMCE_BRANCH@" ${FLIPPERMCE_BRANCH} template_file "${template_file}")
string(REPLACE "@VARIANT@" ${VARIANT} template_file "${template_file}")

file(WRITE ${SCRIPT_OUTPUT_FILE} "${template_file}")
