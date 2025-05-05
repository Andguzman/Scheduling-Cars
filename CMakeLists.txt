cmake_minimum_required(VERSION 3.10)
project(CEThreads_Project C)

# Set C standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Set compiler flags
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -g")

# Source files for the CEThreads library
set(CETHREADS_SOURCES
        CEThreads.c
)

# Source files for the main application
set(MAIN_SOURCES
        main.c
)

# Create the CEThreads library
add_library(CEThreads STATIC ${CETHREADS_SOURCES})

# Specify include directories
target_include_directories(CEThreads PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Create the main executable
add_executable(main ${MAIN_SOURCES})

# Link the main executable with the CEThreads library
target_link_libraries(main PRIVATE CEThreads)

# Check for pthread and link if found
find_package(Threads)
if(CMAKE_THREAD_LIBS_INIT)
    target_link_libraries(CEThreads PUBLIC ${CMAKE_THREAD_LIBS_INIT})
endif()

# Add custom target for easy testing
add_custom_target(run
        COMMAND main
        DEPENDS main
        WORKING_DIRECTORY ${CMAKE_PROJECT_DIR}
)

# Optional: Install targets
install(TARGETS CEThreads
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
)

install(FILES CEThreads.h
        DESTINATION include
)