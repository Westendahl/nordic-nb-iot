# Install script for directory: /home/thomas/Documents/UA/Doctoraat/projects/nb-iot-multiple-bs/ncs/nrf/drivers

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "/usr/local")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "TRUE")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for each subdirectory.
  include("/home/thomas/Documents/UA/Doctoraat/projects/nb-iot-multiple-bs/ncs/nrf/samples/nrf9160/serial_lte_modem/build/zephyr/modules/nrf/drivers/at_cmd/cmake_install.cmake")
  include("/home/thomas/Documents/UA/Doctoraat/projects/nb-iot-multiple-bs/ncs/nrf/samples/nrf9160/serial_lte_modem/build/zephyr/modules/nrf/drivers/lte_link_control/cmake_install.cmake")
  include("/home/thomas/Documents/UA/Doctoraat/projects/nb-iot-multiple-bs/ncs/nrf/samples/nrf9160/serial_lte_modem/build/zephyr/modules/nrf/drivers/net/cmake_install.cmake")
  include("/home/thomas/Documents/UA/Doctoraat/projects/nb-iot-multiple-bs/ncs/nrf/samples/nrf9160/serial_lte_modem/build/zephyr/modules/nrf/drivers/flash_patch/cmake_install.cmake")
  include("/home/thomas/Documents/UA/Doctoraat/projects/nb-iot-multiple-bs/ncs/nrf/samples/nrf9160/serial_lte_modem/build/zephyr/modules/nrf/drivers/entropy/cmake_install.cmake")

endif()

