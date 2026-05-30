# Intentionally empty.
#
# The native build is driven by meson via the Gradle `mesonCompile` task in
# app/build.gradle.kts. This file exists only because some IDE integrations
# expect a CMakeLists at this path. It is not referenced by `externalNativeBuild`.
cmake_minimum_required(VERSION 3.22.1)
project(xemu-stub LANGUAGES C)
