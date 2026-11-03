file(GLOB_RECURSE PKG_BINARIES RELATIVE "${CPACK_TEMPORARY_DIRECTORY}" "${CPACK_TEMPORARY_DIRECTORY}/bin/*"
        "${CPACK_TEMPORARY_DIRECTORY}/lib/*.so*")

execute_process(COMMAND ${CMAKE_COMMAND} -E chdir "${CPACK_TEMPORARY_DIRECTORY}" tar cfvz
        "${CPACK_PACKAGE_DIRECTORY}/${CPACK_PACKAGE_FILE_NAME}_dbgsym.tar.gz" -- ${PKG_BINARIES})

# --strip-debug leaves the code and the function names, so the app's crash record
# (src/app/crash_recorder.c) can still name what crashed.
foreach (PKG_BINARY ${PKG_BINARIES})
    execute_process(COMMAND "${CPACK_WEBOS_STRIP_COMMAND}" --strip-debug ${PKG_BINARY} WORKING_DIRECTORY "${CPACK_TEMPORARY_DIRECTORY}"
            COMMAND_ERROR_IS_FATAL ANY)
endforeach ()