cmake_minimum_required(VERSION 2.7)

project(selx)

if (WIN32)
	file(GLOB_RECURSE SELX_OS_SOURCES "source-code/selx/iocp.cpp")
elseif (UNIX)
	file(GLOB_RECURSE SELX_OS_SOURCES "source-code/selx/epoll.cpp")
endif ()

file(GLOB_RECURSE SELX_HEADERS "source-code/selx/selx.hpp")
file(GLOB_RECURSE SELX_SOURCES "")

add_library(${PROJECT_NAME} STATIC ${SELX_SOURCES} ${SELX_OS_SOURCES})

install(TARGETS ${PROJECT_NAME} DESTINATION lib)
install(FILES ${SELX_HEADERS} DESTINATION include)
