@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 exit /b 1
if not exist "%~dp0..\build\tests" mkdir "%~dp0..\build\tests"
cl /nologo /std:c++20 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /Fe:"%~dp0..\build\tests\NetworkTests.exe" /Fo:"%~dp0..\build\tests\NetworkTests.obj" "%~dp0NetworkTests.cpp"
if errorlevel 1 exit /b 1
"%~dp0..\build\tests\NetworkTests.exe"
exit /b %errorlevel%
