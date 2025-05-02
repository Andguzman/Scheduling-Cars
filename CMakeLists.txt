cmake_minimum_required(VERSION 3.10)
project(RoadSim C)

# Find pkg-config
find_package(PkgConfig REQUIRED)

# Find GTK3
pkg_check_modules(GTK3 REQUIRED gtk+-3.0)

# Create the executable
add_executable(Cars Cars.c)

# Add the GTK3 include directories directly to the target
target_include_directories(Cars PRIVATE ${GTK3_INCLUDE_DIRS})

# Add compile options
target_compile_options(Cars PRIVATE ${GTK3_CFLAGS_OTHER})

# Link against GTK3 libraries and pthread
target_link_libraries(Cars PRIVATE ${GTK3_LIBRARIES} pthread)

# Output debug information
message(STATUS "GTK3_INCLUDE_DIRS: ${GTK3_INCLUDE_DIRS}")
message(STATUS "GTK3_LIBRARIES: ${GTK3_LIBRARIES}")
message(STATUS "GTK3_LIBRARY_DIRS: ${GTK3_LIBRARY_DIRS}")