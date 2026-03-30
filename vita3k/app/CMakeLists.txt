add_library(
	app
	STATIC
	include/app/functions.h
	include/app/discord.h
	include/app/state.h
	src/app_init.cpp
	src/app.cpp
	src/game_list.cpp
	src/discord.cpp
	src/user.cpp
)

target_include_directories(app PUBLIC include)
target_link_libraries(app PUBLIC emuenv mem)
if(USE_DISCORD_RICH_PRESENCE)
  target_link_libraries(app PUBLIC discord-rpc)
endif()
target_link_libraries(app PRIVATE audio config display gdbstub io motion ngs renderer SDL3::SDL3 pugixml::pugixml compat packages)
if(WIN32)
	target_link_libraries(app PRIVATE dwmapi)
elseif(ANDROID)
	target_link_libraries(app PRIVATE android adrenotools host::dialog miniz)
endif()

if (LINUX)
    find_package(X11 REQUIRED)
    target_link_libraries(app PRIVATE ${X11_LIBRARIES})
endif()
