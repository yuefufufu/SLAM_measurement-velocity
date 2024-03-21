cmake_minimum_required(VERSION 2.8.3)
project(ndt_slam)
find_package(PCL REQUIRED)

set(CMAKE_CXX_STANDARD 14)

IF(NOT (PCL_VERSION VERSION_LESS "1.7.2"))
SET(FAST_PCL_PACKAGES filters registration)
ENDIF(NOT (PCL_VERSION VERSION_LESS "1.7.2"))

find_package(catkin REQUIRED COMPONENTS
  geometry_msgs
  message_generation
  pcl_conversions
  pcl_ros
  roscpp
  sensor_msgs
  std_msgs
  velodyne_pointcloud
  ${FAST_PCL_PACKAGES}
)


generate_messages(
  DEPENDENCIES
  std_msgs
)

###################################
## catkin specific configuration ##
###################################
catkin_package(
  CATKIN_DEPENDS std_msgs velodyne_pointcloud ${FAST_PCL_PACKAGES}
)

###########
## Build ##
###########

include_directories(include ${catkin_INCLUDE_DIRS})

IF(PCL_VERSION VERSION_LESS "1.7.2")
SET(CMAKE_CXX_FLAGS "-std=c++11 -O2 -g -Wall ${CMAKE_CXX_FLAGS}")
ELSE(PCL_VERSION VERSION_LESS "1.7.2")
SET(CMAKE_CXX_FLAGS "-std=c++11 -O2 -g -Wall -DUSE_FAST_PCL ${CMAKE_CXX_FLAGS}")
ENDIF(PCL_VERSION VERSION_LESS "1.7.2")

add_executable(ndt_localize src/ndt_localize.cpp)
add_executable(queue_counter src/queue_counter.cpp)

target_link_libraries(ndt_localize ${catkin_LIBRARIES})
target_link_libraries(queue_counter ${catkin_LIBRARIES})
