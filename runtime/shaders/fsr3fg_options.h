// The build options of the FSR 3.1 frame generation passes (runtime/shaders/fsr3fi_*.comp, frame interpolation, and
// runtime/shaders/fsr3of_*.comp, optical flow; recorded by runtime/vk_fsr3fg.c), included after #version, for the
// inputs runtime/vk_dlaa.c hands over: motion vectors at the render size and without the jitter, depth not inverted
// (0 at the near plane). Everything runs in 32-bit floats. The colour of an SDR swapchain image is sRGB-encoded UNORM,
// which the passes take from their constants (backbuffer transfer function 0), not from an option.
#define FFX_GPU 1
#define FFX_GLSL 1

#define FFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS 1
#define FFX_FRAMEINTERPOLATION_OPTION_JITTERED_MOTION_VECTORS 0
#define FFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH 0
