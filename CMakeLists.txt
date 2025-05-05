cmake_minimum_required(VERSION 3.10)
project(Scheduling_Cars C)

set(CMAKE_C_STANDARD 11)

# Find the required packages
find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
pkg_check_modules(CAIRO REQUIRED cairo)

# Include directories
include_directories(
        ${GTK3_INCLUDE_DIRS}
        ${CAIRO_INCLUDE_DIRS}
        ${CMAKE_CURRENT_SOURCE_DIR}  # For including CEThreads.h
)

# Add link directories
link_directories(
        ${GTK3_LIBRARY_DIRS}
        ${CAIRO_LIBRARY_DIRS}
)

# Add compile options
add_compile_options(
        ${GTK3_CFLAGS_OTHER}
        ${CAIRO_CFLAGS_OTHER}
)

# Define your source files
set(SOURCE_FILES
        main.c
        # Add your other source files here
        # For example:
        CEThreads.c
)

# Create executable
add_executable(${PROJECT_NAME} ${SOURCE_FILES})

# Link libraries
target_link_libraries(${PROJECT_NAME}
        ${GTK3_LIBRARIES}
        ${CAIRO_LIBRARIES}
        # Add any other libraries you need
        m  # Math library (if needed)
)

# If you have any custom CEThreads library that needs compilation
# Uncomment and modify these lines:
# add_library(CEThreads CEThreads.c)
# target_link_libraries(${PROJECT_NAME} CEThreads)