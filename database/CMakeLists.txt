
set(GAMEDB_GC_OBJ "${CMAKE_CURRENT_BINARY_DIR}/gamedbps2.o")

add_library(gamedb INTERFACE)
add_custom_target(gamedbobjs_gc ALL
                    COMMAND ${CMAKE_COMMAND}
                        -D OUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}
                        -D PYTHON_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/parse_gc.py
                        -D CMAKE_OBJCOPY=${CMAKE_OBJCOPY}
                        -D SYSTEM=gc
                        -P ${CMAKE_CURRENT_SOURCE_DIR}/db_obj_builder.cmake
                    VERBATIM
                    BYPRODUCTS ${GAMEDB_PS2_OBJ})

add_dependencies(gamedb gamedbobjs_gc)

target_link_libraries(gamedb INTERFACE ${GAMEDB_GC_OBJ})
