if(WIN32)
	message(STATUS "Uninstalling \"C:/Program Files (x86)/pvpgn\"")
	exec_program(
		"C:/Program Files (x86)/CMake/bin/cmake.exe" ARGS "-E remove_directory \"C:/Program Files (x86)/pvpgn\""
		OUTPUT_VARIABLE rm_out
		RETURN_VALUE rm_retval
	)
	if(NOT "${rm_retval}" STREQUAL 0)
		message(FATAL_ERROR "Problem when removing \"C:/Program Files (x86)/pvpgn\"")
	endif(NOT "${rm_retval}" STREQUAL 0)
else(WIN32)
	if(NOT EXISTS "C:/Users/Administrator/Desktop/pvpgn-server/build/install_manifest.txt")
	  message(FATAL_ERROR "Cannot find install manifest: C:/Users/Administrator/Desktop/pvpgn-server/build/install_manifest.txt")
	endif(NOT EXISTS "C:/Users/Administrator/Desktop/pvpgn-server/build/install_manifest.txt")

	file(READ "C:/Users/Administrator/Desktop/pvpgn-server/build/install_manifest.txt" files)
	string(REGEX REPLACE "\n" ";" files "${files}")
	foreach(file ${files})
	  message(STATUS "Uninstalling $ENV{DESTDIR}${file}")
	  if(IS_SYMLINK "$ENV{DESTDIR}${file}" OR EXISTS "$ENV{DESTDIR}${file}")
		exec_program(
		  "C:/Program Files (x86)/CMake/bin/cmake.exe" ARGS "-E remove \"$ENV{DESTDIR}${file}\""
		  OUTPUT_VARIABLE rm_out
		  RETURN_VALUE rm_retval
		  )
		if(NOT "${rm_retval}" STREQUAL 0)
		  message(FATAL_ERROR "Problem when removing $ENV{DESTDIR}${file}")
		endif(NOT "${rm_retval}" STREQUAL 0)
	  else(IS_SYMLINK "$ENV{DESTDIR}${file}" OR EXISTS "$ENV{DESTDIR}${file}")
		message(STATUS "File $ENV{DESTDIR}${file} does not exist.")
	  endif(IS_SYMLINK "$ENV{DESTDIR}${file}" OR EXISTS "$ENV{DESTDIR}${file}")
	endforeach(file)

	#remove directories
	message(STATUS "Uninstalling \"C:/Program Files (x86)/pvpgn/conf\"")
	exec_program(
		"C:/Program Files (x86)/CMake/bin/cmake.exe" ARGS "-E remove_directory \"C:/Program Files (x86)/pvpgn/conf\""
		OUTPUT_VARIABLE rm_out
		RETURN_VALUE rm_retval
	)
	if(NOT "${rm_retval}" STREQUAL 0)
		message(FATAL_ERROR "Problem when removing \"C:/Program Files (x86)/pvpgn/conf\"")
	endif(NOT "${rm_retval}" STREQUAL 0)
	message(STATUS "Uninstalling \"C:/Program Files (x86)/pvpgn/var\"")
	exec_program(
		"C:/Program Files (x86)/CMake/bin/cmake.exe" ARGS "-E remove_directory \"C:/Program Files (x86)/pvpgn/var\""
		OUTPUT_VARIABLE rm_out
		RETURN_VALUE rm_retval
	)
	if(NOT "${rm_retval}" STREQUAL 0)
		message(FATAL_ERROR "Problem when removing \"C:/Program Files (x86)/pvpgn/var\"")
	endif(NOT "${rm_retval}" STREQUAL 0)
endif(WIN32)
