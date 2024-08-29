set(RAUC_POC_SRC main.cc)
set(RAUC_POC_HEADERS rauc_poc.h)

find_package(sdbus-c++ REQUIRED)

add_executable(rauc-poc ${RAUC_POC_SRC})
set_target_properties(rauc-poc PROPERTIES
                      CXX_STANDARD 17
                      CXX_EXTENSIONS off)

# See https://github.com/Kistler-Group/sdbus-cpp/blob/master/docs/using-sdbus-c++.md#integrating-sdbus-c-into-your-project
target_link_libraries(rauc-poc PUBLIC aktualizr_lib PRIVATE SDBusCpp::sdbus-c++)

install(TARGETS rauc-poc
        COMPONENT aktualizr
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

add_aktualizr_test(NAME rauc_poc SOURCES rauc_poc_test.cc)
set_target_properties(t_rauc_poc PROPERTIES
                      CXX_STANDARD 17
                      CXX_EXTENSIONS off)

aktualizr_source_file_checks(${RAUC_POC_SRC} ${RAUC_POC_HEADERS} ${TEST_SOURCES})
