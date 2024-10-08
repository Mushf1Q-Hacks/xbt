cmake_minimum_required(VERSION 2.4)
project(xbt-tracker)
set(CMAKE_BUILD_TYPE debug)
# set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_CXX_FLAGS -std=c++11)
set(CPACK_GENERATOR DEB)
set(CPACK_PACKAGE_CONTACT "Olaf van der Spek <olafvdspek@gmail.com>")
set(CPACK_STRIP_FILES true)
include_directories(. ../misc)
include(CheckIncludeFileCXX)
check_include_file_cxx(sys/epoll.h HAVE_SYS_EPOLL)
if(HAVE_SYS_EPOLL)
	add_definitions(-DEPOLL)
endif()
# set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pedantic -Wall -Wextra")
# set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Werror -Wno-implicit-fallthrough -Wno-parentheses -Wno-sign-compare")
add_executable(
	xbt_tracker
	../misc/bt_misc.cpp
	../misc/database.cpp
	../misc/sha1.cpp
	../misc/socket.cpp
	../misc/sql_query.cpp
	../misc/xcc_z.cpp
	config.cpp
	connection.cpp
	epoll.cpp
	server.cpp
	tracker_input.cpp
	transaction.cpp
	"XBT Tracker.cpp"
)
target_link_libraries(xbt_tracker mysqlclient z)
install(TARGETS xbt_tracker DESTINATION bin)
include(CPack)
