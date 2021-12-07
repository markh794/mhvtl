/*
 * Include wrappers for older kernels as interfaces change
 */

#ifndef SG_SEGMENT_SZ
#define SG_SEGMENT_SZ	65536
#endif

#ifndef slab_flags_t
typedef unsigned __bitwise slab_flags_t;
#endif

/*
 * Copied kmem_cache_create_usercopy() from scst project
 */
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

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 9, 0)
/*
 * See also patch "new helper: file_inode(file)" (commit ID
 * 496ad9aa8ef448058e36ca7a787c61f2e63f0f54).
 */
static inline struct inode *file_inode(struct file *f)
{
	return f->f_path.dentry->d_inode;
}
#endif

/* HAVE_UNLOCKED_IOCTL removed in linux/fs.h for kernels 5.9+ */
#if LINUX_VERSION_CODE > KERNEL_VERSION(5, 8, 0)
#define HAVE_UNLOCKED_IOCTL 1
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 14, 0)
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
static sysfs_emit(char *buf, const char *fmt, ...)
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
