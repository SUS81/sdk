macro(load_sdklib_libraries)

    find_package(PkgConfig REQUIRED) # For libraries loaded using pkg-config

    if(VCPKG_ROOT)
        find_package(cryptopp CONFIG REQUIRED)
        target_link_libraries(SDKlib PRIVATE cryptopp::cryptopp)

        find_package(unofficial-sodium REQUIRED)
        if(WIN32)
            target_link_libraries(SDKlib PRIVATE unofficial-sodium::sodium)
        else()
            target_link_libraries(SDKlib PRIVATE unofficial-sodium::sodium unofficial-sodium::sodium_config_public)
        endif()

        find_package(unofficial-sqlite3 REQUIRED)
        target_link_libraries(SDKlib PRIVATE unofficial::sqlite3::sqlite3)

        find_package(CURL REQUIRED)
        target_link_libraries(SDKlib PRIVATE CURL::libcurl)

        find_package(ICU COMPONENTS uc data REQUIRED)
        target_link_libraries(SDKlib PRIVATE ICU::uc ICU::data)

        if(USE_OPENSSL)
            find_package(OpenSSL REQUIRED)
            target_link_libraries(SDKlib PRIVATE OpenSSL::SSL OpenSSL::Crypto)
        endif()

        if(USE_MEDIAINFO)
            # MediaInfo is not setting libzen dependency correctly. Preload it.
            find_package(ZenLib CONFIG REQUIRED)
            target_link_libraries(SDKlib PRIVATE zen)

            find_package(MediaInfoLib REQUIRED)
            target_link_libraries(SDKlib PRIVATE mediainfo)
        endif()

        if(USE_FREEIMAGE)
            find_package(freeimage REQUIRED)
            target_link_libraries(SDKlib PRIVATE freeimage::FreeImage)
        endif()

        if(USE_FFMPEG)
            find_package(FFMPEG REQUIRED)
            target_include_directories(SDKlib PRIVATE ${FFMPEG_INCLUDE_DIRS})
            target_link_directories(SDKlib PRIVATE ${FFMPEG_LIBRARY_DIRS})
            target_link_libraries(SDKlib PRIVATE ${FFMPEG_LIBRARIES})
            set(HAVE_FFMPEG 1)
        endif()

        if(USE_LIBUV)
            find_package(libuv REQUIRED)
            target_link_libraries(SDKlib PRIVATE $<IF:$<TARGET_EXISTS:libuv::uv_a>,libuv::uv_a,libuv::uv>)
            set(HAVE_LIBUV 1)
        endif()

        if(USE_PDFIUM)
            find_package(pdfium REQUIRED)
            target_link_libraries(SDKlib PRIVATE PDFIUM::pdfium)
            set(HAVE_PDFIUM 1)
        endif()

        if(USE_C_ARES)
            find_package(c-ares REQUIRED)
            target_link_libraries(SDKlib PRIVATE c-ares::cares)
            set(MEGA_USE_C_ARES 1)
        endif()

        if(USE_READLINE)
            pkg_check_modules(readline REQUIRED IMPORTED_TARGET readline)
            target_link_libraries(SDKlib PRIVATE PkgConfig::readline)
        else()
            set(NO_READLINE 1)
        endif()

    else() # No VCPKG usage. Use pkg-config
        pkg_check_modules(cryptopp REQUIRED IMPORTED_TARGET libcrypto++)
        target_link_libraries(SDKlib PRIVATE PkgConfig::cryptopp)

        pkg_check_modules(sodium REQUIRED IMPORTED_TARGET libsodium)
        target_link_libraries(SDKlib PRIVATE PkgConfig::sodium)

        pkg_check_modules(sqlite3 REQUIRED IMPORTED_TARGET sqlite3)
        target_link_libraries(SDKlib PRIVATE PkgConfig::sqlite3)

        pkg_check_modules(curl REQUIRED IMPORTED_TARGET libcurl)
        target_link_libraries(SDKlib PRIVATE PkgConfig::curl)

        find_package(ICU COMPONENTS uc data REQUIRED)
        target_link_libraries(SDKlib PRIVATE ICU::uc ICU::data)

        if(USE_OPENSSL)
            find_package(OpenSSL REQUIRED)
            target_link_libraries(SDKlib PRIVATE OpenSSL::SSL OpenSSL::Crypto)
        endif()

        if(USE_MEDIAINFO)
            pkg_check_modules(mediainfo REQUIRED IMPORTED_TARGET libmediainfo)
            target_link_libraries(SDKlib PRIVATE PkgConfig::mediainfo)
        endif()

        if(USE_FREEIMAGE)
            # FreeImage has no pkg-config file. Use out own FindFreeImage.cmake to find the library.
            find_package(FreeImage REQUIRED)
            target_link_libraries(SDKlib PRIVATE FreeImage::FreeImage)
        endif()

        if(USE_FFMPEG)
            pkg_check_modules(ffmpeg REQUIRED IMPORTED_TARGET libavformat libavutil libavcodec libswscale libswresample)
            target_link_libraries(SDKlib PRIVATE PkgConfig::ffmpeg)
            set(HAVE_FFMPEG 1)
        endif()

        if(USE_LIBUV)
            pkg_check_modules(uv REQUIRED IMPORTED_TARGET libuv)
            target_link_libraries(SDKlib PRIVATE PkgConfig::uv)
            set(HAVE_LIBUV 1)
        endif()

        if(USE_PDFIUM)
            pkg_check_modules(pdfium REQUIRED IMPORTED_TARGET pdfium)
            target_link_libraries(SDKlib PRIVATE PkgConfig::pdfium)
            set(HAVE_PDFIUM 1)
        endif()

        if(USE_C_ARES)
            pkg_check_modules(cares REQUIRED IMPORTED_TARGET libcares)
            target_link_libraries(SDKlib PRIVATE PkgConfig::cares)
            set(MEGA_USE_C_ARES 1)
        endif()

        if(USE_READLINE)
            pkg_check_modules(readline REQUIRED IMPORTED_TARGET readline)
            target_link_libraries(SDKlib PRIVATE PkgConfig::readline)
        else()
            set(NO_READLINE 1)
        endif()

    endif()

endmacro()
