cmake_minimum_required(VERSION 3.0)
project(mongoose)
find_package(Threads)

option(EXAMPLES
    "Compile examples" OFF)

set(SOURCES
    mongoose.c
)

# Adding dl
# set(EXTRA_LIBS ${EXTRA_LIBS} dl)

# Adding sockets for Win32
if(WIN32)
    set(EXTRA_LIBS ${EXTRA_LIBS} ws2_32)
endif(WIN32)

# Compiling library
add_library(_mongoose ${SOURCES})
target_link_libraries(_mongoose ${EXTRA_LIBS} ${CMAKE_THREAD_LIBS_INIT})

include_directories("${PROJECT_SOURCE_DIR}")

# Compiling tests
if(EXAMPLES)
    add_executable(hello examples/hello.c)
    target_link_libraries(hello _mongoose)

    add_executable(post examples/post.c)
    target_link_libraries(post _mongoose)

    add_executable(chat examples/chat.c)
    target_link_libraries(chat _mongoose)
endif(EXAMPLES)