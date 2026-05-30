specific_ss.add(files(
	'pgraph.c',
	'profile.c',
	'rdi.c',
	's3tc.c',
	'swizzle.c',
	'texture.c',
	'vertex.c',
	))
if have_renderdoc
	specific_ss.add(files('debug_renderdoc.c'))
endif
subdir('thirdparty')
subdir('null')
# pgraph/gl is the OpenGL renderer backend; Android is Vulkan-only and we
# stub gloffscreen, so the GL sources can't compile.
if host_os != 'android'
  subdir('gl')
endif
subdir('glsl')
subdir('vk')
specific_ss.add(nv2a_vsh_cpu)
