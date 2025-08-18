/*
 * Include wrappers for older kernels as interfaces change
 */

#include "config.h"

#ifndef SG_SEGMENT_SZ
#define SG_SEGMENT_SZ	65536
#endif

#ifndef slab_flags_t
typedef unsigned __bitwise slab_flags_t;
#endif

/*
 * Copied kmem_cache_create_usercopy() from scst project
 */
#if !defined(HAVE_KMEM_CACHE_CREATE_USERCOPY)
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 23)
static inline struct kmem_cache *kmem_cache_create_usercopy(const char *name,
			unsigned int size, unsigned int align,
			slab_flags_t flags,
			unsigned int useroffset, unsigned int usersize,
			void (*ctor)(void *))
{
	return kmem_cache_create(name, size, align, flags, ctor, NULL);
}
#elif LINUX_VERSION_CODE < KERNEL_VERSION(4, 16, 0)
static inline struct kmem_cache *kmem_cache_create_usercopy(const char *name,
			unsigned int size, unsigned int align,
			slab_flags_t flags,
			unsigned int useroffset, unsigned int usersize,
			void (*ctor)(void *))
{
	return kmem_cache_create(name, size, align, flags, ctor);
}
#endif
#endif

#if !defined(HAVE_FILE_INODE)
/*
 * See also patch "new helper: file_inode(file)" (commit ID
 * 496ad9aa8ef448058e36ca7a787c61f2e63f0f54).
 */
static inline struct inode *file_inode(struct file *f)
{
	return f->f_path.dentry->d_inode;
}
#endif

#if !defined(HAVE_SYSFS_EMIT)
/* https://patches.linaro.org/project/stable/patch/20210305120853.392925382@linuxfoundation.org/ */
/**
 *	sysfs_emit - scnprintf equivalent, aware of PAGE_SIZE buffer.
 *	@buf:	start of PAGE_SIZE buffer.
 *	@fmt:	format
 *	@...:	optional arguments to @format
 *
 *
 * Returns number of characters written to @buf.
 */
static int sysfs_emit(char *buf, const char *fmt, ...)
{
	va_list args;
	int len;

	if (WARN(!buf || offset_in_page(buf),
		 "invalid sysfs_emit: buf:%p\n", buf))
		return 0;

	va_start(args, fmt);
	len = vscnprintf(buf, PAGE_SIZE, fmt, args);
	va_end(args);

	return len;
}
#endif

#if !defined(USE_TIMER_DELETE_NOT_DEL_TIMER)
#define timer_delete_sync del_timer_sync
#endif

/*
 * 6.16 kernel change, from "from_timer()" to "timer_container_of()" in timer.h.
 */
#if !defined(FROM_TIMER_NOW_TIMER_CONTAINER_OF)
#define timer_container_of	from_timer
#endif
