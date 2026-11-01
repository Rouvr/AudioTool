# ============================================================================
# Rouvr AudioTool — Standard Makefile (Compatible with GNU make & MSVC nmake)
# ============================================================================

all: build

build:
	powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1

build-nocmt:
	powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1 -SkipCommit

test:
	"C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe" --test-dir build --output-on-failure -V

run:
	build\bin\at_engine_poc.exe

devices:
	build\bin\at_engine_poc.exe --list-devices

clean:
	powershell -NoProfile -Command "if (Test-Path build) { Remove-Item -Recurse -Force build }"
