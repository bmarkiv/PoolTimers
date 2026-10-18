idf_component_register(
    SRCS "../main.cpp"
    INCLUDE_DIRS ".."
    REQUIRES ESP32WifiManager cjson nvs_flash esp_driver_gpio esp_timer
)

set(PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(WEB_HEADER "${PROJECT_ROOT}/index_html_gz.h")
add_custom_command(
    OUTPUT "${WEB_HEADER}"
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_ROOT}/generate_header_for_index_html.py"
    DEPENDS
        "${PROJECT_ROOT}/generate_header_for_index_html.py"
        "${PROJECT_ROOT}/index.html"
        "${PROJECT_ROOT}/index_style.css"
        "${PROJECT_ROOT}/index_script.js"
    WORKING_DIRECTORY "${PROJECT_ROOT}"
    VERBATIM
)
add_custom_target(generate_web_header DEPENDS "${WEB_HEADER}")
add_dependencies(${COMPONENT_LIB} generate_web_header)