@echo off
cmake.exe -Wno-dev ^
  -G "Visual Studio 18 2026" ^
  -A "Win32" ^
  -D MYSQL_INCLUDE_DIR=include\mysql\5.7.30\ ^
  -D MYSQL_LIBRARY=include\mysql\5.7.30\libmysql.lib ^
  -D WITH_MYSQL=true ^
  -D WITH_ANSI=false ^
  -D ZLIB_ROOT=include\zlib\1.2.11\ ^
  -D ZLIB_INCLUDE_DIR=include\zlib\1.2.11\ ^
  -D ZLIB_LIBRARY=include\zlib\1.2.11\zdll.lib ^
  -D CURL_INCLUDE_DIR=include\curl\7.74.0\ ^
  -D CURL_LIBRARY=include\curl\7.74.0\libcurl.lib ^
  -D CMAKE_CXX_FLAGS_RELEASE="/MT /Od" ^
  -D CMAKE_CONFIGURATION_TYPES="Release" ^
  -D CMAKE_SUPPRESS_REGENERATION=true ^
  -D WITH_WIN32_GUI=true ^
  -Hsource\ ^
  -Bbuild\

if %ERRORLEVEL% neq 0 (
    echo CMake failed with error code %ERRORLEVEL%
    pause
    exit /b %ERRORLEVEL%
)

echo CMake completed successfully!
pause