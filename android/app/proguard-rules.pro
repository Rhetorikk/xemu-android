# Keep SDL3 Java glue accessible from native.
-keep class org.libsdl.app.** { *; }
-keepclassmembers class org.libsdl.app.** { *; }

# Keep our JNI surface.
-keep class app.xemu.XemuNative { *; }
-keepclasseswithmembernames class * {
    native <methods>;
}
