# Sourced by the KernelSU module installer. Guard the measured binary, not the fingerprint.
[ "$(getprop ro.product.vendor.device)" = "waydroid_x86_64" ] || abort "Requires Waydroid x86_64."
[ "$(getprop ro.build.version.sdk)" = "36" ] || abort "Requires the tested Android 16 image."
sf_sha=$(sha256sum /system/bin/surfaceflinger) || abort "Cannot read SurfaceFlinger."
[ "${sf_sha%% *}" = "2604ff0aa99db34e25c7f40a2ffa2532e4ded00ef689e313f0d57cf8bd2a6080" ] || abort "Untested SurfaceFlinger build."
ui_print "- Applies on next Waydroid container boot. No system files are mounted."
